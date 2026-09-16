# MKart360 Translation Adaptation

This documents the changes made to get the Xbox 360 port working with a
fan-translated Mario Kart 64 ROM.

The reference ROM used during development was a Brazilian Portuguese
translation. The ROM itself is not included in this repository.

The idea here is to document the parts that had to be changed so the same
process can be used with another fan translation.

## ROM used for the BR build

The original project expected the USA ROM:

```text
baserom.us.z64
CRC32: 434389C1
```

The translated ROM used for this work was:

```text
Mario Kart 64 [BR].z64

Size:    0xC00000
CRC32:   3B0D98C1
SHA-1:   c2baf5b4a5355fff2dac08e971a62834ef70268c
MD5:     d81760c53417ea97b04f311b02558aae
```

The ROM is a normal big-endian `.z64`.

For another translation, the CRC and hashes will be different and should
be replaced with the values from that ROM.

---

## 1. ROM loader

The first problem was the Xbox 360 loader itself.

It was hardcoded for the USA ROM.

### Before

`mk64-master/src/xbox360/xbox360_asset_loader.cpp`

```c
#define X360_EXPECTED_ROM_CRC 0x434389C1U

#ifndef X360_ROM_PATH
#define X360_ROM_PATH "game:\\baserom.us.z64"
#endif
```

The loader calculated the ROM CRC and rejected anything that did not match
the USA value.

### After

The BR ROM was added as another accepted ROM:

```c
#define X360_EXPECTED_ROM_CRC 0x434389C1U
#define X360_BR_ROM_CRC       0x3B0D98C1U

#ifndef X360_ROM_PATH
#define X360_ROM_PATH "game:\\baserom.br.z64"
#endif
```

The CRC check was changed to accept either ROM:

```cpp
unsigned int finalCrc = (crc ^ 0xFFFFFFFFU);

if (finalCrc != X360_EXPECTED_ROM_CRC &&
    finalCrc != X360_BR_ROM_CRC) {
    x360_log("MK64: unsupported or corrupt local ROM\n");
    return 0;
}

romReady = true;

if (finalCrc == X360_BR_ROM_CRC)
    x360_log("MK64: local BR ROM validated\n");
else
    x360_log("MK64: local US ROM validated\n");
```

The ROM check was not removed. The translated ROM was simply added as a
supported variant.

---

## 2. ROM-derived assets

Changing the loader was not enough.

Some of the assets used by the Xbox 360 build are generated from the ROM.
If the source ROM is different, those generated files can also be
different.

The asset preparation script is:

```text
mk64-master/PUBLIC_PREPARE_MK64_ASSETS.py
```

The original script expected the USA ROM and its known hashes.

It was changed so the BR ROM is accepted as a valid source as well.

The BR preparation regenerated:

```text
627 excluded media source fragments
13 generated-bank C files / 3736 objects
20 course linkonly C/header pairs
```

The important part is that these files were generated from the translated
ROM, not copied from the USA build.

For another fan translation, the same preparation should be done with that
translation's ROM.

---

## 3. ROM hash checks

The project has source and asset integrity checks.

These checks were kept.

When the Xbox 360 loader was intentionally changed, its authorized hash
also had to be updated in:

```text
mk64-master/PUBLIC_SOURCE_GOLD_HASHES.json
```

This is preferable to disabling the integrity checks.

The change was only made for the source file that was intentionally
modified.

---

## 4. Hardcoded menu text

After the translated ROM was accepted and the assets were regenerated,
some text was still English.

This happened because some strings are hardcoded in the source.

The main file is:

```text
mk64-master/src/menu_items.c
```

For example, the original source contained:

```c
char* gTextOptionMenu[] = {
    "RETURN TO GAME SELECT",
    "SOUND MODE",
    "COPY N64 CONTROLLER PAK",
    "ERASE ALL DATA",
};
```

These strings were replaced with the corresponding strings from the
translated ROM.

The BR version includes text such as:

```text
RETORNAR À SELEÇÃO DE JOGO
MODO DE SOM
COPIAR N64 CONTROLLER PAK
APAGAR TODOS OS DADOS
```

The important part is not the Portuguese text itself. The bytes used by
the translated ROM need to be preserved.

---

## 5. Pause menu

The pause menu also contained hardcoded English strings.

The BR version uses:

```text
CONTINUAR JOGO
TENTAR DE NOVO
MUDAR PERCURSO
MUDAR PILOTO
SAIR
SALVAR FANTASMA
```

These were changed in the same source area.

This is why changing only the main Options menu does not translate the
whole game.

When adapting another translation, these strings should be replaced with
the corresponding strings from that ROM.

---

## 6. Cup names

Cup names were another separate group of strings.

The BR version uses:

```text
COPA COGUMELO
COPA FLOR
COPA ESTRELA
COPA ESPECIAL
BATALHA
```

The original English names were therefore not left in the Xbox 360 build.

For another translation, use the names from that translation instead.

---

## 7. Course names

Course names are handled separately from the normal menu strings.

The source file is:

```text
mk64-master/assets/course_metadata/gCourseNames.inc.c
```

The BR version contains names such as:

```text
circuito do mario
montanha choco
vale do yoshi
terra da neve
praia koopa troopa
circuito real
circuito do luigi
fazenda moo moo
rodovia do toad
deserto kalimari
terra do sorvete
estrada do arco-íris
estádio do wario
forte de bloco
plataforma dupla
selva do d.k.
frigideira
```

This file is easy to miss because it is separate from `menu_items.c`.

When another translation is used, the course names have to be checked
separately.

---

## 8. Character encoding

The translated ROM does not store these strings as normal UTF-8.

The game uses its own character encoding. Extended byte values are used
for characters that are not part of the normal ASCII range.

For example, the source can contain bytes like:

```c
"\xA4"
"\xA5"
"\xA1"
```

These values came from the translated ROM.

They should not simply be replaced with UTF-8 text.

For example, writing:

```c
"Ã"
"É"
"Ç"
```

does not automatically produce the same bytes expected by the game's
renderer.

The safest method is to look at the actual bytes in the translated ROM
and use those bytes in the source.

---

## 9. Accented characters and glyphs

The character bytes are only part of the problem.

The renderer also needs the correct glyph for the character.

This became noticeable with words such as:

```text
BOTÃO
SELEÇÃO
ESTÉREO
```

At one point the strings were present, but the accented characters were
not displayed correctly.

The glyph mapping was then corrected for the characters used by the BR
translation.

After that, the text displayed correctly on the Xbox 360.

Another translation may need different glyphs depending on which
characters it uses.

---

## 10. C hexadecimal escape problem

There was also a compiler issue caused by C hexadecimal escapes.

For example:

```c
"\xA5DA"
```

does not necessarily mean:

```text
A5 + DA
```

The `\x` escape keeps reading hexadecimal characters, so the compiler can
treat the whole thing as one large character value.

This caused errors such as:

```text
C2022: '50093' : too big for character
```

The fix is to stop the escape before the following hexadecimal characters:

```c
"\xA5" "DA"
```

C joins adjacent string literals automatically.

The same problem happened in `gCourseNames.inc.c`, not only in
`menu_items.c`.

After fixing the strings, the source was checked for ambiguous hexadecimal
escapes.

The final check found:

```text
0 remaining ambiguous hex escapes
```

A syntax-only C check was also successful:

```text
clang -std=c11 -Wall -Wextra -Werror -fsyntax-only
```

---

## 11. Checking the translated strings

The strings were compared against the bytes found in the translated ROM.

The check resulted in:

```text
source nonempty: 106
rom nonempty:    196
matches:         106
```

This helped confirm that the source strings were using the same byte
sequences as the ROM.

It also helped catch differences in:

- accents
- spaces
- spelling
- character bytes

This is a useful step when adapting another translation.

---

## 12. Files changed

The main project files involved in this adaptation are:

```text
PUBLIC_BUILD_XBOX360.ps1

mk64-master/PUBLIC_PREPARE_MK64_ASSETS.py

mk64-master/PUBLIC_SOURCE_GOLD_HASHES.json

mk64-master/src/menu_items.c

mk64-master/src/xbox360/xbox360_asset_loader.cpp

mk64-master/assets/course_metadata/gCourseNames.inc.c
```

The asset preparation step can also regenerate additional files based on
the ROM.

Those generated files should come from the ROM being used for the build.

---

## 13. Build

After the changes, the Xbox 360 build can be started with:

```powershell
.\PUBLIC_BUILD_XBOX360.ps1
```

The resulting build should be tested on the Xbox 360.

The main things worth checking are:

```text
Main menus
Options menu
Pause menu
Cup names
Course names
Accented characters
Save messages
Controller Pak messages
Ghost messages
```

---

## 14. Adapting another translation

The same process can be used for another fan translation.

### 1. Identify the ROM

Get its:

```text
size
CRC32
SHA-1
```

### 2. Add the ROM to the asset preparation script

Use the new ROM information instead of the BR values.

### 3. Update the Xbox 360 loader

Add the new CRC and ROM path without removing the existing validation.

### 4. Regenerate the assets

Run the asset preparation using the translated ROM.

### 5. Find hardcoded text

Start with:

```text
mk64-master/src/menu_items.c
```

Then check the other source files and metadata.

### 6. Check course names

Check:

```text
mk64-master/assets/course_metadata/gCourseNames.inc.c
```

### 7. Check the encoding

Look at the actual bytes in the translated ROM.

Do not assume UTF-8.

### 8. Check glyphs

If accented or special characters do not display correctly, check the
glyph table and character-to-glyph mapping.

### 9. Check `\x` escapes

If a hexadecimal escape is followed immediately by another hexadecimal
character, split the string:

```c
"\xA4" "DA"
```

### 10. Build and test

Build the Xbox 360 version and test the menus and in-game text.

---

## Notes

The BR translation was used as the reference for this work, but the
project is not limited to Portuguese.

The translated ROM itself is not included in the repository.

Different translations can require different ROM hashes, strings, byte
values and glyphs.

The main idea is to use the translated ROM as the source for the
ROM-derived data and to make the Xbox 360 port use the same character data
and translated text where the port has hardcoded strings.
