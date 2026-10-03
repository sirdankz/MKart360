#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EXPERIMENTO BETA / BETA EXPERIMENT -- 60 FPS offline
=====================================================
Duplo-clique para abrir o assistente (portugues ou ingles).
Double-click to open the wizard (Portuguese or English).

Linha de comando / command line (dentro de mk64-master / inside mk64-master):
    py .\\EXPERIMENTO_60FPS.py status
    py .\\EXPERIMENTO_60FPS.py apply
    py .\\EXPERIMENTO_60FPS.py revert

O experimento altera dois arquivos (src\\main.c e src\\xbox360\\xbox360_video.cpp)
e guarda backups deles. Reverter devolve os dois exatamente ao original.
The experiment changes two files (src\\main.c and src\\xbox360\\xbox360_video.cpp)
and keeps backups. Reverting restores both exactly to the original.
"""
from pathlib import Path
import argparse, re, sys, shutil, datetime

MARKER = "X360_EXPERIMENTO_60FPS"
MAIN_REL = Path("src/main.c")
VIDEO_REL = Path("src/xbox360/xbox360_video.cpp")
MSBUILD = ('& "$env:WINDIR\\Microsoft.NET\\Framework\\v4.0.30319\\MSBuild.exe" '
           '".\\MK64.sln" /t:Rebuild "/p:Configuration=Release" "/p:Platform=Xbox 360"')

TXT = {
    "pt": {
        "title": "EXPERIMENTO BETA -- 60 FPS OFFLINE",
        "explain": (
            "Isto NAO e uma correcao oficial do port: e um teste.\n\n"
            "O QUE MUDA\n"
            "  Nas corridas OFFLINE de 1 ou 2 jogadores (Grand Prix, Time Trial, VS\n"
            "  local), o jogo passa a mostrar 60 quadros por segundo em vez de 30.\n"
            "  A fisica do jogo ja roda a 60 passos por segundo; o port original\n"
            "  so mostrava metade deles. A velocidade das corridas nao muda.\n\n"
            "O QUE NAO MUDA\n"
            "  Netplay, 3-4 jogadores e menus continuam a 30 FPS, como sempre.\n\n"
            "EFEITOS CONHECIDOS\n"
            "  Algumas animacoes atualizadas por quadro desenhado (balões, a forma\n"
            "  que gira na pausa, brilho das rodas) ficam no dobro da velocidade.\n"
            "  Nao afeta a jogabilidade.\n\n"
            "SEGURANCA\n"
            "  Sao alterados so src\\main.c e src\\xbox360\\xbox360_video.cpp, com\n"
            "  backup. Reverter devolve os dois exatamente ao original. Nada muda no\n"
            "  jogo ate voce recompilar."
        ),
        "no_root": "Nao encontrei src\\main.c a partir daqui. Coloque este arquivo dentro\n"
                   "da pasta mk64-master e abra de novo.",
        "project": "Projeto: {root}",
        "checking": "\nEstado atual:",
        "file_missing": "  {label}: ARQUIVO NAO ENCONTRADO",
        "file_applied": "  {label}: experimento APLICADO",
        "file_original": "  {label}: original",
        "backup_yes": "  (com backup)",
        "mixed": ("\nAVISO: os dois arquivos estao em estados diferentes. O mais seguro e\n"
                  "reverter e, se quiser, aplicar de novo."),
        "is_applied": "\n>>> O experimento de 60 FPS esta ATIVO nos seus arquivos. <<<",
        "is_original": "\n>>> O experimento de 60 FPS NAO esta aplicado (jogo original, 30 FPS). <<<",
        "ask_revert": "\nDeseja REVERTER e voltar ao original? (s/n): ",
        "ask_apply": "\nDeseja APLICAR o experimento de 60 FPS? (s/n): ",
        "yes": ("s", "sim", "y", "yes"),
        "cancelled": "\nNada foi alterado.",
        "applying": "\nAplicando...",
        "reverting": "\nRevertendo...",
        "bkp_saved": "  backup salvo: {name}",
        "bkp_kept": "  backup ja existia, mantido: {name}",
        "applied_ok": "  {rel}: experimento aplicado",
        "already": "  {rel}: ja estava aplicado",
        "restored": "  {rel}: restaurado do backup",
        "stripped": "  {rel}: sem backup -- trechos do experimento removidos",
        "already_orig": "  {rel}: ja estava no original",
        "nothing": "  nada para reverter.",
        "anchor": ("\nERRO: o trecho esperado ({where}) nao foi encontrado em {rel}.\n"
                   "O arquivo pode ter sido modificado de outra forma, ou o projeto mudou\n"
                   "de versao. Nada foi alterado."),
        "done_apply": (
            "\nPRONTO -- experimento aplicado ({date}).\n\n"
            "PROXIMO PASSO: recompilar. Importante: o src\\main.c esta na trava de\n"
            "hashes do PUBLIC_BUILD_XBOX360.ps1, que RECUSA compilar com o experimento\n"
            "aplicado. Compile direto com o MSBuild, na pasta do MK64.sln:\n\n"
            "  {msbuild}\n\n"
            "Se notar algum problema, abra este arquivo de novo e escolha reverter."
        ),
        "done_revert": (
            "\nPRONTO -- arquivos de volta ao original.\n\n"
            "PROXIMO PASSO: recompilar (o .ps1 volta a funcionar normalmente), ou:\n\n"
            "  {msbuild}"
        ),
        "oficial": (
            "\n>>> Esta versao do projeto JA TEM 60 FPS OFICIAL. <<<\n\n"
            "O port original (sirdankz/MKart360) passou a incluir 60 FPS nativo nas\n"
            "corridas offline (e tambem em sessoes online), ligado por padrao pela\n"
            "chave MK64_OFFLINE_60FPS_TEST no src\\main.c. Este experimento nao e\n"
            "necessario e nao sera aplicado -- nada foi alterado."
        ),
        "enter": "\nPressione Enter para sair...",
    },
    "en": {
        "title": "BETA EXPERIMENT -- 60 FPS OFFLINE",
        "explain": (
            "This is NOT an official port fix: it's a test.\n\n"
            "WHAT CHANGES\n"
            "  In OFFLINE races with 1 or 2 players (Grand Prix, Time Trial, local\n"
            "  VS), the game shows 60 frames per second instead of 30. The game's\n"
            "  physics already runs at 60 steps per second; the original port only\n"
            "  showed half of them. Race speed is unchanged.\n\n"
            "WHAT STAYS THE SAME\n"
            "  Netplay, 3-4 players and menus keep running at 30 FPS, as always.\n\n"
            "KNOWN SIDE EFFECTS\n"
            "  Some animations updated per drawn frame (balloons, the spinning shape\n"
            "  on the pause screen, wheel glow) run at double speed. Gameplay is not\n"
            "  affected.\n\n"
            "SAFETY\n"
            "  Only src\\main.c and src\\xbox360\\xbox360_video.cpp are changed, with\n"
            "  backups. Reverting restores both exactly to the original. Nothing\n"
            "  changes in the game until you rebuild."
        ),
        "no_root": "Could not find src\\main.c from here. Put this file inside the\n"
                   "mk64-master folder and open it again.",
        "project": "Project: {root}",
        "checking": "\nCurrent state:",
        "file_missing": "  {label}: FILE NOT FOUND",
        "file_applied": "  {label}: experiment APPLIED",
        "file_original": "  {label}: original",
        "backup_yes": "  (backed up)",
        "mixed": ("\nWARNING: the two files are in different states. The safest option is to\n"
                  "revert and, if you want, apply again."),
        "is_applied": "\n>>> The 60 FPS experiment is ACTIVE in your files. <<<",
        "is_original": "\n>>> The 60 FPS experiment is NOT applied (original game, 30 FPS). <<<",
        "ask_revert": "\nDo you want to REVERT back to the original? (y/n): ",
        "ask_apply": "\nDo you want to APPLY the 60 FPS experiment? (y/n): ",
        "yes": ("y", "yes", "s", "sim"),
        "cancelled": "\nNothing was changed.",
        "applying": "\nApplying...",
        "reverting": "\nReverting...",
        "bkp_saved": "  backup saved: {name}",
        "bkp_kept": "  backup already existed, kept: {name}",
        "applied_ok": "  {rel}: experiment applied",
        "already": "  {rel}: was already applied",
        "restored": "  {rel}: restored from backup",
        "stripped": "  {rel}: no backup -- experiment code removed",
        "already_orig": "  {rel}: was already original",
        "nothing": "  nothing to revert.",
        "anchor": ("\nERROR: the expected code ({where}) was not found in {rel}.\n"
                   "The file may have been modified some other way, or the project changed\n"
                   "version. Nothing was changed."),
        "done_apply": (
            "\nDONE -- experiment applied ({date}).\n\n"
            "NEXT STEP: rebuild. Important: src\\main.c is in the hash lock of\n"
            "PUBLIC_BUILD_XBOX360.ps1, which REFUSES to build with the experiment\n"
            "applied. Build directly with MSBuild, from the MK64.sln folder:\n\n"
            "  {msbuild}\n\n"
            "If you notice any problem, open this file again and choose revert."
        ),
        "done_revert": (
            "\nDONE -- files are back to the original.\n\n"
            "NEXT STEP: rebuild (the .ps1 works normally again), or:\n\n"
            "  {msbuild}"
        ),
        "oficial": (
            "\n>>> This version of the project ALREADY HAS OFFICIAL 60 FPS. <<<\n\n"
            "The original port (sirdankz/MKart360) now includes native 60 FPS in\n"
            "offline races (and in online sessions too), enabled by default by the\n"
            "MK64_OFFLINE_60FPS_TEST switch in src\\main.c. This experiment is not\n"
            "needed and will not be applied -- nothing was changed."
        ),
        "enter": "\nPress Enter to exit...",
    },
}

T = TXT["pt"]          # idioma atual; o assistente troca conforme a escolha
INTERATIVO = False


class Falha(Exception):
    pass


def falha(where, rel):
    raise Falha(T["anchor"].format(where=where, rel=rel))


def backup_path(p):
    return p.with_suffix(p.suffix + ".exp60bak")


def is_patched(text):
    return MARKER in text


# ---- alteracoes (mesma logica testada da versao anterior) -------------------
def patch_main_c(text):
    if is_patched(text):
        return text, False
    anchor1 = "s32 gGamestate = 0xFFFF;\n"
    if text.count(anchor1) != 1:
        falha("gGamestate", MAIN_REL)
    injection = f'''#ifdef XBOX360_PORT
/* ===== EXPERIMENTO BETA: 60 FPS offline (aplicado por EXPERIMENTO_60FPS.py) =====
   Corridas offline de 1-2 jogadores a 60 FPS (um passo de simulacao por
   quadro exibido, em vez de dois). Netplay, 3-4 jogadores e menus continuam
   a 30 FPS. Para desfazer: abra EXPERIMENTO_60FPS.py e escolha reverter. */
#define {MARKER} 1
int x360_exp60_active = 0;
int x360_exp60_now(void) {{ return x360_exp60_active && gGamestate == RACING; }}
#endif
'''
    text = text.replace(anchor1, anchor1 + injection, 1)

    anchor2 = ("    if (sNumVBlanks < 0) {\n"
               "        sNumVBlanks = 1;\n"
               "    }\n"
               "    func_802A4EF4();\n")
    if text.count(anchor2) != 1:
        falha("race_logic_loop", MAIN_REL)
    decide = f'''#if defined(XBOX360_PORT) && {MARKER}
    x360_exp60_active = !x360_net_active() && !x360_net8_active() &&
        (gActiveScreenMode == SCREEN_MODE_1P ||
         gActiveScreenMode == SCREEN_MODE_2P_SPLITSCREEN_VERTICAL ||
         gActiveScreenMode == SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL);
#endif
'''
    text = text.replace(anchor2, anchor2 + decide, 1)

    tick = f'''#if defined(XBOX360_PORT) && {MARKER}
            if (x360_exp60_active) gTickSpeed = 1;  /* EXPERIMENTO 60 FPS */
#endif
'''
    anchor3 = "        case SCREEN_MODE_1P:\n            gTickSpeed = 2;\n"
    if text.count(anchor3) != 1:
        falha("SCREEN_MODE_1P", MAIN_REL)
    text = text.replace(anchor3, anchor3 + tick, 1)

    pat = re.compile(r"(case SCREEN_MODE_2P_SPLITSCREEN_(?:VERTICAL|HORIZONTAL):.*?"
                     r"gTickSpeed = 2;\n            \}\n#endif\n)", re.S)
    n = 0

    def rep(m):
        nonlocal n
        n += 1
        return m.group(1) + tick

    text = pat.sub(rep, text)
    if n != 2:
        falha("SCREEN_MODE_2P", MAIN_REL)
    return text, True


def unpatch_main_c(text):
    text = re.sub(r"#ifdef XBOX360_PORT\n/\* ===== EXPERIMENTO BETA.*?#endif\n",
                  "", text, count=1, flags=re.S)
    text = re.sub(rf"#if defined\(XBOX360_PORT\) && {MARKER}\n"
                  rf"    x360_exp60_active = .*?\n#endif\n", "", text, flags=re.S)
    text = re.sub(rf"#if defined\(XBOX360_PORT\) && {MARKER}\n"
                  rf"            if \(x360_exp60_active\) gTickSpeed = 1;.*?\n#endif\n",
                  "", text)
    return text


def patch_video_cpp(text):
    if is_patched(text):
        return text, False
    anchor1 = 'extern "C" void x360_present_and_pace(void) {'
    if text.count(anchor1) != 1:
        falha("x360_present_and_pace", VIDEO_REL)
    text = text.replace(anchor1, f'extern "C" int x360_exp60_now(void);  /* {MARKER} (main.c) */\n'
                        + anchor1, 1)
    anchor2 = "    LONGLONG frame=g_clock_frequency.QuadPart/30;"
    if text.count(anchor2) != 1:
        falha("30 FPS pacing", VIDEO_REL)
    text = text.replace(anchor2, "    LONGLONG frame=g_clock_frequency.QuadPart/"
                                 "(x360_exp60_now()?60:30);", 1)
    return text, True


def unpatch_video_cpp(text):
    text = re.sub(rf'extern "C" int x360_exp60_now\(void\);  /\* {MARKER} \(main\.c\) \*/\n',
                  "", text)
    return text.replace("    LONGLONG frame=g_clock_frequency.QuadPart/(x360_exp60_now()?60:30);",
                        "    LONGLONG frame=g_clock_frequency.QuadPart/30;", 1)


# ---- acoes -------------------------------------------------------------------
def find_root():
    here = Path(__file__).resolve().parent
    for base in (here, *here.parents):
        if (base / MAIN_REL).is_file():
            return base
    return None


def tem_60fps_oficial(root):
    p = root / MAIN_REL
    return p.is_file() and "mk64_offline_60fps_active" in p.read_text(encoding="utf-8", errors="ignore")


def estado(root):
    print(T["checking"])
    vals = []
    for label, rel in (("main.c", MAIN_REL), ("xbox360_video.cpp", VIDEO_REL)):
        p = root / rel
        if not p.is_file():
            print(T["file_missing"].format(label=label))
            continue
        ap = is_patched(p.read_text(encoding="utf-8", errors="ignore"))
        vals.append(ap)
        print((T["file_applied"] if ap else T["file_original"]).format(label=label)
              + (T["backup_yes"] if backup_path(p).is_file() else ""))
    if len(set(vals)) > 1:
        print(T["mixed"])
    return bool(vals) and all(vals), bool(vals) and any(vals)


def aplicar(root):
    print(T["applying"])
    novos = {}
    for rel, fn in ((MAIN_REL, patch_main_c), (VIDEO_REL, patch_video_cpp)):
        p = root / rel
        text = p.read_text(encoding="utf-8", errors="ignore")
        novos[rel] = fn(text)           # valida TUDO antes de gravar qualquer coisa
    for rel, (new_text, changed) in novos.items():
        p = root / rel
        if not changed:
            print(T["already"].format(rel=rel))
            continue
        bkp = backup_path(p)
        if not bkp.is_file():
            shutil.copy2(p, bkp)
            print(T["bkp_saved"].format(name=bkp.name))
        else:
            print(T["bkp_kept"].format(name=bkp.name))
        p.write_text(new_text, encoding="utf-8")
        print(T["applied_ok"].format(rel=rel))
    print(T["done_apply"].format(date=datetime.date.today(), msbuild=MSBUILD))


def reverter(root):
    print(T["reverting"])
    feito = False
    for rel in (MAIN_REL, VIDEO_REL):
        p = root / rel
        bkp = backup_path(p)
        if bkp.is_file():
            shutil.copy2(bkp, p)
            print(T["restored"].format(rel=rel))
            feito = True
        elif p.is_file():
            text = p.read_text(encoding="utf-8", errors="ignore")
            if is_patched(text):
                p.write_text(unpatch_main_c(text) if rel == MAIN_REL else unpatch_video_cpp(text),
                             encoding="utf-8")
                print(T["stripped"].format(rel=rel))
                feito = True
            else:
                print(T["already_orig"].format(rel=rel))
    if feito:
        print(T["done_revert"].format(msbuild=MSBUILD))
    else:
        print(T["nothing"])


def sim(resp):
    return resp.strip().lower() in T["yes"]


def assistente():
    global T
    while True:
        esc = input("Idioma / Language:  1) Portugues (Brasil)   2) English\n> ").strip()
        if esc in ("1", "2"):
            break
    T = TXT["pt" if esc == "1" else "en"]
    print("\n" + "=" * 70 + f"\n{T['title']}\n" + "=" * 70 + "\n")
    print(T["explain"])
    root = find_root()
    if not root:
        print("\n" + T["no_root"])
        return
    print("\n" + T["project"].format(root=root))
    if tem_60fps_oficial(root) and not is_patched((root / MAIN_REL).read_text(encoding="utf-8", errors="ignore")):
        print(T["oficial"])
        return
    todos, algum = estado(root)
    try:
        if todos or algum:
            print(T["is_applied"])
            if sim(input(T["ask_revert"])):
                reverter(root)
            else:
                print(T["cancelled"])
        else:
            print(T["is_original"])
            if sim(input(T["ask_apply"])):
                aplicar(root)
            else:
                print(T["cancelled"])
    except Falha as e:
        print(e)


def main():
    if len(sys.argv) > 1:   # modo linha de comando (compativel com a versao anterior)
        ap = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
        ap.add_argument("action", choices=["status", "apply", "revert"])
        a = ap.parse_args()
        root = find_root()
        if not root:
            sys.exit(T["no_root"])
        try:
            if a.action == "status":
                estado(root)
            elif a.action == "apply":
                if tem_60fps_oficial(root):
                    sys.exit(T["oficial"])
                aplicar(root)
            else:
                reverter(root)
        except Falha as e:
            sys.exit(str(e))
        return
    try:
        assistente()
    finally:
        input(T["enter"])


if __name__ == "__main__":
    main()
