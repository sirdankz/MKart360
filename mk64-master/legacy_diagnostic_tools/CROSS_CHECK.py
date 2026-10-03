#!/usr/bin/env python3
"""
Cruza kart_sprite_manifest.json com hdtex-trace.log.

Responde objetivamente: as metades de CIMA e de BAIXO que o extrator calculou
aparecem no runtime? Quais casaram (found=1) e quais nem aparecem no log?

Uso (dentro de mk64-master, com o log copiado do console para ca):
    py .\\CROSS_CHECK.py --log .\\hdtex-trace.log --kart bowser
"""
from pathlib import Path
import argparse, json, re

LINE_RE = re.compile(r"hash=([0-9a-f]{8})\s+found=([01])")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default="hdtex-trace.log")
    ap.add_argument("--manifest", default="extracted_textures/kart_sprite_manifest.json")
    ap.add_argument("--kart", default="bowser")
    a = ap.parse_args()

    log_txt = Path(a.log).read_text(encoding="utf-8", errors="ignore")
    runtime = {}
    for h, f in LINE_RE.findall(log_txt):
        runtime[h] = runtime.get(h, "0") if f == "0" else "1"
    print(f"log: {len(runtime)} hashes distintos em runtime "
          f"({sum(1 for v in runtime.values() if v == '1')} com found=1)")

    entries = json.loads(Path(a.manifest).read_text(encoding="utf-8"))
    entries = [e for e in entries if e["symbol"].startswith(a.kart)]
    print(f"manifest: {len(entries)} quadros de '{a.kart}'\n")

    stats = {"cima_no_log": 0, "cima_found1": 0, "baixo_no_log": 0, "baixo_found1": 0,
             "cima_ausente": 0, "baixo_ausente": 0,
             "alt_cima_no_log": 0, "alt_baixo_no_log": 0}
    exemplos = []

    for e in entries:
        halves = e.get("tmem_halves") or []
        if len(halves) != 2:
            continue
        top, bot = halves[0], halves[1]
        t_in, b_in = top["hash"] in runtime, bot["hash"] in runtime
        stats["cima_no_log"] += t_in
        stats["baixo_no_log"] += b_in
        stats["cima_ausente"] += (not t_in)
        stats["baixo_ausente"] += (not b_in)
        if t_in and runtime[top["hash"]] == "1":
            stats["cima_found1"] += 1
        if b_in and runtime[bot["hash"]] == "1":
            stats["baixo_found1"] += 1
        for alt in top.get("alt_hashes", []):
            if alt in runtime:
                stats["alt_cima_no_log"] += 1
        for alt in bot.get("alt_hashes", []):
            if alt in runtime:
                stats["alt_baixo_no_log"] += 1
        if len(exemplos) < 5:
            exemplos.append((e["symbol"], top["hash"], t_in, bot["hash"], b_in))

    for k, v in stats.items():
        print(f"  {k:<18} {v}")

    print("\nexemplos:")
    for sym, th, ti, bh, bi in exemplos:
        print(f"  {sym:<26} cima={th} {'OK' if ti else 'AUSENTE'}   "
              f"baixo={bh} {'OK' if bi else 'AUSENTE'}")

    print("\nComo ler:")
    print("  - baixo_ausente alto  -> o jogo NAO carrega [2048:4096]; e outra regiao.")
    print("  - baixo_no_log alto mas baixo_found1 zero -> hash certo, .tex faltando.")


if __name__ == "__main__":
    main()
