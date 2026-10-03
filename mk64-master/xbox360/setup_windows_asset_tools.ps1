param(
    [switch]$SkipTorch
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# This script is intended to work from a fresh MKart360-Universal checkout.
# It prepares:
#   1) w64devkit (GCC + G++ + GNU Make)
#   2) the upstream MK64 native tools source tree when tools/ is missing
#   3) n64graphics, mio0 and tkmk00
#   4) optional Torch support unless -SkipTorch is used

$Repo = Split-Path -Parent $PSScriptRoot
$Tools = Join-Path $Repo 'tools'
$Portable = Join-Path $Tools 'mingw64'
$PortableHost = Join-Path $Tools '_portable_host'
$DownloadDir = Join-Path $Tools '_downloads'
$SevenZip = Join-Path $PortableHost '7zr.exe'

function Say([string]$Text) {
    Write-Host "[MK64] $Text"
}

function Find-PortableRoot {
    if (-not (Test-Path $Portable)) {
        return $null
    }

    $gcc = Get-ChildItem -Path $Portable -Filter gcc.exe -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'bin' } |
        Select-Object -First 1

    if ($gcc) {
        return $gcc.Directory.Parent.FullName
    }

    return $null
}

function Ensure-7Zip {
    if (Test-Path $SevenZip) {
        return $SevenZip
    }

    New-Item -ItemType Directory -Force -Path $PortableHost, $DownloadDir | Out-Null

    Say 'Downloading the standalone 7-Zip extractor...'

    $url = 'https://www.7-zip.org/a/7zr.exe'

    Invoke-WebRequest `
        -Uri $url `
        -OutFile $SevenZip `
        -UseBasicParsing

    if (-not (Test-Path $SevenZip)) {
        throw "7zr.exe could not be downloaded to $SevenZip"
    }

    return $SevenZip
}

function Get-W64DevKit {
    $root = Find-PortableRoot

    if ($root) {
        Say "Portable GCC already present: $root"
        return $root
    }

    New-Item -ItemType Directory -Force -Path $DownloadDir, $Portable | Out-Null

    $headers = @{
        'User-Agent' = 'MK64-Xbox360-AssetSetup'
    }

    Say 'Downloading the current signed w64devkit x64 toolchain from GitHub...'

    $release = Invoke-RestMethod `
        -Headers $headers `
        -Uri 'https://api.github.com/repos/skeeto/w64devkit/releases/latest'

    $asset = $release.assets |
        Where-Object {
            $_.name -match '^w64devkit-x64-.*\.7z\.exe$'
        } |
        Select-Object -First 1

    if (-not $asset) {
        throw 'Could not find the x64 w64devkit release asset.'
    }

    $archive = Join-Path $DownloadDir $asset.name

    if (-not (Test-Path $archive)) {
        Invoke-WebRequest `
            -Headers $headers `
            -Uri $asset.browser_download_url `
            -OutFile $archive `
            -UseBasicParsing
    }

    $seven = Ensure-7Zip

    # w64devkit releases are self-extracting 7-Zip archives.
    # Using 7zr explicitly is more reliable than launching the SFX directly,
    # especially when the project path contains spaces.

    Say "Extracting $($asset.name) with 7-Zip..."

    if (Test-Path $Portable) {
        Remove-Item $Portable -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $Portable | Out-Null

    & $seven x $archive "-o$Portable" -y | Out-Host

    if ($LASTEXITCODE -ne 0) {
        throw "7-Zip failed to extract w64devkit (exit code $LASTEXITCODE)."
    }

    $root = Find-PortableRoot

    if (-not $root) {
        throw "w64devkit was extracted, but gcc.exe was not found under $Portable."
    }

    Say "w64devkit ready: $root"

    return $root
}

function Ensure-MK64ToolSources {
    $makefile = Join-Path $Tools 'Makefile'

    # Current upstream MK64 keeps these native tools as source files
    # directly inside tools/.
    #
    # There are NOT separate directories such as:
    #   tools/n64graphics/
    #   tools/mio0/
    #   tools/tkmk00/
    #
    # The required source files are:
    #   n64graphics.c
    #   n64graphics.h
    #   libmio0.c
    #   libtkmk00.c
    #   utils.c
    #   utils.h

    $valid = (
        (Test-Path $makefile) -and
        (Test-Path (Join-Path $Tools 'n64graphics.c')) -and
        (Test-Path (Join-Path $Tools 'n64graphics.h')) -and
        (Test-Path (Join-Path $Tools 'libmio0.c')) -and
        (Test-Path (Join-Path $Tools 'libtkmk00.c')) -and
        (Test-Path (Join-Path $Tools 'utils.c')) -and
        (Test-Path (Join-Path $Tools 'utils.h'))
    )

    if ($valid) {
        Say 'MK64 native tool sources are already present.'
        return
    }

    Say 'MK64 native tool sources are missing/incomplete.'
    Say 'Downloading the upstream MK64 source archive to restore tools/...'

    New-Item `
        -ItemType Directory `
        -Force `
        -Path $DownloadDir | Out-Null

    $zip = Join-Path $DownloadDir 'mk64-upstream-master.zip'

    $url = 'https://github.com/n64decomp/mk64/archive/refs/heads/master.zip'

    if (-not (Test-Path $zip)) {
        Invoke-WebRequest `
            -Uri $url `
            -OutFile $zip `
            -UseBasicParsing
    }

    $temp = Join-Path $DownloadDir 'mk64-upstream-extracted'

    if (Test-Path $temp) {
        Remove-Item $temp -Recurse -Force
    }

    New-Item `
        -ItemType Directory `
        -Force `
        -Path $temp | Out-Null

    Expand-Archive `
        -Path $zip `
        -DestinationPath $temp `
        -Force

    $upstream = Get-ChildItem `
        -Path $temp `
        -Directory |
        Where-Object {
            Test-Path (Join-Path $_.FullName 'tools\Makefile')
        } |
        Select-Object -First 1

    if (-not $upstream) {
        throw 'The upstream MK64 archive was downloaded, but tools\Makefile was not found.'
    }

    $upTools = Join-Path $upstream.FullName 'tools'

    # Restore only the tools directory.
    # Do not replace mk64-master and do not touch the source tree.

    if (Test-Path $Tools) {
        Get-ChildItem -Path $Tools -Force |
            Where-Object {
                $_.Name -notin @(
                    '_downloads',
                    '_portable_host',
                    'mingw64',
                    'torch'
                )
            } |
            Remove-Item -Recurse -Force
    }
    else {
        New-Item `
            -ItemType Directory `
            -Force `
            -Path $Tools | Out-Null
    }

    Copy-Item `
        -Path (Join-Path $upTools '*') `
        -Destination $Tools `
        -Recurse `
        -Force

    # Validate the actual upstream layout.

    if (-not (Test-Path $makefile)) {
        throw 'MK64 tools were restored, but tools\Makefile is still missing.'
    }

    if (-not (Test-Path (Join-Path $Tools 'n64graphics.c'))) {
        throw 'MK64 tools were restored, but tools\n64graphics.c is missing.'
    }

    if (-not (Test-Path (Join-Path $Tools 'libmio0.c'))) {
        throw 'MK64 tools were restored, but tools\libmio0.c is missing.'
    }

    if (-not (Test-Path (Join-Path $Tools 'libtkmk00.c'))) {
        throw 'MK64 tools were restored, but tools\libtkmk00.c is missing.'
    }

    Say 'MK64 native tool sources restored successfully.'
}

function Run-Make([string[]]$MakeArgs) {
    $make = Join-Path $script:W64Root 'bin\make.exe'

    if (-not (Test-Path $make)) {
        throw "make.exe not found at $make"
    }

    & $make @MakeArgs

    if ($LASTEXITCODE -ne 0) {
        throw "make failed with exit code $LASTEXITCODE"
    }
}

function Ensure-PortableCMake {
    $existing = Get-Command cmake.exe -ErrorAction SilentlyContinue

    if ($existing) {
        return $existing.Source
    }

    $dest = Join-Path $PortableHost 'cmake'

    $found = Get-ChildItem `
        -Path $dest `
        -Filter cmake.exe `
        -Recurse `
        -File `
        -ErrorAction SilentlyContinue |
        Select-Object -First 1

    if ($found) {
        return $found.FullName
    }

    New-Item `
        -ItemType Directory `
        -Force `
        -Path $DownloadDir, $dest | Out-Null

    Say 'Downloading portable CMake...'

    $h = @{
        'User-Agent' = 'MK64-Xbox360-AssetSetup'
    }

    $r = Invoke-RestMethod `
        -Headers $h `
        -Uri 'https://api.github.com/repos/Kitware/CMake/releases/latest'

    $a = $r.assets |
        Where-Object {
            $_.name -match '^cmake-.*-windows-x86_64\.zip$'
        } |
        Select-Object -First 1

    if (-not $a) {
        throw 'Could not locate portable CMake package.'
    }

    $z = Join-Path $DownloadDir $a.name

    Invoke-WebRequest `
        -Headers $h `
        -Uri $a.browser_download_url `
        -OutFile $z `
        -UseBasicParsing

    if (Test-Path $dest) {
        Remove-Item $dest -Recurse -Force
    }

    New-Item `
        -ItemType Directory `
        -Force `
        -Path $dest | Out-Null

    Expand-Archive `
        $z `
        $dest `
        -Force

    $found = Get-ChildItem `
        -Path $dest `
        -Filter cmake.exe `
        -Recurse `
        -File |
        Select-Object -First 1

    if (-not $found) {
        throw 'CMake download extracted but cmake.exe was not found.'
    }

    return $found.FullName
}

function Ensure-PortableNinja {
    $existing = Get-Command ninja.exe -ErrorAction SilentlyContinue

    if ($existing) {
        return $existing.Source
    }

    $dest = Join-Path $PortableHost 'ninja'
    $exe = Join-Path $dest 'ninja.exe'

    if (Test-Path $exe) {
        return $exe
    }

    New-Item `
        -ItemType Directory `
        -Force `
        -Path $DownloadDir, $dest | Out-Null

    Say 'Downloading portable Ninja...'

    $h = @{
        'User-Agent' = 'MK64-Xbox360-AssetSetup'
    }

    $r = Invoke-RestMethod `
        -Headers $h `
        -Uri 'https://api.github.com/repos/ninja-build/ninja/releases/latest'

    $a = $r.assets |
        Where-Object {
            $_.name -eq 'ninja-win.zip'
        } |
        Select-Object -First 1

    if (-not $a) {
        throw 'Could not locate ninja-win.zip.'
    }

    $z = Join-Path $DownloadDir 'ninja-win.zip'

    Invoke-WebRequest `
        -Headers $h `
        -Uri $a.browser_download_url `
        -OutFile $z `
        -UseBasicParsing

    Expand-Archive `
        $z `
        $dest `
        -Force

    if (-not (Test-Path $exe)) {
        throw 'Ninja download extracted but ninja.exe was not found.'
    }

    return $exe
}

function Ensure-PortableGit {
    $existing = Get-Command git.exe -ErrorAction SilentlyContinue

    if ($existing) {
        return $existing.Source
    }

    $dest = Join-Path $PortableHost 'git'

    $found = Get-ChildItem `
        -Path $dest `
        -Filter git.exe `
        -Recurse `
        -File `
        -ErrorAction SilentlyContinue |
        Where-Object {
            $_.FullName -match '\\cmd\\git\.exe$'
        } |
        Select-Object -First 1

    if ($found) {
        return $found.FullName
    }

    New-Item `
        -ItemType Directory `
        -Force `
        -Path $DownloadDir, $dest | Out-Null

    Say 'Downloading portable Git (needed by Torch dependencies)...'

    $h = @{
        'User-Agent' = 'MK64-Xbox360-AssetSetup'
    }

    $r = Invoke-RestMethod `
        -Headers $h `
        -Uri 'https://api.github.com/repos/git-for-windows/git/releases/latest'

    $a = $r.assets |
        Where-Object {
            $_.name -match '^PortableGit-.*-64-bit\.7z\.exe$'
        } |
        Select-Object -First 1

    if (-not $a) {
        throw 'Could not locate PortableGit 64-bit package.'
    }

    $pkg = Join-Path $DownloadDir $a.name

    Invoke-WebRequest `
        -Headers $h `
        -Uri $a.browser_download_url `
        -OutFile $pkg `
        -UseBasicParsing

    if (Test-Path $dest) {
        Remove-Item $dest -Recurse -Force
    }

    New-Item `
        -ItemType Directory `
        -Force `
        -Path $dest | Out-Null

    $proc = Start-Process `
        -FilePath $pkg `
        -ArgumentList @(
            '-y',
            "-o$dest"
        ) `
        -Wait `
        -PassThru

    if ($proc.ExitCode -ne 0) {
        throw "PortableGit extractor exited with code $($proc.ExitCode)."
    }

    $found = Get-ChildItem `
        -Path $dest `
        -Filter git.exe `
        -Recurse `
        -File |
        Where-Object {
            $_.FullName -match '\\cmd\\git\.exe$'
        } |
        Select-Object -First 1

    if (-not $found) {
        throw 'PortableGit extracted but git.exe was not found.'
    }

    return $found.FullName
}

function Ensure-TorchSource {
    $torch = Join-Path $Tools 'torch'

    if (
        (Test-Path (Join-Path $torch 'CMakeLists.txt')) -and
        (Test-Path (Join-Path $torch 'src'))
    ) {
        Say 'Torch source is already present.'
        return
    }

    Say 'Cloning Torch and its submodules...'

    if (Test-Path $torch) {
        Remove-Item $torch -Recurse -Force
    }

    & $script:GitExe clone `
        --recursive `
        --depth 1 `
        https://github.com/HarbourMasters/Torch.git `
        $torch

    if ($LASTEXITCODE -ne 0) {
        throw "git clone Torch failed with exit code $LASTEXITCODE"
    }

    if (-not (Test-Path (Join-Path $torch 'CMakeLists.txt'))) {
        throw 'Torch source setup failed.'
    }
}

function Patch-TorchMinGWZlib {
    $cmakeFile = Join-Path `
        (Join-Path $Tools 'torch') `
        'CMakeLists.txt'

    if (-not (Test-Path $cmakeFile)) {
        throw 'Torch CMakeLists.txt was not found.'
    }

    $text = Get-Content `
        -Raw `
        -Path $cmakeFile

    $pattern = '(?s)if\s*\(MSVC\)\s*\r?\n\s*FetchContent_Declare\(\s*\r?\n\s*zlib\s*\r?\n\s*GIT_REPOSITORY\s+"https://github\.com/madler/zlib\.git".*?\r?\n\s*endif\(\)'

    if ($text -match $pattern) {
        Say 'Patching Torch CMake so MinGW uses Torch''s bundled zlib build...'

        $replacement = @'
# MK64 Xbox 360 setup: skip Torch's premature standalone/system ZLIB lookup.
# Torch's later "Link zlib" section FetchContent-builds zlibstatic for Windows.
'@

        $text = [regex]::Replace(
            $text,
            $pattern,
            $replacement,
            1
        )

        Set-Content `
            -Path $cmakeFile `
            -Value $text `
            -Encoding UTF8
    }
    elseif (
        $text -match
        'MK64 Xbox 360 setup: skip Torch''s premature standalone/system ZLIB lookup'
    ) {
        Say 'Torch MinGW zlib patch is already applied.'
    }
    else {
        throw 'Torch CMake zlib section changed upstream; refusing to patch the wrong block.'
    }
}

function Build-Torch {
    $torch = Join-Path $Tools 'torch'
    $build = Join-Path $torch 'cmake-build-release'

    $existing = Get-ChildItem `
        -Path $build `
        -Filter torch.exe `
        -Recurse `
        -File `
        -ErrorAction SilentlyContinue |
        Select-Object -First 1

    if ($existing) {
        Say ("Reusing existing Torch executable: " + $existing.FullName)
        return [string]$existing.FullName
    }

    if (Test-Path $build) {
        Remove-Item $build -Recurse -Force
    }

    New-Item `
        -ItemType Directory `
        -Force `
        -Path $build | Out-Null

    Say 'Configuring Torch with portable GCC + Ninja (MK64 support only)...'

    & $script:CMakeExe `
        -S $torch `
        -B $build `
        -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DBUILD_MK64=ON `
        -DBUILD_SM64=OFF `
        -DBUILD_SF64=OFF `
        -DBUILD_PM64=OFF `
        -DBUILD_FZERO=OFF `
        -DBUILD_BK64=OFF `
        -DBUILD_MARIO_ARTIST=OFF `
        -DBUILD_NAUDIO=OFF `
        -DBUILD_OOT=OFF `
        -DBUILD_UI=OFF `
        -DBUILD_STORMLIB=OFF |
        Out-Host

    if ($LASTEXITCODE -ne 0) {
        throw "Torch CMake configure failed with exit code $LASTEXITCODE"
    }

    Say 'Building Torch...'

    & $script:CMakeExe `
        --build $build `
        --parallel |
        Out-Host

    if ($LASTEXITCODE -ne 0) {
        throw "Torch build failed with exit code $LASTEXITCODE"
    }

    $exe = Get-ChildItem `
        -Path $build `
        -Filter torch.exe `
        -Recurse `
        -File `
        -ErrorAction SilentlyContinue |
        Select-Object -First 1

    if (-not $exe) {
        throw 'Torch build completed but torch.exe was not found.'
    }

    return $exe.FullName
}

# ---------------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------------

$script:W64Root = Get-W64DevKit

$env:PATH =
    (Join-Path $script:W64Root 'bin') +
    ';' +
    $env:PATH

Ensure-MK64ToolSources

Say 'Building only the three native helpers extract_assets.py actually needs...'

Push-Location $Repo

try {
    Run-Make @(
        '-C',
        'tools',
        'n64graphics',
        'mio0',
        'tkmk00'
    )
}
finally {
    Pop-Location
}

foreach ($base in @(
    'n64graphics',
    'mio0',
    'tkmk00'
)) {
    $candidates = @(
        (Join-Path $Tools ($base + '.exe')),
        (Join-Path $Tools $base)
    )

    $candidate = $candidates |
        Where-Object {
            Test-Path $_
        } |
        Select-Object -First 1

    if (-not $candidate) {
        throw "Expected helper was not built: tools\$base(.exe)"
    }

    Say ("Found helper: " + $candidate)
}

Say 'Legacy extraction helper tools are ready.'

if (-not $SkipTorch) {
    $script:CMakeExe = Ensure-PortableCMake
    $script:NinjaExe = Ensure-PortableNinja
    $script:GitExe = Ensure-PortableGit

    $env:PATH =
        (Split-Path $script:CMakeExe -Parent) +
        ';' +
        (Split-Path $script:NinjaExe -Parent) +
        ';' +
        (Split-Path $script:GitExe -Parent) +
        ';' +
        $env:PATH

    Ensure-TorchSource
    Patch-TorchMinGWZlib

    $torchExe = Build-Torch

    Set-Content `
        -Path (Join-Path $Tools 'torch_exe_path.txt') `
        -Value $torchExe `
        -Encoding ASCII

    Say "Torch ready: $torchExe"
}

Say 'Windows asset tools setup completed successfully.'