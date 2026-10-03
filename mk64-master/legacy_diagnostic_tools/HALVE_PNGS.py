#!/usr/bin/env python3
"""
Reduz PNGs a METADE da resolucao, in-place, na pasta onde este script esta.

Motivo: sprites de kart em 256x256 somam ~660 MB em .tex (RGBA32 sem
compressao), acima do que cabe na memoria de 512 MB do Xbox 360 -- o que
causa releitura constante do disco durante o jogo. Em 128x128 o total cai
para ~165 MB, ainda 4x a resolucao original do N64 (64x64).

Por padrao sobrescreve os PNGs no lugar. Use --backup para guardar os
originais antes, ou --out PASTA para escrever noutra pasta sem tocar nos
arquivos atuais.

Uso (coloque o script na pasta com os PNGs e rode de la):
    py .\\HALVE_PNGS.py                 # previa: mostra o que faria
    py .\\HALVE_PNGS.py --apply         # aplica de fato
    py .\\HALVE_PNGS.py --apply --recursive --backup
"""
from pathlib import Path
import argparse, sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Precisa do Pillow: pip install pillow")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true",
                    help="aplica de fato (sem isto, so mostra o que seria feito)")
    ap.add_argument("--recursive", action="store_true",
                    help="processa tambem as subpastas")
    ap.add_argument("--backup", action="store_true",
                    help="salva o original como <nome>.orig.png antes de sobrescrever")
    ap.add_argument("--out", help="escreve nesta pasta em vez de sobrescrever")
    ap.add_argument("--min", type=int, default=8,
                    help="nao reduz abaixo desta dimensao (padrao 8)")
    a = ap.parse_args()

    here = Path(__file__).resolve().parent
    files = sorted(here.rglob("*.png") if a.recursive else here.glob("*.png"))
    files = [f for f in files if not f.name.endswith(".orig.png")]
    if not files:
        print(f"nenhum PNG encontrado em {here}"
              + ("" if a.recursive else " (use --recursive para subpastas)"))
        return

    outdir = Path(a.out).resolve() if a.out else None
    if outdir:
        outdir.mkdir(parents=True, exist_ok=True)

    print(f"{len(files)} PNG(s) em {here}")
    if not a.apply:
        print("PREVIA -- nada sera gravado. Use --apply para valer.\n")

    feitos = pulados = 0
    bytes_antes = bytes_depois = 0

    for f in files:
        try:
            img = Image.open(f)
            w, h = img.size
        except Exception as exc:
            print(f"  ! {f.name}: nao consegui abrir ({exc})")
            pulados += 1
            continue

        nw, nh = max(w // 2, 1), max(h // 2, 1)
        if nw < a.min or nh < a.min:
            print(f"  - {f.relative_to(here)}: {w}x{h} -> ficaria {nw}x{nh}, abaixo de --min, pulando")
            pulados += 1
            continue

        bytes_antes += w * h * 4
        bytes_depois += nw * nh * 4

        if not a.apply:
            print(f"  {f.relative_to(here)}: {w}x{h} -> {nw}x{nh}")
            feitos += 1
            continue

        try:
            img = img.convert("RGBA")
            # LANCZOS preserva melhor o detalhe ao reduzir
            menor = img.resize((nw, nh), Image.LANCZOS)
            destino = (outdir / f.relative_to(here)) if outdir else f
            if outdir:
                destino.parent.mkdir(parents=True, exist_ok=True)
            elif a.backup:
                bkp = f.with_suffix(".orig.png")
                if not bkp.exists():
                    f.replace(bkp)
                    img = Image.open(bkp).convert("RGBA")
                    menor = img.resize((nw, nh), Image.LANCZOS)
            menor.save(destino, "PNG", optimize=True)
            feitos += 1
        except Exception as exc:
            print(f"  ! {f.name}: falhou ({exc})")
            pulados += 1

    print(f"\n{feitos} processado(s), {pulados} pulado(s)")
    if bytes_antes:
        print(f"em .tex (RGBA32): {bytes_antes/1024/1024:.0f} MB -> {bytes_depois/1024/1024:.0f} MB")
    if not a.apply:
        print("\nNada foi alterado. Rode de novo com --apply.")
    elif outdir:
        print(f"gravado em {outdir}")
    elif a.backup:
        print("originais guardados como *.orig.png")


if __name__ == "__main__":
    main()
