$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Source = Join-Path $Here 'mk64-master'
Push-Location $Source
try {
    $BrRom = Join-Path $Source 'baserom.br.z64'
    $UsRom = Join-Path $Source 'baserom.us.z64'
    if (Test-Path $BrRom) {
        py .\PUBLIC_PREPARE_MK64_ASSETS.py --rom $BrRom
    } elseif (Test-Path $UsRom) {
        py .\PUBLIC_PREPARE_MK64_ASSETS.py --rom $UsRom
    } else {
        Write-Host 'ERROR: No ROM found.' -ForegroundColor Red
        Write-Host "Put your BR ROM here: $BrRom"
        Write-Host "or your US ROM here: $UsRom"
        exit 1
    }
    if ($LASTEXITCODE -ne 0) { throw "asset preparation failed" }
} finally { Pop-Location }
$MSBuild = "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"
if (-not (Test-Path $MSBuild)) { throw "MSBuild not found at $MSBuild" }
& $MSBuild (Join-Path $Here 'MK64.sln') /t:Rebuild "/p:Configuration=Release" "/p:Platform=Xbox 360"
if ($LASTEXITCODE -ne 0) { throw "Xbox 360 Release build failed" }
Write-Host "Build complete: $Here\MK64\Release\MK64.xex"
