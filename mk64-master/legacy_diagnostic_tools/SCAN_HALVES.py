#!/usr/bin/env python3
"""
Descobre ONDE ficam as metades de baixo dos sprites de kart.

As metades de cima casam ([0:2048] de cada quadro), mas as de baixo nunca
aparecem em runtime -- logo nao estao em [2048:4096]. Este script pega os
hashes que o console registrou e NAO casaram (found=0) e varre o bloco
descomprimido inteiro procurando qual janela de 2048 bytes produz cada um.

O resultado mostra o deslocamento real de cada metade e, com isso, da para
corrigir o extrator para calcular os hashes certos.

Uso (dentro de mk64-master):
    py .\\SCAN_HALVES.py --rom .\\baserom.us.z64 --log .\\hdtex-trace.log --kart bowser
"""
from pathlib import Path
import argparse, json, re, sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from EXTRACT_MK64_TEXTURES import source_bytes, fnv1a32, int_value

LINE_RE = re.compile(r"hash=([0-9a-f]{8})\s+found=([01])")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default="baserom.us.z64")
    ap.add_argument("--log", default="hdtex-trace.log")
    ap.add_argument("--kart", default="bowser")
    ap.add_argument("--step", type=int, default=64,
                    help="passo da varredura em bytes (64 = uma linha de 64 texels)")
    ap.add_argument("--window", type=int, default=2048)
    a = ap.parse_args()

    root = Path(__file__).resolve().parent
    rom = Path(a.rom).read_bytes()

    txt = Path(a.log).read_text(encoding="utf-8", errors="ignore")
    want = {h for h, f in LINE_RE.findall(txt) if f == "0"}
    have = {h for h, f in LINE_RE.findall(txt) if f == "1"}
    print(f"log: {len(want)} hashes NAO casados, {len(have)} casados")

    jpath = root / "assets" / "karts" / f"{a.kart}_kart.json"
    entries = json.loads(jpath.read_text(encoding="utf-8"))

    # blocos distintos (rom_offset) usados pelos quadros
    offsets = []
    frame_off = {}
    for symbol, info in entries.items():
        if "_frame" not in symbol:
            continue
        off = int_value(info["rom_offset"])
        frame_off[symbol] = off
        if off not in offsets:
            offsets.append(off)
    print(f"{len(frame_off)} quadros em {len(offsets)} bloco(s) distinto(s)")

    cache = {}
    found_map = {}
    MAX_SCAN = 1 << 20  # 1 MB: blocos MIO0 de kart sao bem menores que isso
    for off in offsets:
        buf = source_bytes(rom, off, cache, None)
        if len(buf) > MAX_SCAN:
            buf = buf[:MAX_SCAN]  # dados crus devolvem o resto da ROM inteira
        n = len(buf)
        print(f"  bloco 0x{off:X}: {n} bytes descomprimidos -- varrendo...")
        pos = 0
        while pos + a.window <= n:
            h = f"{fnv1a32(buf[pos:pos + a.window]):08x}"
            if h in want and h not in found_map:
                found_map[h] = (off, pos)
            pos += a.step

    print(f"\nlocalizados {len(found_map)} de {len(want)} hashes nao casados\n")
    for h, (off, pos) in sorted(found_map.items(), key=lambda kv: kv[1][1])[:40]:
        print(f"  hash={h}  bloco=0x{off:X}  offset={pos} (0x{pos:X})"
              f"  = quadro {pos // 4096}, byte {pos % 4096} dentro dele")

    if not found_map:
        print("  Nenhum encontrado: os bytes nao vem cru deste bloco.")
    else:
        deltas = sorted({pos % 4096 for _, pos in found_map.values()})
        print(f"\ndeslocamentos dentro de cada quadro de 4096 bytes: {deltas[:10]}")


if __name__ == "__main__":
    main()
