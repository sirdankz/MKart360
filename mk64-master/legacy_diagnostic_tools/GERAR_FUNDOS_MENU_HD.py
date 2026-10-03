#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Assistente: fundos coloridos dos menus em HD (duplo-clique ou py).
Wizard: HD tinted menu backgrounds (double-click or run with py).

Junta, num passo so, a extracao dos PNGs originais da ROM (pasta separada,
sem tocar em extracted_textures) e o TINT_MENU_BACKGROUNDS.py.
"""
from pathlib import Path
import subprocess, sys

PASTA_ORIG = "extracted_originais"
FUNDOS_REL = ("generated/texture_tkmk00/background_blue_sky.png",
              "generated/texture_tkmk00/background_sunset.png")

TXT = {
    "pt": {
        "title": "FUNDOS COLORIDOS DOS MENUS EM HD",
        "intro": (
            "As telas de selecao de modo (rosa), de personagens (verde) e de\n"
            "pistas (azul) usam a mesma imagem de fundo da tela inicial, que o\n"
            "jogo converte e tinge na hora. Este assistente:\n\n"
            "  1. extrai da sua ROM os PNGs originais para a pasta separada\n"
            f"     '{PASTA_ORIG}' (a sua extracted_textures nao e alterada);\n"
            "  2. gera os tres fundos coloridos em HD a partir da SUA arte HD do\n"
            "     fundo da tela inicial, com os hashes que o jogo calcula.\n\n"
            "Antes de continuar, confira:\n"
            "  - a arte HD do fundo ja esta em extracted_textures\\generated\\\n"
            "    texture_tkmk00\\background_blue_sky.png (e/ou background_sunset.png);\n"
            "  - use a MESMA ROM com que o jogo roda no console."
        ),
        "no_root": "Nao encontrei EXTRACT_MK64_TEXTURES.py e TINT_MENU_BACKGROUNDS.py aqui.\n"
                   "Coloque este arquivo dentro da pasta mk64-master.",
        "roms_found": "ROMs encontradas nesta pasta:",
        "rom_pick": "Digite o numero da ROM, ou o caminho de outra ROM: ",
        "rom_ask": "Digite o caminho/nome da ROM (ex.: baserom.us.z64): ",
        "rom_missing": "Arquivo nao encontrado: {p}. Tente de novo.",
        "orig_exists": f"A pasta '{PASTA_ORIG}' ja tem os fundos originais.",
        "reuse": "Usar a extracao existente (s) ou extrair de novo (n)? [s/n]: ",
        "yes": ("s", "sim", "y", "yes", ""),
        "extracting": "\n[1/2] Extraindo os originais da ROM (pode levar alguns minutos)...",
        "extract_fail": "A extracao falhou (codigo {c}). Confira se a ROM e valida.",
        "extract_skip": "\n[1/2] Usando a extracao existente.",
        "tinting": "\n[2/2] Gerando os fundos coloridos...",
        "tint_fail": "A geracao falhou (codigo {c}). Veja as mensagens acima.",
        "done": (
            "\nPRONTO!\n\n"
            "Os fundos coloridos em HD estao em:\n"
            "  extracted_textures\\generated\\texture_tkmk00\\tinted\\\n"
            "    __modo (rosa), __personagem (verde), __pista (azul)\n"
            "Voce pode retoca-los; para regerar um, apague o PNG e rode de novo.\n\n"
            "Proximo passo -- empacotar e copiar o tex.pak para o console:\n"
            "  py .\\PACK_TEXTURES.py --pak\n\n"
            f"A pasta '{PASTA_ORIG}' contem dados da ROM: nao a envie ao GitHub."
        ),
        "enter": "\nPressione Enter para sair...",
    },
    "en": {
        "title": "HD TINTED MENU BACKGROUNDS",
        "intro": (
            "The mode select (pink), character select (green) and course select\n"
            "(blue) screens use the same background as the title screen, which\n"
            "the game converts and tints at runtime. This wizard:\n\n"
            "  1. extracts the original PNGs from your ROM into the separate\n"
            f"     '{PASTA_ORIG}' folder (your extracted_textures is not touched);\n"
            "  2. builds the three tinted backgrounds in HD from YOUR HD art of\n"
            "     the title background, with the hashes the game computes.\n\n"
            "Before continuing, make sure that:\n"
            "  - the HD background art is already at extracted_textures\\generated\\\n"
            "    texture_tkmk00\\background_blue_sky.png (and/or background_sunset.png);\n"
            "  - you use the SAME ROM the game runs with on the console."
        ),
        "no_root": "Could not find EXTRACT_MK64_TEXTURES.py and TINT_MENU_BACKGROUNDS.py here.\n"
                   "Put this file inside the mk64-master folder.",
        "roms_found": "ROMs found in this folder:",
        "rom_pick": "Type the ROM number, or the path to another ROM: ",
        "rom_ask": "Type the ROM path/name (e.g. baserom.us.z64): ",
        "rom_missing": "File not found: {p}. Try again.",
        "orig_exists": f"The '{PASTA_ORIG}' folder already has the original backgrounds.",
        "reuse": "Reuse the existing extraction (y) or extract again (n)? [y/n]: ",
        "yes": ("y", "yes", "s", "sim", ""),
        "extracting": "\n[1/2] Extracting the originals from the ROM (may take a few minutes)...",
        "extract_fail": "Extraction failed (code {c}). Check that the ROM is valid.",
        "extract_skip": "\n[1/2] Reusing the existing extraction.",
        "tinting": "\n[2/2] Building the tinted backgrounds...",
        "tint_fail": "Generation failed (code {c}). See the messages above.",
        "done": (
            "\nDONE!\n\n"
            "The HD tinted backgrounds are in:\n"
            "  extracted_textures\\generated\\texture_tkmk00\\tinted\\\n"
            "    __modo (pink), __personagem (green), __pista (blue)\n"
            "You can touch them up; to regenerate one, delete the PNG and run again.\n\n"
            "Next step -- pack and copy tex.pak to the console:\n"
            "  py .\\PACK_TEXTURES.py --pak\n\n"
            f"The '{PASTA_ORIG}' folder contains ROM data: don't upload it to GitHub."
        ),
        "enter": "\nPress Enter to exit...",
    },
}


def sair(t, codigo=0):
    input(t["enter"])
    sys.exit(codigo)


def escolher_rom(root, t):
    roms = sorted(root.glob("*.z64")) + sorted(root.glob("*.n64")) + sorted(root.glob("*.v64"))
    while True:
        if roms:
            print("\n" + t["roms_found"])
            for i, r in enumerate(roms, 1):
                print(f"  {i}) {r.name}")
            resp = input(t["rom_pick"]).strip().strip('"')
            if resp.isdigit() and 1 <= int(resp) <= len(roms):
                return roms[int(resp) - 1]
        else:
            resp = input(t["rom_ask"]).strip().strip('"')
        p = Path(resp)
        if not p.is_absolute():
            p = root / p
        if resp and p.is_file():
            return p
        print(t["rom_missing"].format(p=resp))


def main():
    while True:
        esc = input("Idioma / Language:  1) Portugues (Brasil)   2) English\n> ").strip()
        if esc in ("1", "2"):
            break
    t = TXT["pt" if esc == "1" else "en"]

    print("\n" + "=" * 68 + f"\n{t['title']}\n" + "=" * 68 + "\n")
    print(t["intro"])

    root = Path(__file__).resolve().parent
    if not ((root / "EXTRACT_MK64_TEXTURES.py").is_file() and
            (root / "TINT_MENU_BACKGROUNDS.py").is_file()):
        print("\n" + t["no_root"])
        sair(t, 1)

    orig = root / PASTA_ORIG
    ja_tem = any((orig / rel).is_file() for rel in FUNDOS_REL)
    extrair = True
    if ja_tem:
        print("\n" + t["orig_exists"])
        extrair = input(t["reuse"]).strip().lower() not in t["yes"]

    if extrair:
        rom = escolher_rom(root, t)
        print(t["extracting"])
        r = subprocess.run([sys.executable, str(root / "EXTRACT_MK64_TEXTURES.py"),
                            "--rom", str(rom), "--out", PASTA_ORIG, "--no-karts"], cwd=root)
        if r.returncode != 0:
            print(t["extract_fail"].format(c=r.returncode))
            sair(t, 1)
    else:
        print(t["extract_skip"])

    print(t["tinting"])
    r = subprocess.run([sys.executable, str(root / "TINT_MENU_BACKGROUNDS.py"),
                        "--originais", PASTA_ORIG], cwd=root)
    if r.returncode != 0:
        print(t["tint_fail"].format(c=r.returncode))
        sair(t, 1)

    print(t["done"])
    sair(t)


if __name__ == "__main__":
    main()
