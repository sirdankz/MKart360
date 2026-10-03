#!/usr/bin/env python3
"""
Mede como o jogo divide as imagens grandes de menu (RGBA16).

Imagens que passam do limite de 4 KB da TMEM sao carregadas em PEDACOS, cada
um com hash proprio. O trace mostrou que nao sao so faixas horizontais: ha
BLOCOS 2D (ex: 33x33 e 32x33 numa imagem de 64 de largura -- blocos de 32
com 1 texel de sobreposicao, para a filtragem bilinear nao mostrar emenda).

Como o port calcula o hash: o pedaco comeca no canto (x0,y0) do bloco dentro
da imagem, e o hash cobre um trecho CONTINUO de memoria de tamanho
largura_bloco*altura_bloco*2 a partir dali. Entao testamos todas as posicoes
iniciais possiveis. FNV-1a e calculavel por prefixo: avancamos todas as
posicoes em paralelo (numpy) e comparamos com o log a cada tamanho de pedaco.

  1. o trace do jogo registra hash e tamanho de cada pedaco carregado;
  2. este script acha de que posicao/tamanho da imagem veio cada pedaco;
  3. grava menu_tiles_measured.json; o PACK_TEXTURES.py recorta a arte HD
     nos mesmos retangulos.

Uso (dentro de mk64-master, com o log do console copiado para ca):
    py .\\SCAN_MENU.py --log .\\hdtex-trace.log
    py .\\SCAN_MENU.py --log .\\hdtex-trace.log --only generated/course_player_selection

Requer numpy:  pip install numpy
"""
from pathlib import Path
import argparse, json, re, sys

try:
    import numpy as np
except ImportError:
    sys.exit("Precisa do numpy: pip install numpy")

sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    from EXTRACT_MK64_TEXTURES import mio0_decode
except ImportError:
    mio0_decode = None

LOG_RE = re.compile(
    r"w=(\d+)\s+h=(\d+)\s+srcsz=(\d+)\s+hash=([0-9a-f]{8})\s+found=([01])")
ARRAY_RE = re.compile(
    r"unsigned\s+char\s+([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\]\s*=\s*\{(.*?)\};", re.S)
HEX_RE = re.compile(r"0x([0-9A-Fa-f]{1,2})")

FNV_OFFSET = np.uint32(2166136261)
FNV_PRIME = np.uint32(16777619)


def carregar_banco(caminho, cache):
    if caminho in cache:
        return cache[caminho]
    arrays = {}
    if caminho.is_file():
        txt = caminho.read_text(encoding="utf-8", errors="ignore")
        for nome, corpo in ARRAY_RE.findall(txt):
            arrays[nome] = bytes(int(h, 16) for h in HEX_RE.findall(corpo))
    cache[caminho] = arrays
    return arrays


def achar_pedacos(img, alvos):
    """alvos: {tamanho_em_bytes: {hash_int: largura_do_pedaco}}.
    Devolve [(offset, tamanho, hash_int)] dos pedacos encontrados."""
    arr = np.frombuffer(img, dtype=np.uint8)
    n = arr.size
    tamanhos = sorted(t for t in alvos if 0 < t <= n)
    if not tamanhos:
        return []
    offs = np.arange(0, n, 2, dtype=np.int64)   # cada texel RGBA16
    h = np.full(offs.size, FNV_OFFSET, dtype=np.uint32)
    achados, t_idx = [], 0
    with np.errstate(over="ignore"):
        for i in range(tamanhos[-1]):
            pos = offs + i
            valido = pos < n
            b = np.zeros(offs.size, dtype=np.uint32)
            b[valido] = arr[pos[valido]]
            h = (h ^ b) * FNV_PRIME
            while t_idx < len(tamanhos) and tamanhos[t_idx] == i + 1:
                L = tamanhos[t_idx]
                alvo = np.fromiter(alvos[L].keys(), dtype=np.uint32)
                hits = np.nonzero(((offs + L) <= n) & np.isin(h, alvo))[0]
                achados.extend((int(offs[k]), L, int(h[k])) for k in hits)
                t_idx += 1
    return achados


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default="hdtex-trace.log")
    ap.add_argument("--textures", default="extracted_textures")
    ap.add_argument("--only", help="so imagens cujo PNG comeca com este prefixo, "
                                   "ex: generated/course_player_selection")
    a = ap.parse_args()

    root = Path(__file__).resolve().parent
    texdir = Path(a.textures)

    # hash_int -> (largura, srcsz, found)
    pedacos = {}
    # hash_int -> (lx, ly, pitch) quando o log traz a geometria real do
    # carregamento (trace mais novo). Usado para descartar casamentos por
    # coincidencia: um pedaco verdadeiro tem que estar na posicao registrada.
    geometria = {}
    for linha in Path(a.log).read_text(encoding="utf-8", errors="ignore").splitlines():
        m = LOG_RE.search(linha)
        if not m:
            continue
        w, h, srcsz, hh, found = m.groups()
        hv = int(hh, 16)
        pedacos[hv] = (int(w), int(srcsz), found == "1")
        g = re.search(r"pitch=(\d+)\s+lx=(\d+)\s+ly=(\d+)", linha)
        if g:
            geometria[hv] = (int(g.group(2)), int(g.group(3)), int(g.group(1)))
    print(f"log: {len(pedacos)} pedacos RGBA16 distintos "
          f"({sum(1 for v in pedacos.values() if v[2])} ja casando)")

    man_path = texdir / "generated_texture_manifest.json"
    if not man_path.is_file():
        sys.exit(f"nao encontrei {man_path} -- rode o extrator antes")
    entradas = [e for e in json.loads(man_path.read_text(encoding="utf-8"))
                if e.get("format") == "rgba16"]
    if a.only:
        pref = a.only.replace("\\", "/").lower()
        entradas = [e for e in entradas if e["png"].lower().startswith(pref)]
    print(f"{len(entradas)} imagens RGBA16 candidatas -- varrendo...\n")

    banks_dir = root / "src" / "xbox360" / "generated_banks"
    cache_bancos, medido, usados = {}, {}, set()

    for e in entradas:
        W, H = int(e["width"]), int(e["height"])
        dados = carregar_banco(banks_dir / e["bank"], cache_bancos).get(e["symbol"])
        if not dados:
            continue
        if dados[:4] == b"MIO0" and mio0_decode:
            dados = mio0_decode(dados)
        if dados[:4] == b"TKMK":
            continue  # TKMK00: exige decodificador proprio
        img = dados[:W * H * 2]
        if len(img) < W * H * 2:
            continue

        alvos = {}
        for hv, (pw, srcsz, _) in pedacos.items():
            if hv in usados or pw <= 0 or pw > W or srcsz % (pw * 2):
                continue
            if srcsz // (pw * 2) > H:
                continue
            alvos.setdefault(srcsz, {})[hv] = pw
        if not alvos:
            continue

        tiles = []
        for off, L, hv in achar_pedacos(img, alvos):
            if hv in usados:
                continue
            pw = alvos[L][hv]
            ph = L // (pw * 2)
            # Trechos de cor unica (ex: area transparente) tem o MESMO hash que
            # qualquer outro trecho uniforme de igual tamanho em qualquer
            # textura do jogo -- casamento por coincidencia. Atribui-los aqui
            # faria o empacotador trocar texturas nao relacionadas.
            trecho = img[off:off + L]
            if trecho == trecho[:2] * (L // 2):
                continue
            y0, x0 = divmod(off // 2, W)
            geo = geometria.get(hv)
            if geo:
                lx, ly, pitch = geo
                if (x0, y0) != (lx, ly) or (pitch and pitch != W * 2):
                    continue  # posicao ou largura de origem nao batem: coincidencia
            # A largura e cortada pelo port na borda direita, mas a ALTURA nao:
            # blocos de 33 linhas comecando em y=32 numa imagem de 64 vao ate a
            # linha 64, uma alem do fim. O hash cobre esse trecho continuo
            # normalmente, entao o bloco e valido (o empacotador estende a
            # ultima linha da arte HD para cobrir essa linha extra).
            if x0 + pw > W or y0 >= H:
                continue
            tiles.append({"x0": x0, "y0": y0, "x1": x0 + pw, "y1": y0 + ph,
                          "hash": f"{hv:08x}"})
            usados.add(hv)
        if tiles:
            tiles.sort(key=lambda t: (t["y0"], t["x0"]))
            medido[e["png"]] = {"symbol": e["symbol"], "width": W,
                                "height": H, "tiles": tiles}

    for png, info in sorted(medido.items()):
        cob = np.zeros((info["height"], info["width"]), dtype=bool)
        for t in info["tiles"]:
            cob[t["y0"]:min(t["y1"], info["height"]),
                t["x0"]:min(t["x1"], info["width"])] = True
        falta = int((~cob).sum())
        estado = "completa" if falta == 0 else f"faltam {falta} texel(s)"
        print(f"  {info['symbol']:<34} {info['width']}x{info['height']}  "
              f"{len(info['tiles'])} pedaco(s), {estado}")
        for t in info["tiles"][:6]:
            print(f"      x {t['x0']:>3}-{t['x1']:<3} y {t['y0']:>3}-{t['y1']:<3} hash={t['hash']}")
        if len(info["tiles"]) > 6:
            print(f"      ... e mais {len(info['tiles']) - 6}")

    sem_dono = sum(1 for hv, v in pedacos.items() if hv not in usados and not v[2])
    print(f"\n{len(medido)} imagem(ns) com pedacos identificados")
    print(f"{sem_dono} pedaco(s) do log sem imagem correspondente "
          f"(outras texturas do jogo ou imagens TKMK00 -- normal)")

    if medido:
        out = texdir / "menu_tiles_measured.json"
        out.write_text(json.dumps(medido, indent=1), encoding="utf-8")
        print(f"\ngravado: {out}")

        # Mescla na geometria portatil versionada (so coordenadas, sem hashes).
        geo_path = root / "menu_tiles_geometry.json"
        try:
            geo = json.loads(geo_path.read_text(encoding="utf-8")) if geo_path.is_file() else {}
        except (OSError, json.JSONDecodeError):
            geo = {}
        geo.setdefault("bank", "course_player_selection.c")
        imgs = geo.setdefault("images", {})
        novas = 0
        for info in medido.values():
            if info["symbol"] not in imgs:
                novas += 1
            imgs[info["symbol"]] = {
                "width": info["width"], "height": info["height"],
                "tiles": [{k: t[k] for k in ("x0", "y0", "x1", "y1")} for t in info["tiles"]]}
        geo_path.write_text(json.dumps(geo, indent=1), encoding="utf-8")
        print(f"geometria atualizada: {geo_path.name} ({novas} imagem(ns) nova(s))")
        print("Empacote com:  py .\\PACK_TEXTURES.py --only generated --pak")


if __name__ == "__main__":
    main()
