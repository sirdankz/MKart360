# MKart360 — HD Textures

## 1. Division of the work

Before the technical part, I want to make clear what Felipe and I each worked on.

`PACK_TEXTURES.py` was made by Felipe. I only made some adjustments to it when needed for the project.

The 60 FPS script was also made by Felipe. I only changed some parts of it when necessary for the project.

The `menu_tiles_geometry.json` file was made entirely by Felipe. The menu geometry is his work.

My work was mainly on the rest of the system: extractor, decompressor, analysis of the extracted data, manifests, loading adjustments, TKMK00, cache, and the other changes needed to make the textures work on Xbox 360.

I am not taking credit for the parts Felipe made. This document separates what each of us actually worked on.

## 1. What I changed

This document covers only the HD texture work in MKart360.

I keep ROM translation, region support, and translated text separate.

This part covers:

- hash-based texture identification;
- HD loading;
- `.tex`;
- `tex.pak`;
- manifests;
- sprites;
- menu images;
- TMEM;
- caching;
- TKMK00;
- final Xbox 360 organization.

## 2. Texture identification

The original texture is identified with a 32-bit FNV-1a hash.

The basic process is:

```text
original texture
    ↓
FNV-1a 32-bit
    ↓
hash
    ↓
find HD replacement
```

Example:

```text
79fcbc0f
```

The same identifier is used by the preparation system and the Xbox 360 code.

The HD texture stays outside the ROM. If there is no replacement, the game keeps using the original texture.

## 3. `gfx_pc.c`

The main HD loading file is:

```text
mk64-master/include/xbox360/gfx_pc.c
```

It identifies the texture, looks for the HD version, reads the data, uploads it to the GPU, and keeps cache information.

The shared HD buffer is:

```c
static uint8_t x360_hd_buf[2048 * 2048 * 4];
```

## 4. From `.tex` files to `tex.pak`

The first version used separate `.tex` files:

```text
game:\tex\<prefix>\<hash>.tex
```

I later added:

```powershell
py .\PACK_TEXTURES.py --pak
```

The main output is:

```text
tex.pak
```

The PAK contains an index and texture data. The index stores information such as:

```text
hash
offset
size
width
height
```

On Xbox 360, `gfx_pc.c` uses:

```c
x360_pak_read()
```

to find the texture by hash.

Old `.tex` files can remain as compatibility support, but `tex.pak` is the main path.

## 5. Manifests

I use manifests to separate the different texture categories.

The main ones are:

```text
generated_texture_manifest.json
kart_sprite_manifest.json
lakitu_sprite_manifest.json
asset_sprite_manifest.json
texture_tkmk00_manifest.json
```

They connect the extracted PNG with the information needed to prepare the texture.

For textures loaded in parts, the manifests can also contain:

```text
tmem_halves
tmem_tiles
```

This lets me reproduce the way the game actually loaded the texture instead of simply cutting an image into a generic grid.

## 6. Large menu images

Some menu images are loaded by the game through several TMEM operations.

For cases where the complete image can be used, the code has:

```c
x360_try_draw_hd_menu_quad()
```

The related files include:

```text
menu_items.c
menu_tiles_geometry.json
```

Some menu geometry was recorded manually because it matches the real loading behavior better.

## 7. Cache

The cache prevents the same HD texture from being read and uploaded to the GPU repeatedly.

There is also a menu-specific cache.

In the validated HD base, the menu cache was increased to:

```c
#define X360_HDMENU_MAX 512
```

I keep this base before making new changes.

The general cache can also use texture content to distinguish cases where the same RAM address contains different data.

For CI textures, the identity can consider both the indices and the palette.

The content hash is bounded by checks such as:

```text
size > 0
size <= 8192
line_size <= 8192
```

## 8. Deduplication and collisions

`PACK_TEXTURES.py` also checks for duplicate data.

If two entries have the same hash, dimensions, and bytes, they can share the same stored texture data.

FNV-1a is only 32 bits, so different textures can also produce the same hash.

When the packer finds:

```text
same hash
+
different data
```

it records the collision instead of silently replacing one texture with another.

## 9. Texture size limit

The packer supports:

```text
--max
```

The limit used by this system is:

```text
2048
```

A texture larger than that in width or height is not automatically included.

## 10. TKMK00

TKMK00 is handled separately because some images are assembled and loaded in TMEM during runtime.

The main manifest is:

```text
texture_tkmk00_manifest.json
```

I also created:

```text
texture_tkmk00_runtime_manifest.json
texture_tkmk00_names_runtime_manifest.json
```

These record runtime information such as:

```text
width
height
tile_width
tile_height
TMEM
position
hash
logical_hash
```

The important point is that the hash of the block actually read by the runtime can differ from the logical image hash.

### Character names

One example is an image logically registered as:

```text
64 × 12
```

but loaded by the runtime as:

```text
64 × 13
```

That changes the number of bytes used by the hash.

Because of this, the code tries the normal hash first. If it is not found, it can try the logical hash.

This second lookup was added without changing the normal path for textures that already worked.

### `OK` texture

The texture:

```text
texture_ok
```

has logical hash:

```text
dff91b13
```

The runtime showed two parts with:

```text
35d069ff
154196f3
```

In this case, the code can find the complete texture and crop the required region.

I did not turn all TKMK00 resources into a generic full-image path. Resources that depend on their original TMEM divisions keep that path.

## 11. Debugging

During the investigation I temporarily enabled texture tracing in `gfx_pc.c` to discover:

```text
format
siz
width
height
source size
hash
pitch
position
tile
HD lookup result
```

This was used to discover the actual runtime behavior.

The final version keeps the texture trace disabled:

```c
#define X360_HDTEX_TRACE 0
```

The debug was a development tool, not part of the final runtime.

## 12. What is automatic and what is manual

Automatic:

- common texture extraction;
- common manifest generation;
- `tex.pak` packing.

Manual:

- discovering TKMK00 runtime data;
- `texture_tkmk00_runtime_manifest.json`;
- `texture_tkmk00_names_runtime_manifest.json`;
- `menu_tiles_geometry.json`;
- editing HD textures;
- final Xbox 360 testing.

The manual manifests are kept because they bridge the runtime data I discovered with the offline packing process.

## 13. Main files

```text
mk64-master/include/xbox360/gfx_pc.c
mk64-master/src/menu_items.c
mk64-master/PACK_TEXTURES.py
mk64-master/EXTRACT_MK64_TEXTURES.py
mk64-master/menu_tiles_geometry.json

mk64-master/extracted_textures/generated_texture_manifest.json
mk64-master/extracted_textures/kart_sprite_manifest.json
mk64-master/extracted_textures/lakitu_sprite_manifest.json
mk64-master/extracted_textures/asset_sprite_manifest.json
mk64-master/extracted_textures/texture_tkmk00_manifest.json
mk64-master/extracted_textures/texture_tkmk00_runtime_manifest.json
mk64-master/extracted_textures/texture_tkmk00_names_runtime_manifest.json
```

## 14. The workflow I use

```text
ROM
↓
EXTRACT_MK64_TEXTURES.py
↓
extracted_textures
↓
manifests
↓
edit PNGs
↓
PACK_TEXTURES.py --pak
↓
tex.pak
↓
Xbox 360
↓
gfx_pc.c
↓
original texture hash
↓
x360_pak_read()
↓
cache
↓
GPU
```

For continued development, I start from the validated functional base and make one change at a time. I generate the PAK and test on Xbox 360 before making the next change.

## 15. Repository organization

The ROM is not included in the repository.

Generated folders such as `extracted_textures/` and `tex/` do not need to be distributed as part of the initial source base.

The important part is keeping the source code, required manual manifests, and configuration needed to reproduce the process.

## 16. Result

The final system keeps HD textures separate from the original ROM data.

The main container is:

```text
tex.pak
```

and the main loading path is:

```text
hash → PAK → cache → GPU
```

TKMK00 remains a special category, with the runtime information needed to reproduce cases that cannot be handled correctly by offline extraction alone.
