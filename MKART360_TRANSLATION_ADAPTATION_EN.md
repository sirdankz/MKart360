# MKart360 — Adapting the Project for Translated ROMs

## 1. What I am adapting

MKart360 was originally built around the USA ROM:

```text
baserom.us.z64
```

A translated ROM contains different bytes, so replacing the ROM file is not enough.

I need to:

- identify the ROM;
- register its identity;
- let the Xbox 360 loader recognize it;
- regenerate ROM-derived data;
- adapt text and characters when necessary;
- check offsets and metadata;
- test the real ROM.

## 2. ROM identification

I do not identify a ROM only by its filename.

I check:

```text
size
CRC32
SHA-1
```

For my PT-BR example:

```text
Size: 0xC00000
CRC32: 3B0D98C1
SHA-1: c2baf5b4a5355fff2dac08e971a62834ef70268c
MD5: d81760c53417ea97b04f311b02558aae
```

To calculate SHA-1:

```powershell
Get-FileHash ".\MyROM.z64" -Algorithm SHA1
```

For CRC32, I use a tool or script that calculates the value from the complete ROM.

## 3. Xbox 360 ROM loader

The main file is:

```text
mk64-master/src/xbox360/xbox360_asset_loader.cpp
```

The original USA ROM uses:

```c
#define X360_EXPECTED_ROM_CRC 0x434389C1U
```

For my PT-BR adaptation, I added:

```c
#define X360_BR_ROM_CRC 0x3B0D98C1U
```

The ROM path changed from:

```text
game:\baserom.us.z64
```

to:

```text
game:\baserom.br.z64
```

The validation is still active. It now accepts the known USA ROM or the known BR ROM and rejects unknown ROMs.

## 4. Asset preparation

I also changed:

```text
mk64-master/PUBLIC_PREPARE_MK64_ASSETS.py
```

For the BR ROM, I registered:

```python
BR_ROM_CRC32 = 0x3B0D98C1
BR_ROM_SHA1 = "c2baf5b4a5355fff2dac08e971a62834ef70268c"
```

The script identifies the variant using CRC32 and SHA-1.

This matters because data extracted from the translated ROM does not have to match the USA ROM data.

I do not remove validation. I add a known ROM variant.

## 5. Regenerating ROM-derived data

After registering the ROM, I use the translated ROM itself to generate the derived data:

```powershell
python PUBLIC_PREPARE_MK64_ASSETS.py --rom "baserom.br.z64"
```

The process is:

```text
translated ROM
↓
extraction
↓
ROM-derived data
↓
generated files
↓
compilation
```

I should not simply copy USA-generated data and expect it to be correct for the translated ROM.

## 6. Build script

I also changed:

```text
PUBLIC_BUILD_XBOX360.ps1
```

so the build can look for the correct ROM before preparing the assets.

In my case:

```text
baserom.br.z64
```

can be selected before:

```text
baserom.us.z64
```

The filename is only a convention. The actual identity still comes from the ROM data.

## 7. Translated text

Some game text is stored directly in:

```text
mk64-master/src/menu_items.c
```

For a translation, I need to locate the original strings and replace them with the translated text.

I also need to check how special characters are encoded.

## 8. Special characters and glyphs

The game does not simply treat these characters as UTF-8.

For each special character, I check:

```text
character
↓
bytes used by the ROM
↓
corresponding glyph
```

If the glyph already exists, I reuse it.

If it does not, I may need to:

```text
create the glyph
↓
create the MenuTexture
↓
add it to gGlyphTextureLUT[]
↓
update the decoder
```

In my PT-BR adaptation I added, for example:

```text
x360_font_Atilde
x360_font_Eacute
```

I do not automatically copy those mappings to another translation. Each ROM needs to be checked.

## 9. C `\x` escapes

I need to be careful with strings such as:

```c
"\xA5DA"
```

because a hexadecimal escape can continue consuming following hexadecimal characters.

When I need separate bytes, I can write:

```c
"\xA5" "DA"
```

The compiler concatenates the strings.

## 10. Character decoder

Adding a glyph is not enough.

The decoder must also recognize the byte sequence used by the translation and select the correct glyph.

When a translation adds characters, I check:

```text
gGlyphTextureLUT[]
decoder
byte sequences
MenuTexture
```

## 11. Track names

I also check:

```text
mk64-master/assets/course_metadata/gCourseNames.inc.c
```

Track names may need translation and special byte sequences.

So translated text is not limited to `menu_items.c`.

## 12. Source integrity hash

The project contains:

```text
mk64-master/PUBLIC_SOURCE_GOLD_HASHES.json
```

If I intentionally change a file covered by this check, I update its SHA-256.

For example, for:

```text
src/xbox360/xbox360_asset_loader.cpp
```

my adaptation changed the hash from:

```text
1d21071ae526bc287f27097f68d211d082c3d9c21b5ea64d46be207384d478c8
```

to:

```text
70a427097d48d4d18d25e9aee54d79de32c643325803b9aec584efb562e88b0f
```

To calculate a new value:

```powershell
Get-FileHash ".\src\xbox360\xbox360_asset_loader.cpp" -Algorithm SHA256
```

I do not change hashes for files I did not modify.

## 13. Region-aware texture extraction

For ROMs from different regions, I also use:

```text
mk64-master/EXTRACT_MK64_TEXTURES.py
```

I can specify the region:

```powershell
py .\EXTRACT_MK64_TEXTURES.py --rom .\baserom.br.z64 --region br
```

or:

```powershell
py .\EXTRACT_MK64_TEXTURES.py --rom .\baserom.us.z64 --region us
```

Region metadata is stored under:

```text
mk64-master/yamls/<region>/
```

For example:

```text
yamls/us/
yamls/br/
```

If region-specific metadata is missing, the project can use USA metadata as a fallback.

I treat that fallback only as compatibility support. It does not prove that USA offsets are correct for another ROM.

## 14. Offsets and TKMK00

A different ROM can move data inside the ROM.

So I check:

```text
offsets
structures
sizes
ROM-derived data
TKMK00
```

For TKMK00, extraction metadata can contain region-specific offsets.

The logic is:

```text
region offset exists
↓
use the region offset

no region offset
↓
use USA as fallback
```

If TKMK00 moved in the target ROM, I need to correct the metadata before relying on the extraction.

## 15. Official ROM from another region

For an official ROM from another region, I use essentially the same process:

1. identify size, CRC32, and SHA-1;
2. register it in the loader;
3. register it in the preparation script;
4. verify offsets;
5. update metadata if necessary;
6. regenerate the assets;
7. compile and test.

I do not rename a different ROM to `baserom.us.z64` and expect that to solve compatibility.

## 16. Translated or modified ROM

A translated or modified ROM should be treated as its own ROM variant.

The process is:

```text
original ROM
↓
translation/modification
↓
new ROM
↓
new CRC32/SHA-1
↓
register the variant
↓
verify the structure
↓
regenerate the data
```

A different CRC does not automatically mean every offset changed.

But if offsets or structures changed, the metadata must also be reviewed.

## 17. Main files

```text
PUBLIC_BUILD_XBOX360.ps1

mk64-master/PUBLIC_PREPARE_MK64_ASSETS.py

mk64-master/src/xbox360/xbox360_asset_loader.cpp

mk64-master/PUBLIC_SOURCE_GOLD_HASHES.json

mk64-master/src/menu_items.c

mk64-master/assets/course_metadata/gCourseNames.inc.c

mk64-master/EXTRACT_MK64_TEXTURES.py

mk64-master/yamls/<region>/

mk64-master/PUBLIC_ASSET_RECIPES.json
```

## 18. Xbox 360 testing

After building, I check at least:

```text
game starts
ROM is recognized
main menu
character selection
cup selection
track names
translated menus
special characters
pause
options
save
error messages
```

Special characters need attention because an incorrect byte mapping can make a character display incorrectly or disappear.

## 19. Workflow for a new ROM

When I adapt another ROM, I use this order:

```text
1. identify the ROM
2. register CRC32/SHA-1
3. register the variant in the loader
4. register the variant in asset preparation
5. verify offsets and metadata
6. verify TKMK00
7. regenerate the assets
8. adapt text and glyphs
9. adapt track names
10. update integrity hashes
11. compile
12. test on Xbox 360
```

I do the ROM analysis before changing the files. This keeps ROM, translation, extraction, and HD rendering problems separate.

## 20. Main rule

The rule I keep is simple:

```text
known ROM
→ accepted

known ROM + correct metadata
→ extract and generate assets

unknown ROM
→ not supported until I analyze it
```

This lets me add ROM variants without removing the original integrity checks and without mixing ROM adaptation with the separate HD texture system.
