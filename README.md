# MKart360 — Edit Textures Fork

Fork of [sirdankz/MKart360](https://github.com/sirdankz/MKart360) (Xbox 360
port of Mario Kart 64, based on the [n64decomp/mk64](https://github.com/n64decomp/mk64)
decompilation). This fork adds a **runtime HD texture replacement system**,
without modifying the ROM or the compiled game assets, and works with **all ROM regions** thanks to my personal friend **[Eduardo](https://github.com/EduDicaseGameplay)** from the YouTube channel **[Edu Dicas e Gameplay](https://www.youtube.com/@EduDicaseGameplay)**,

---

## What changed compared to the original project

### Modified code

- **`mk64-master/include/xbox360/gfx_pc.c`**
  HD texture hook inside `import_texture()`: the game computes an FNV-1a
  hash of each original texture's content, and if an HD version with that
  hash exists, it's used instead. Includes:
  - reading `tex.pak` through a hash-indexed table;
  - fallback to standalone `.tex` files in subfolders (to work around the
    FATX per-folder file limit);
  - RAM cache for HD textures (avoids re-reading from disk);
  - enlarged texture cache (from 512 to 1024 entries);
  - diagnostic trace disabled by default (`X360_HDTEX_TRACE 0`), with a
    menu mode (`X360_HDTEX_TRACE_MENU`) that also logs each piece's position;
  - the HD hook no longer overwrites the texture cache's content hash —
    this fixed flickering on menu textures loaded in blocks;
  - optional **DXT1/DXT5 compression** inside `tex.pak` (see
    [Compressed tex.pak](#compressed-texpak-dxt-optional)), decoded on the
    console before the texture is sent to the GPU. Older uncompressed
    `tex.pak` files are still read normally.
  - `x360_try_draw_hd_menu_quad` already exists in the code but is
    **inactive** (not called yet) — reserved for a future replacement of
    the large menu textures.

- **`mk64-master/.gitignore`**
  Now ignores extracted textures, `tex.pak`, trace logs, and
  `src/xbox360/generated_banks/` (all derived from the user's own ROM, and
  shouldn't go into version control).

### Known limitations

- High-resolution textures (higher than HQ) can overload the Xbox 360 hardware and cause crashes or performance issues; HD support exists but is limited to the console's hardware.
- Running tex.pak on internal or external mechanical hard disk drives may cause stuttering during gameplay; it is recommended to use USB flash drives (pen drives) or SSDs.
- DXT compression is lossy: it shrinks `tex.pak` a lot, but can add visible
  grain/banding and small color shifts (see
  [Compressed tex.pak](#compressed-texpak-dxt-optional)).

---

## 🖼️ Preview

### Previous background 

<p align="center">
  <img src="assets/screenshot1.png" width="800"/>
</p>

### Background later

<p align="center">
  <img src="assets/screenshot2.png" width="800"/>
</p>

### Select game first

<p align="center">
  <img src="assets/screenshot3.png" width="800"/>
</p>

### Select game later

<p align="center">
  <img src="assets/screenshot4.png" width="800"/>
</p>

### Select character first

<p align="center">
  <img src="assets/screenshot5.png" width="800"/>
</p>

### Select character later

<p align="center">
  <img src="assets/screenshot6.png" width="800"/>
</p>

### Select maps first

<p align="center">
  <img src="assets/screenshot7.png" width="800"/>
</p>

### Select maps later

<p align="center">
  <img src="assets/screenshot8.png" width="800"/>
</p>

### gameplay of the game from before

<p align="center">
  <img src="assets/screenshot9.png" width="800"/>
</p>

### gameplay of the game later

<p align="center">
  <img src="assets/screenshot10.png" width="800"/>
</p>

---

## New tools

All the scripts below must be executed from within the `mk64-master/` folder; those created for diagnostics/debugging and testing are located in the `legacy_diagnostic_tools/` folder but must be moved to `mk64-master/` to work—with the exception of `HALVE_PNGS.py`, which works from any location.

| File | Purpose |
|---|---|
| `EXTRACT_MK64_TEXTURES.py` | Extracts textures from the ROM into editable PNGs |
| `PACK_TEXTURES.py` | Packs edited PNGs back into the format the game reads (`tex.pak`) |
| `HALVE_PNGS.py` | Halves PNG resolution, to fit the console's memory, Use this for very large textures or if you encounter performance issues.|
| `SCAN_HALVES.py` | Diagnostic tool: finds where the "bottom halves" of kart sprites live when their hash doesn't match |
| `CROSS_CHECK.py` | Cross-references the game's trace log with the manifests to find textures that weren't found |
| `SCAN_MENU.py` | Measures how the game splits large menu images into blocks, from a trace log |
| `menu_tiles_geometry.json` | Measured block layout of the menu images (coordinates only, no ROM data) |

One-time install requirement:

```powershell
pip install pillow
```

### Full usage workflow

**1. Extract textures from the ROM**

```powershell
py .\EXTRACT_MK64_TEXTURES.py --rom .\baserom.us.z64
```

Generates the `extracted_textures\` folder with:

- **root** — common textures, named `<hash>__name.png`;
- **`generated\`** — generated banks (menus, HUD), named by symbol;
- **`karts\<character>\frames\`** — driver+kart sprites (321 per
  character);
- **`*_manifest.json`** — metadata linking each PNG to its hash. **Do not
  delete or move these files.**

By default the extractor doesn't overwrite existing PNGs (safe to re-run —
it only fills in what's missing). Use `--force` to regenerate everything
from scratch (discards your edits). Other options: `--no-karts`,
`--no-generated`, `--out FOLDER`.


**2. Edit**

Open the PNGs and redraw/upscale them in HD. You can freely change the
resolution (e.g. 128×128 instead of a 64×64 original). **Don't rename the
files** — the name (or the path recorded in the manifest) is what links the
edited texture back to the original.

**3. Pack**

```powershell
py .\PACK_TEXTURES.py --only karts --pak
```

Generates `tex\tex.pak`, a single file with everything bundled inside.

| Option | Effect |
|---|---|
| `--pak` | Produces a single file (**recommended**) |
| `--only PREFIX` | Limits to a subset, e.g. `--only karts\bowser` |
| `--out FOLDER` | Output folder (default `tex`) |
| `--max N` | Skips images larger than N pixels (default 2048) |
| `--dxt` | With `--pak`: stores textures compressed as DXT1/DXT5 (see below) |

Without `--pak`, thousands of loose `.tex` files are generated — it works,
but loads more slowly and is more fragile to transfer; use it only for
debugging. Only pack what you actually edited: unedited PNGs produce
textures identical to the originals, with no visual gain but still costing
space and memory.

**4. Install on the console**

Copy `tex.pak` to the root of the game folder, next to the executable:

```
MK64.xex
baserom.us.z64
tex.pak          <- here, NOT inside a tex\ folder
```

Swapping textures doesn't require recompiling .XEX file — `tex.pak` is read at
runtime. Recompiling is only needed when you change C code.

### Compressed tex.pak (DXT, optional)

```powershell
pip install numpy
py .\PACK_TEXTURES.py --pak --dxt
```

Each texture is stored as **DXT1** (opaque or on/off transparency, ~8× smaller
than RGBA) or **DXT5** (smooth transparency, ~4× smaller). The console reads
less from disk and fits more textures in its RAM cache; the data is
decompressed by the CPU before being sent to the GPU.

Limitations:

- **Lossy.** Gradients may show grain or banding, and sprites can get small
  color shifts (e.g. lighter edges). Compare with and without `--dxt` and
  keep whichever looks better to you.
- **It does not remove loading hitches.** Testing showed the first-time
  stutters in menus and in texture-heavy courses are caused by the *number*
  of disk reads, not their size — DXT makes each read smaller, not fewer.
- **GPU memory is unchanged:** textures still reach the GPU as RGBA, so
  video memory use is the same as without compression.
- Textures whose width or height is not a multiple of 4, and the menu "OK"
  button texture, are always stored uncompressed.
- A compressed `tex.pak` **requires the updated `gfx_pc.c`**; older builds
  can't read it. The updated build reads both compressed and uncompressed
  packs.

### Memory limits

The Xbox 360 has 512 MB shared between system and video. The full
character roster adds up to 2568 sprites (2 files each):

| Resolution | Estimated total | Notes |
|---|---|---|
| 256×256 | ~640 MB | doesn't fit |
| 128×128 | ~160 MB | recommended |
| 96×96 | ~90 MB | more headroom |

To shrink already-edited PNGs:

```powershell
py .\HALVE_PNGS.py --recursive              # preview, doesn't change anything
py .\HALVE_PNGS.py --apply --recursive      # actually applies it
```

Use `--backup` to keep the originals as `*.orig.png`.

### Diagnostics (when a texture doesn't show up in HD)

In `include\xbox360\gfx_pc.c`, change:

```c
#define X360_HDTEX_TRACE 0   →   1
```

Recompile, play for a few seconds, then grab `game:\hdtex-trace.log`. Each
line shows the hash computed at runtime and `found=1` or `found=0`. To
cross-reference that log with the manifests:

```powershell
py .\CROSS_CHECK.py --log .\hdtex-trace.log --kart bowser
```

If the missing texture is a kart sprite's "bottom half" that never matches
(`found=0` even though it exists in the manifest), run:

```powershell
py .\SCAN_HALVES.py --rom .\baserom.us.z64 --log .\hdtex-trace.log --kart bowser
```

This scans the decompressed ROM block looking for which 2048-byte window
produces the missing hash, revealing the half's real offset — useful for
fixing the extractor.

Leave the trace disabled during normal use: it writes to disk during
gameplay and affects performance.

### Menu textures

Large menu images (e.g. the character-select portraits) exceed the 4 KB
TMEM, so the game loads them in blocks (33×33 with a 1-texel overlap), each
with its own hash. `menu_tiles_geometry.json` stores the measured block
layout; `PACK_TEXTURES.py` computes each block's hash from **your own** ROM
data and crops the HD art to match. Nothing to do: just edit the PNG under
`extracted_textures\generated\course_player_selection\` and pack.

To add a screen that isn't measured yet: set `X360_HDTEX_TRACE 1` in
`gfx_pc.c`, rebuild with `/t:Rebuild`, stay a few seconds on the screen,
copy `hdtex-trace.log` to `mk64-master\` and run:

```powershell
py .\SCAN_MENU.py --log .\hdtex-trace.log --only generated/course_player_selection
```

It adds the new images to `menu_tiles_geometry.json`. Set the trace back to
`0` afterwards.

### Common issues

- **Textures don't show up** → check that `tex.pak` is at the root, next
  to `MK64.xex` (not inside `tex\`). If that's correct, force a full
  rebuild (`/t:Rebuild`) — `gfx_pc.c` is included by another file, and
  incremental builds sometimes miss the change.
- **Only half the sprite is HD** → CI8 sprites are loaded in two windows
  because of the 4 KB TMEM limit, each with its own hash. The extractor
  already handles this; if it happens, regenerate the manifests with the
  updated extractor and repack.
- **Transfer to the console fails at the end** → the Xbox 360's file
  system (FATX) allows a maximum of 4096 files per folder. That's exactly
  what the `--pak` option solves, by producing a single file.
- **Stutters in-game** → lower the resolution (128×128) or the number of
  replaced characters. A small hitch when a new opponent appears on screen
  is expected: their sprites are loaded at that moment.

### How it works under the hood

1. The game loads a texture and computes an FNV-1a hash of the original
   content.
2. `gfx_pc.c` looks up that hash in `tex.pak`.
3. If found, the HD version is sent to the GPU instead of the original.
4. If not found, it falls back to the normal path — nothing breaks.

Original dimensions are preserved internally for coordinate mapping, so
the HD texture can be any resolution. Loaded data is cached in RAM to
avoid re-reading from disk.

---

## How to build the project

### Requirements

- Windows
- Python 3
- Xbox 360 SDK / Visual Studio Xbox 360 build tools
- Your own US Mario Kart 64 ROM
- An Xbox 360 capable of running homebrew XEX files

### 1. Prepare the assets

Place your ROM at:

```
mk64-master\baserom.us.z64
```

Then, inside `mk64-master`:

```powershell
powershell -ExecutionPolicy Bypass -File ".\xbox360\setup_windows_asset_tools.ps1" -SkipTorch
```
Wait for all the necessary requirements to download and install, and then:

```powershell
py ".\PUBLIC_PREPARE_MK64_ASSETS.py"
```

The script verifies the ROM and generates the ROM-derived files that are
intentionally left out of the public source release.

### 2. Build

From the folder containing `MK64.sln`, run:

```powershell
& "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe" ".\MK64.sln" /t:Build "/p:Configuration=Release" "/p:Platform=Xbox 360"
```

Or, more simply:

```powershell
powershell -ExecutionPolicy Bypass -File ".\PUBLIC_BUILD_XBOX360.ps1"
```

The compiled `.xex` will be placed in the Xbox 360 project's Release
output folder.

### 3. Apply HD textures (optional)

After building, generate and copy `tex.pak` as described in the
["New tools"](#new-tools) section above. Swapping textures afterward
doesn't require rebuilding.

> If something doesn't take effect after editing `gfx_pc.c`, force
> `/t:Rebuild` — incremental builds sometimes don't detect changes to that
> file.

---

## Credits

- **[n64decomp/mk64](https://github.com/n64decomp/mk64)** — the original
  Mario Kart 64 decompilation, the foundation of the whole project.
- **[sirdankz/MKart360](https://github.com/sirdankz/MKart360)** — the
  Xbox 360 port (native build, console-to-console multiplayer, rendering
  fixes), of which this repository is a fork.
- **[Edu dicas e gameplay](https://github.com/EduDicaseGameplay)** - The person
  who managed to simply decipher the texture extraction system of "TKMK00" so that
  it was possible to replace the frames and textures of the main menu, in addition
  to having managed to improve the extraction system to a point that I could not achieve.

Mario Kart 64 and related properties belong to Nintendo. This is an
unofficial homebrew port, not affiliated with or endorsed by Nintendo.
