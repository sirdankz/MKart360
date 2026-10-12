# MKart360 — Adapting the Project for Translated ROMs

This guide explains how I adapted the original **MKart360** project, which was designed to work with the USA ROM of Mario Kart 64, so that it can also work with a fan-translated ROM.

For the practical example in this guide, I use my **Mario Kart 64 PT-BR translation**.

The goal is to show the process in a way that can be repeated with another fan translation, even if the translation is different from mine.

---

## 1. What I am going to change

The original project was designed around a specific ROM:

```text
baserom.us.z64
```

and verifies that it matches the expected ROM.

The problem is that a translated ROM contains different bytes from the original ROM. Therefore, simply replacing the USA ROM with a translated ROM is not enough.

For my PT-BR adaptation, the main files I changed were:

```text
PUBLIC_BUILD_XBOX360.ps1

mk64-master/PUBLIC_PREPARE_MK64_ASSETS.py

mk64-master/PUBLIC_SOURCE_GOLD_HASHES.json

mk64-master/src/xbox360/xbox360_asset_loader.cpp

mk64-master/src/menu_items.c

mk64-master/assets/course_metadata/gCourseNames.inc.c
```

I also added:

```text
MKART360_TRANSLATION_ADAPTATION.md
```

The files above contain the functional changes needed for the adaptation.

---

## 2. First, I identify my ROM

Before changing the code, I need to identify exactly which ROM I am using.

I do not identify the ROM only by its filename.

For example:

```text
Mario Kart 64 [BR].z64
```

is not enough.

I need to obtain:

- File size
- CRC32
- SHA-1
- Optionally, MD5

These values allow me to identify the exact ROM.

### My PT-BR ROM

The ROM used for this example has:

```text
Size:    0xC00000
CRC32:   3B0D98C1
SHA-1:   c2baf5b4a5355fff2dac08e971a62834ef70268c
MD5:     d81760c53417ea97b04f311b02558aae
```

The size:

```text
0xC00000
```

corresponds to:

```text
12,582,912 bytes
12 MiB
```

---

## 3. How I find the information for another ROM

If I want to adapt another translation, I first calculate the information for that ROM.

On Windows PowerShell, I can calculate the SHA-1 with:

```powershell
Get-FileHash ".\MyROM.z64" -Algorithm SHA1
```

For CRC32, I can use a tool that reports the ROM's CRC32 or use a small Python script.

For example, I can create:

```text
rom_info.py
```

with:

```python
import sys
import hashlib
import zlib
from pathlib import Path

rom = Path(sys.argv[1])
data = rom.read_bytes()

print("ROM:", rom.name)
print("Size:", len(data), "bytes")
print("Size HEX:", hex(len(data)))
print("CRC32:", f"{zlib.crc32(data) & 0xFFFFFFFF:08X}")
print("SHA-1:", hashlib.sha1(data).hexdigest())
print("MD5:", hashlib.md5(data).hexdigest())
```

Then I run:

```powershell
python rom_info.py "MyROM.z64"
```

The result will look like:

```text
ROM: MyROM.z64
Size: 12582912 bytes
Size HEX: 0xc00000
CRC32: XXXXXXXX
SHA-1: XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
MD5: XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
```

These are the values I will use during the adaptation.

---

## 4. I compare the translated ROM with the original USA ROM

The original project expects:

```text
Size:    0xC00000
CRC32:   434389C1
```

My translated ROM has:

```text
Size:    0xC00000
CRC32:   3B0D98C1
SHA-1:   c2baf5b4a5355fff2dac08e971a62834ef70268c
```

This immediately shows that I cannot simply replace the USA ROM with the translated ROM.

I need to teach the project to recognize the new ROM.

---

# 5. I modify the ROM loader

I open:

```text
mk64-master/src/xbox360/xbox360_asset_loader.cpp
```

This file is responsible for loading the ROM on Xbox 360.

## Original code

The original project contains:

```c
#define X360_EXPECTED_ROM_CRC 0x434389C1U
```

The default ROM path is:

```c
#ifndef X360_ROM_PATH
#define X360_ROM_PATH "game:\\baserom.us.z64"
#endif
```

So the project expects:

```text
baserom.us.z64
```

It also checks the CRC:

```c
if((crc^0xFFFFFFFFU)!=X360_EXPECTED_ROM_CRC){
    x360_log("MK64: unsupported or corrupt local ROM\n");
    return 0;
}
```

If the CRC does not match, the ROM is rejected.

---

# 6. I add the CRC of my translated ROM

First, I add:

```c
#define X360_BR_ROM_CRC 0x3B0D98C1U
```

So I have:

```c
#define X360_EXPECTED_ROM_CRC 0x434389C1U
#define X360_BR_ROM_CRC 0x3B0D98C1U
```

If I am adapting another translation, I replace `0x3B0D98C1U` with the CRC32 of that ROM.

For example:

```c
#define X360_CUSTOM_ROM_CRC 0xXXXXXXXXU
```

---

# 7. I change the ROM filename

For my PT-BR example, I change:

```c
#define X360_ROM_PATH "game:\\baserom.us.z64"
```

to:

```c
#define X360_ROM_PATH "game:\\baserom.br.z64"
```

Therefore, the ROM file I use is:

```text
baserom.br.z64
```

If I want to adapt another translation, I can use another filename, for example:

```c
#define X360_ROM_PATH "game:\\baserom.custom.z64"
```

and place that file in the expected location.

---

# 8. I change the CRC validation

Originally there was only one accepted CRC:

```c
if((crc^0xFFFFFFFFU)!=X360_EXPECTED_ROM_CRC){
    x360_log("MK64: unsupported or corrupt local ROM\n");
    return 0;
}
```

I store the final CRC in a variable:

```c
unsigned int finalCrc=(crc^0xFFFFFFFFU);
```

Then I allow both ROMs:

```c
if(finalCrc!=X360_EXPECTED_ROM_CRC &&
   finalCrc!=X360_BR_ROM_CRC){
    x360_log("MK64: unsupported or corrupt local ROM\n");
    return 0;
}
```

Now the logic is:

```text
USA CRC
    -> accepted

BR CRC
    -> accepted

Any other CRC
    -> rejected
```

I am not removing the protection.

I am adding a second known ROM.

---

# 9. I identify which ROM was loaded

After validation, I add:

```c
romReady=true;

if(finalCrc==X360_BR_ROM_CRC)
    x360_log("MK64: local BR ROM validated\n");
else
    x360_log("MK64: local US ROM validated\n");

return 1;
```

This allows me to see in the log which ROM variant was detected.

---

# 10. I modify the asset preparation script

The next file is:

```text
mk64-master/PUBLIC_PREPARE_MK64_ASSETS.py
```

This change is very important.

The project does not only use the ROM at runtime.

During the build process, several pieces of data are extracted from the ROM and converted into generated C files.

The original script also checks whether the data extracted from the ROM matches the expected hashes of the USA ROM.

Therefore, changing only `xbox360_asset_loader.cpp` would not be enough.

---

# 11. I add the translated ROM identity

The original script contains:

```python
EXPECTED_ROM_SIZE = 0xC00000
EXPECTED_ROM_CRC32 = 0x434389C1
```

I add:

```python
BR_ROM_CRC32 = 0x3B0D98C1
BR_ROM_SHA1 = "c2baf5b4a5355fff2dac08e971a62834ef70268c"
```

So it becomes:

```python
EXPECTED_ROM_SIZE = 0xC00000
EXPECTED_ROM_CRC32 = 0x434389C1
BR_ROM_CRC32 = 0x3B0D98C1
BR_ROM_SHA1 = "c2baf5b4a5355fff2dac08e971a62834ef70268c"
```

For another translation, I replace these values with the CRC32 and SHA-1 I calculated for that ROM.

---

# 12. I make the script identify the translated ROM

The script calculates:

```python
got_sha1=hashlib.sha1(rom).hexdigest()
```

Then I identify my BR variant with:

```python
is_br_variant = (
    crc == BR_ROM_CRC32 and
    got_sha1 == BR_ROM_SHA1
)
```

This is important because I am not saying:

> Any ROM different from the USA ROM is valid.

I am saying:

> This specific ROM, identified by its CRC32 and SHA-1, is a supported variant.

For another translation, I create the same type of check using that ROM's values.

---

# 13. I change the ROM validation

Originally, the script only accepted the USA CRC.

I change the validation so that the USA ROM or my known BR ROM can be accepted:

```python
if crc != EXPECTED_ROM_CRC32 and not is_br_variant:
    die(
        f"ROM CRC32 mismatch: got {crc:08X}, "
        f"expected {EXPECTED_ROM_CRC32:08X} (USA) "
        f"or {BR_ROM_CRC32:08X} (BR)"
    )
```

Now the script accepts:

```text
USA
or
BR
```

and rejects unknown ROMs.

---

# 14. I handle the SHA-1 check

The original project contains:

```text
mk64-master/mk64.us.sha1
```

which stores the expected SHA-1 of the USA ROM.

I do not want the BR ROM to be compared against the USA SHA-1.

Therefore, I changed the logic so that the USA SHA-1 file is not used for the BR variant:

```python
if sha_file.is_file() and not is_br_variant:
```

The logic becomes:

```text
USA ROM
    -> verify against the USA SHA-1

BR ROM
    -> verify using the registered BR CRC32 + SHA-1
```

---

# 15. The most important part: asset hashes

The original project has SHA-256 hashes for data extracted from the ROM.

Originally, a check looks like:

```python
if got!=r['sha256']:
    die(...)
```

This means:

> The extracted bytes must exactly match the bytes expected from the original USA ROM.

My translated ROM contains different bytes.

Therefore, for the BR variant, I changed the check to:

```python
if got!=r['sha256'] and not is_br_variant:
    die(...)
```

I made the same type of adjustment in the other checks for ROM-derived data.

This allows the known BR ROM to have different extracted data and lets the assets be regenerated from the actual translated ROM.

---

# 16. I do not simply disable all validation

This is important.

I do not want to do something like:

```python
if False:
```

or remove all integrity checks.

Instead, I keep the original validation for the USA ROM and explicitly identify the translated ROM.

The logic is:

```text
USA ROM
    -> original checks remain active

Known BR ROM
    -> identified by CRC32 + SHA-1
    -> ROM-derived assets can be regenerated

Unknown ROM
    -> still rejected
```

---

# 17. I regenerate the assets

After making those changes, I can run:

```powershell
python PUBLIC_PREPARE_MK64_ASSETS.py --rom "baserom.br.z64"
```

The script uses the BR ROM to regenerate the derived data.

The important flow is:

```text
Translated ROM
       ↓
Extraction
       ↓
ROM-derived data
       ↓
Generated .c files
       ↓
Compilation
```

Therefore, I should not simply copy assets generated from the USA ROM and expect them to work correctly with the translated ROM.

---

# 18. I update the build script

Now I open:

```text
PUBLIC_BUILD_XBOX360.ps1
```

Originally it simply ran:

```powershell
py .\PUBLIC_PREPARE_MK64_ASSETS.py
```

This caused the preparation script to use the default USA ROM.

I changed it so that it looks for the BR ROM first:

```powershell
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
```

This makes the build process automatic.

---

# 19. If I want to adapt another translation

I can change:

```powershell
$BrRom = Join-Path $Source 'baserom.br.z64'
```

to:

```powershell
$CustomRom = Join-Path $Source 'baserom.custom.z64'
```

and then:

```powershell
if (Test-Path $CustomRom) {
    py .\PUBLIC_PREPARE_MK64_ASSETS.py --rom $CustomRom
}
```

The filename is only a convention.

The actual ROM identity comes from:

```text
Size
CRC32
SHA-1
```

---

# 20. I adapt the game's hardcoded text

After handling the ROM and the generated assets, there is another issue:

**some game text is stored directly in the source code.**

The main file I work with is:

```text
mk64-master/src/menu_items.c
```

The original project contains English strings such as:

```c
"RETURN TO GAME SELECT",
"SOUND MODE",
"COPY N64 CONTROLLER PAK",
"ERASE ALL DATA",
```

For my PT-BR example, I changed them to:

```c
"RETORNAR A SELEÇÃO DE JOGO",
"MODO DE SOM",
"COPIAR N64 CONTROLLER PAK",
"APAGAR TODOS OS DADOS",
```

I applied the same process to the other hardcoded messages in this file.

---

# 21. I do not simply use UTF-8

This is one of the most important parts.

The game's text system does not simply use standard UTF-8 for these characters.

Therefore, I cannot just take:

```text
NÃO
```

and paste it into the C source expecting the game to understand the characters correctly.

I need to determine which bytes the ROM uses to represent each special character.

For example, my adaptation contains byte sequences such as:

```c
"\xA4\xE3"
```

and:

```c
"\xA5\xA3"
```

These bytes are part of the character encoding used by the game/translation.

---

# 22. How I identify special characters for another translation

When adapting another ROM, I first locate the translated text inside that ROM.

For each special character, I determine:

```text
Character
    ↓
Bytes used by the ROM
    ↓
Corresponding glyph
```

For example, if the translation contains a special character that does not exist in the original character set, I need to determine how that translation encoded it.

I should not assume that another translation uses exactly the same byte sequences as my PT-BR translation.

---

# 23. I pay attention to `\x` escape sequences

There is another important C-language detail.

I should be careful with strings such as:

```c
"\xA5DA"
```

if my intention is to represent:

```text
A5 DA
```

The `\x` escape can continue consuming hexadecimal characters.

To separate the intended bytes, I can write:

```c
"\xA5" "DA"
```

The C compiler automatically concatenates adjacent string literals.

This was necessary for some of the strings in my adaptation.

---

# 24. I check the glyphs

Even if I know the correct byte sequence, the game still needs to have the graphical glyph for that character.

The project uses a glyph texture lookup table:

```c
MenuTexture* gGlyphTextureLUT[]
```

For my PT-BR translation, I needed to add glyphs that were not available in the required form.

I added:

```c
static unsigned char x360_font_Atilde[128] = {
    ...
};
```

and:

```c
static unsigned char x360_font_Eacute[128] = {
    ...
};
```

Then I created the corresponding textures:

```c
static MenuTexture x360_font_Atilde_texture[2] = {
    { 5, x360_font_Atilde, 16, 16, 0, 0, 0x0, 0 },
    { 0, NULL, 0, 0, 0, 0, 0, 0 },
};
```

and:

```c
static MenuTexture x360_font_Eacute_texture[2] = {
    { 5, x360_font_Eacute, 16, 16, 0, 0, 0x0, 0 },
    { 0, NULL, 0, 0, 0, 0, 0, 0 },
};
```

Then I added those textures to:

```c
gGlyphTextureLUT[]
```

with:

```c
&x360_font_Atilde_texture[0],
&x360_font_Eacute_texture[0],
```

---

# 25. I update the character decoder

Adding the glyph image is not enough.

The text system also needs to recognize the byte sequence and point it to the correct glyph.

The original code contains:

```c
case -92:
    index = func_80092E1C(character + 1);
    break;
```

I changed it to:

```c
case -92:
    if ((u8) character[1] == 0xE3) {
        index = 91;
    } else {
        index = func_80092E1C(character + 1);
    }
    break;
```

I did something similar for another special character:

```c
case -91:
    if ((u8) character[1] == 0xA3) {
        index = 92;
    } else {
        index = func_80092DF8(character + 1);
    }
    break;
```

This tells the text system how to recognize the special byte sequences used by my translation.

---

# 26. For another translation, I repeat this process

If another translation needs characters such as:

```text
ã
ç
é
ñ
á
ö
ü
```

I check each one individually:

```text
1. Does the character already exist?
2. Which byte/byte sequence does the ROM use?
3. Is there already a glyph for it?
4. If not, do I need to create one?
5. Do I need to add a texture?
6. Do I need to add a decoder rule?
```

I should not simply copy the Brazilian glyph mappings without checking the target ROM.

---

# 27. I update the track names

There is another file that I need to check:

```text
mk64-master/assets/course_metadata/gCourseNames.inc.c
```

This file contains the track names.

Originally:

```c
"mario raceway",
"choco mountain",
"bowser's castle",
"banshee boardwalk",
"yoshi valley",
...
```

For my PT-BR example, I changed them to:

```c
"circuito do mario",
"montanha choco",
"castelo do bowser",
"cal\xA4\xC3" "ad\xA4\xE3o fantasma",
"vale do yoshi",
...
```

I also adapted names that require special characters.

Therefore, when adapting another translation, I should not look only at `menu_items.c`.

I also need to check:

```text
assets/course_metadata/gCourseNames.inc.c
```

---

# 28. I update the source integrity hash

The project contains:

```text
mk64-master/PUBLIC_SOURCE_GOLD_HASHES.json
```

This file stores SHA-256 hashes for selected source files.

Because I changed:

```text
src/xbox360/xbox360_asset_loader.cpp
```

its hash changed.

Original:

```json
"src/xbox360/xbox360_asset_loader.cpp":
"1d21071ae526bc287f27097f68d211d082c3d9c21b5ea64d46be207384d478c8"
```

After my modification:

```json
"src/xbox360/xbox360_asset_loader.cpp":
"70a427097d48d4d18d25e9aee54d79de32c643325803b9aec584efb562e88b0f"
```

Therefore, whenever I intentionally modify a file that participates in this integrity check, I need to update its hash.

I should not change hashes for files I did not modify.

---

# 29. How I calculate the SHA-256

In PowerShell I can run:

```powershell
Get-FileHash ".\src\xbox360\xbox360_asset_loader.cpp" -Algorithm SHA256
```

I take the resulting hash and put it into:

```text
PUBLIC_SOURCE_GOLD_HASHES.json
```

For example:

```json
"src/xbox360/xbox360_asset_loader.cpp": "NEW_HASH_HERE"
```

---

# 30. I build the project

After all modifications, I place my ROM in the expected location:

```text
mk64-master/
    baserom.br.z64
```

Then I return to the project root and run:

```powershell
.\PUBLIC_BUILD_XBOX360.ps1
```

The process is now:

```text
PUBLIC_BUILD_XBOX360.ps1
        ↓
Find baserom.br.z64
        ↓
PUBLIC_PREPARE_MK64_ASSETS.py
        ↓
Identify CRC32 + SHA-1
        ↓
Extract data from ROM
        ↓
Generate assets
        ↓
MSBuild
        ↓
XEX
```

---

# 31. I test the result on Xbox 360

After generating the XEX, I test at least:

```text
[ ] Game starts
[ ] ROM is recognized
[ ] Main menu
[ ] Character selection
[ ] Cup selection
[ ] Track names
[ ] Translated menus
[ ] Accented characters
[ ] Pause
[ ] Options
[ ] Controller Pak
[ ] Ghost
[ ] Save
[ ] Error messages
```

The accented characters deserve special attention because an incorrect mapping can make a character display incorrectly or disappear entirely.

---

# 32. Complete workflow for adapting another translated ROM

If I want to adapt another Mario Kart 64 fan translation, I follow this sequence:

```text
1. Get the translated ROM
        ↓
2. Determine its size
        ↓
3. Determine its CRC32
        ↓
4. Determine its SHA-1
        ↓
5. Add the CRC/SHA-1 to the project
        ↓
6. Modify xbox360_asset_loader.cpp
        ↓
7. Modify PUBLIC_PREPARE_MK64_ASSETS.py
        ↓
8. Regenerate the ROM-derived assets
        ↓
9. Find hardcoded text
        ↓
10. Adapt menu_items.c
        ↓
11. Identify special characters
        ↓
12. Add missing glyphs
        ↓
13. Modify the character decoder
        ↓
14. Adapt gCourseNames.inc.c
        ↓
15. Update hashes for intentionally modified source files
        ↓
16. Build
        ↓
17. Test on Xbox 360
```

---

# 33. Files I need to modify

| File | What I change |
|---|---|
| `PUBLIC_BUILD_XBOX360.ps1` | Make the build find and pass the correct ROM to the asset preparation script |
| `mk64-master/PUBLIC_PREPARE_MK64_ASSETS.py` | Add identification of the new ROM and allow its ROM-derived assets to be regenerated |
| `mk64-master/src/xbox360/xbox360_asset_loader.cpp` | Make the Xbox 360 loader recognize the new ROM CRC |
| `mk64-master/src/menu_items.c` | Translate hardcoded text, add glyphs, and handle special characters |
| `mk64-master/assets/course_metadata/gCourseNames.inc.c` | Translate the track names |
| `mk64-master/PUBLIC_SOURCE_GOLD_HASHES.json` | Update SHA-256 values for source files I intentionally changed |
| `MKART360_TRANSLATION_ADAPTATION.md` | Document the adaptation |

---

# 34. What I do not need to change

I do not need to modify hundreds of project files just because a file comparison reports differences.

The functional changes are concentrated in the files listed above.

I also do not need to remove the ROM integrity checks.

The goal is to add a **known supported ROM variant**.

The final logic is:

```text
Known USA ROM
    ↓
Accepted

Known PT-BR ROM
    ↓
Accepted

Unknown ROM
    ↓
Rejected
```

---

# 35. The most important information when adapting another ROM

The first three pieces of information I need are:

```text
SIZE
CRC32
SHA-1
```

For my PT-BR example:

```text
Size:
0xC00000

CRC32:
3B0D98C1

SHA-1:
c2baf5b4a5355fff2dac08e971a62834ef70268c
```

After that, I need to identify what differences the translation introduced into the game's data and text.

The main areas are:

```text
ROM
 ↓
ROM-derived assets
 ↓
Hardcoded text
 ↓
Character encoding
 ↓
Glyphs
 ↓
Track names
```

That combination is what allows me to adapt MKart360 to a fan-translated ROM without simply removing the project's existing integrity checks.

---

# 15. Updating PUBLIC_SOURCE_GOLD_HASHES.json

`PUBLIC_SOURCE_GOLD_HASHES.json` contains SHA-256 hashes of selected project source files. These are not the CRC32 or SHA-1 of the ROM, and they are not the hashes of the ROM-derived assets.

After making intentional changes to source files listed in this JSON, update their SHA-256 values. Do not disable the checks and do not replace the JSON with hashes copied from another project version.

From the project root, run:

```powershell
py .\UPDATE_PUBLIC_SOURCE_GOLD_HASHES.py --check
```

This only checks the current hashes and reports files that changed or are missing. If the reported changes are intentional, run:

```powershell
py .\UPDATE_PUBLIC_SOURCE_GOLD_HASHES.py --update
```

Then review the change:

```powershell
git diff -- mk64-master/PUBLIC_SOURCE_GOLD_HASHES.json
```

The script only updates paths already listed in the JSON. It does not add every project file to the list. If a listed file is missing, it stops instead of silently updating an incomplete list.

This script only solves source-file hash mismatches. It does not update `PUBLIC_ASSET_RECIPES.json` or `PUBLIC_GENERATED_BANK_MAP.json`, which verify ROM-derived data. Those files must only be changed when the adaptation actually requires different generated data, and the resulting data must be verified rather than blindly accepting any mismatch.

---

# 16. Recommended adaptation workflow

1. Run `rom_info.py` on the exact ROM you intend to use.
2. Record its size, CRC32 and SHA-1.
3. Follow the earlier sections to adapt the loader, ROM preparation, build script, translated strings, glyphs and course names.
4. Regenerate and verify ROM-derived assets if the new ROM changes data used by those assets.
5. Run `UPDATE_PUBLIC_SOURCE_GOLD_HASHES.py --check`.
6. If intentional source edits changed registered files, run `UPDATE_PUBLIC_SOURCE_GOLD_HASHES.py --update`.
7. Review the JSON diff and build the XEX.
8. Test the game, translated text and special characters on the target setup.

The PT-BR CRC and SHA-1 in this guide are examples for that exact ROM only. Never copy those values to another ROM.
