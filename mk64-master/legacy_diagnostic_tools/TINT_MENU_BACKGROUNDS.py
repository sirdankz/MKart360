#!/usr/bin/env python3
"""
Fundos coloridos dos menus (selecao de modo, personagens e pistas) em HD.

O jogo NAO guarda esses fundos na ROM: ao abrir cada menu, ele carrega a mesma
imagem da tela inicial (background_blue_sky, ou background_sunset com o modo
extra liberado), converte para tons de cinza e tinge com uma cor por menu
(menu_items.c: convert_img_to_greyscale + adjust_img_colour + gBackgroundColor):

    selecao de modo       -> rosa  (0xFF, 0xAF, 0xAF)
    selecao de personagem -> verde (0xAF, 0xFF, 0xAF)
    selecao de pista      -> azul  (0xAF, 0xAF, 0xFF)

Como os pixels mudam, os hashes mudam, e o tex.pak nao tinha nada para eles --
por isso esses fundos apareciam na resolucao original. Este script:

  1. reproduz a conversao exatamente como o jogo (mesmas contas, inclusive a
     curva de potencia implementada pelo proprio jogo), a partir do PNG
     original da ROM;
  2. calcula o hash de cada faixa, usando a disposicao de blocos do
     background_blue_sky (do manifest do fork; nao precisa de trace) e confere
     a formula contra os hashes conhecidos;
  3. aplica o mesmo tingimento a sua arte HD do fundo e salva os PNGs em
     extracted_textures\\generated\\texture_tkmk00\\tinted\\ (pode retoca-los);
  4. grava manifest_runtime\\texture_tkmk00_tinted_manifest.json, que o
     PACK_TEXTURES.py le automaticamente.

Uso (dentro de mk64-master):
    py .\\TINT_MENU_BACKGROUNDS.py
"""
from pathlib import Path
import argparse, json, shutil, struct, sys

try:
    from PIL import Image
    import numpy as np
except ImportError:
    sys.exit("Precisa de: pip install pillow numpy")

FUNDOS = ("background_blue_sky", "background_sunset")
CORES = {   # gBackgroundColor (menu_items.c), na ordem dos menus
    "modo":       (0xFF, 0xAF, 0xAF),
    "personagem": (0xAF, 0xFF, 0xAF),
    "pista":      (0xAF, 0xAF, 0xFF),
}
GREY_ARG = 0x19   # convert_img_to_greyscale(0, 0x19)


# ---- reproducao exata das funcoes matematicas do jogo (menu_items.c) --------
def _pow2(value, exponent):
    if exponent >= 0:
        base = 2.0
    else:
        exponent = -exponent
        base = 0.5
    while exponent != 0:
        if exponent & 1:
            value *= base
        exponent >>= 1
        base *= base
    return value


def _normalize(x):
    e = 0
    while x < 0.5 or x >= 1.0:
        if x < 0.5:
            x *= 2.0; e -= 1
        else:
            x /= 2.0; e += 1
    return x, e


def _ln(x):
    if x <= 0.0:
        return 0.0
    _, sp38 = _normalize(x / 1.414213562373095)
    x /= _pow2(1.0, sp38)
    v = 1
    x = (x - 1.0) / (x + 1.0)
    t12 = x * x
    f2 = x
    while True:
        v += 2
        x *= t12
        f0 = f2
        f2 += x / float(v)
        if f0 == f2:
            break
    return float(sp38) * 0.6931471805599453 + 2 * f2


def _exp(x):
    t10 = int((0.5 if x >= 0.0 else -0.5) + (x / 0.6931471805599453))
    x -= t10 * 0.6931471805599453
    f2 = x * x
    f0 = f2 / 22
    for i in range(4):
        f0 = f2 / ((18 - 4 * i) + f0)
    f2 = 2 + f0
    return _pow2((f2 + x) / (f2 - x), t10)


def menu_pow(a, b):
    if -2147483647.0 <= b <= 2147483647.0 and b == int(b):
        r = 1.0
        for _ in range(int(b)):
            r *= a
        return r
    if a > 0.0:
        return _exp(_ln(a) * b)
    return 0.0


def tabela_cinza():
    """sp48[i] (f32) e o resultado inteiro que o jogo usa para cada nivel."""
    exp_ = (GREY_ARG * 1.5 / 256.0) + 0.25
    tab = np.array([menu_pow(i / 32.0, exp_) for i in range(32)], dtype=np.float32)
    nivel = (tab * np.float32(32.0)).astype(np.uint32)
    return np.minimum(nivel, 31), exp_


def tingir_rgba16(raw, cor):
    """raw: bytes RGBA16 big-endian da imagem original -> bytes tingidos."""
    nivel, _ = tabela_cinza()
    v = np.frombuffer(raw, dtype=">u2").astype(np.uint32)
    r = ((v & 0xF800) >> 11) * 0x55
    g = ((v & 0x07C0) >> 6) * 0x4B
    b = ((v & 0x003E) >> 1) * 0x5F
    a = v & 1
    t = nivel[(r + g + b) // 256]
    # adjust_img_colour: o cinza (r=g=b=t) volta a dar t, pois 0x4D+0x96+0x1D = 256
    lum = (t * 0x4D + t * 0x96 + t * 0x1D) // 256
    out = (((lum * cor[0]) // 256) << 11) + (((lum * cor[1]) // 256) << 6) + \
          (((lum * cor[2]) // 256) << 1) + a
    return out.astype(">u2").tobytes()


def tingir_hd(img, cor):
    """Mesmo tingimento, em ponto flutuante, para a arte HD (8 bits)."""
    _, exp_ = tabela_cinza()
    a = np.asarray(img.convert("RGBA")).astype(np.float64)
    r5, g5, b5 = a[..., 0] * 31 / 255, a[..., 1] * 31 / 255, a[..., 2] * 31 / 255
    cinza = (r5 * 0x55 + g5 * 0x4B + b5 * 0x5F) / 256.0
    t = np.minimum(np.power(np.clip(cinza / 32.0, 0, 1), exp_) * 32.0, 31.0)
    out = np.empty_like(a)
    for k in range(3):
        out[..., k] = (t * cor[k] / 256.0) * 255 / 31
    out[..., 3] = a[..., 3]
    return Image.fromarray(np.clip(np.rint(out), 0, 255).astype(np.uint8), "RGBA")


# ---- utilidades --------------------------------------------------------------
def fnv1a32(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def png_para_rgba16(path, W, H):
    im = Image.open(path).convert("RGBA")
    if im.size != (W, H):
        return None
    a = np.asarray(im).astype(np.uint32)
    v = (np.rint(a[..., 0] * 31 / 255).astype(np.uint32) << 11) | \
        (np.rint(a[..., 1] * 31 / 255).astype(np.uint32) << 6) | \
        (np.rint(a[..., 2] * 31 / 255).astype(np.uint32) << 1) | (a[..., 3] >= 128)
    return v.astype(">u2").tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--textures", default="extracted_textures")
    ap.add_argument("--originais", default=None,
                    help="pasta de uma extracao SEPARADA, com os PNGs originais da ROM "
                         "(use quando os de extracted_textures ja foram trocados por HD)")
    a = ap.parse_args()
    root = Path(__file__).resolve().parent
    tex = Path(a.textures)

    imagens = {e.get("symbol"): e for e in
               json.loads((tex / "texture_tkmk00_manifest.json").read_text(encoding="utf-8"))}
    # Disposicao dos blocos do fundo (so coordenadas; igual para qualquer ROM).
    # Formato usado aqui: y1 = y0 + linhas carregadas (o G_LOADTILE le 1 a mais).
    layout, fonte = None, ""
    anim_p = root / "manifest_runtime" / "texture_tkmk00_anim_manifest.json"
    if anim_p.is_file():
        anim = {e["symbol"]: e for e in json.loads(anim_p.read_text(encoding="utf-8"))}
        if len(anim.get("background_blue_sky", {}).get("tiles", [])) > 10:
            layout, fonte = anim["background_blue_sky"]["tiles"], "medida"
    if layout is None:
        rt_p = root / "manifest_runtime" / "texture_tkmk00_runtime_manifest.json"
        if rt_p.is_file():
            for e in json.loads(rt_p.read_text(encoding="utf-8")):
                if e.get("symbol") == "background_blue_sky" and e.get("tmem_tiles_runtime"):
                    # manifest do Eduardo: y1 e o fim LOGICO; carregadas = y1 - y0 + 1
                    layout = [dict(t, y1=t["y1"] + 1) for t in e["tmem_tiles_runtime"]]
                    fonte = "eduardo"
                    break
    if layout is None:
        # padrao: faixas de largura total, 2 linhas logicas (3 carregadas)
        layout = [{"x0": 0, "y0": y, "x1": 320, "y1": y + 3} for y in range(0, 240, 2)]
        fonte = "padrao"
    print(f"disposicao dos blocos do fundo: {len(layout)} blocos "
          f"({ {'medida': 'medida com a sua ROM', 'eduardo': 'manifest do fork', 'padrao': 'padrao embutido'}[fonte] })")

    saida, resumo = [], []
    for fundo in FUNDOS:
        e = imagens.get(fundo)
        if not e:
            continue
        W, H, rel = int(e["width"]), int(e["height"]), e["png"]
        orig = None
        candidatos = [tex / "_originais" / rel, tex / rel]
        if a.originais:
            candidatos.insert(0, Path(a.originais) / rel)
        for cand in candidatos:
            if cand.is_file():
                orig = png_para_rgba16(cand, W, H)
                if orig:
                    break
        if not orig:
            print(f"  {fundo}: PNG original ({W}x{H}) nao encontrado -- pulado")
            print("    (extraia os originais numa pasta separada e use --originais PASTA)")
            continue

        # confere a formula do hash contra os blocos medidos do fundo original
        if fundo == "background_blue_sky":
            ok = tot = 0
            for t in layout:
                off = (t["y0"] * W + t["x0"]) * 2
                L = (t["x1"] - t["x0"]) * (t["y1"] - t["y0"]) * 2
                if "hash" in t and off + L <= len(orig):
                    tot += 1
                    ok += f"{fnv1a32(orig[off:off + L]):08x}" == t["hash"]
            if tot:
                print(f"conferencia da formula: {ok}/{tot} blocos do fundo original batem")
            if fonte == "medida" and (tot == 0 or ok < tot):
                sys.exit("A formula nao bateu com os hashes medidos com a sua ROM -- nada "
                         "foi gravado. Mande esta saida para analise.")
            if fonte == "eduardo" and tot and ok < tot:
                print("  (aviso: os hashes do manifest do fork foram medidos com outra ROM;\n"
                      "   se a sua for de outra regiao, isso e esperado e a disposicao\n"
                      "   continua valida. Se os fundos nao aparecerem em HD no jogo,\n"
                      "   mande esta saida para analise.)")

        hd_path = tex / rel
        hd = Image.open(hd_path).convert("RGBA") if hd_path.is_file() else None
        if hd is None or hd.size == (W, H):
            print(f"  {fundo}: sem arte HD (o PNG tem o tamanho original) -- pulado")
            continue

        for menu, cor in CORES.items():
            tingido = tingir_rgba16(orig, cor)
            tiles = []
            for t in layout:
                off = (t["y0"] * W + t["x0"]) * 2
                w = t["x1"] - t["x0"]
                L = w * (t["y1"] - t["y0"]) * 2
                L_log = L - w * 2
                nt = {k: t[k] for k in ("x0", "y0", "x1", "y1")}
                if off + L <= len(tingido):
                    nt["hash"] = f"{fnv1a32(tingido[off:off + L]):08x}"
                if L_log > 0 and off + L_log <= len(tingido):
                    nt["logical_hash"] = f"{fnv1a32(tingido[off:off + L_log]):08x}"
                if "hash" in nt or "logical_hash" in nt:
                    tiles.append(nt)
            nome_png = f"generated/texture_tkmk00/tinted/{fundo}__{menu}.png"
            destino = tex / nome_png
            destino.parent.mkdir(parents=True, exist_ok=True)
            if destino.exists():
                print(f"  {nome_png}: ja existe, mantido (apague para regerar)")
            else:
                tingir_hd(hd, cor).save(destino)
            saida.append({"symbol": f"{fundo}__{menu}", "png": nome_png,
                          "width": W, "height": H, "tiles": tiles})
            resumo.append(f"  {fundo:<22} {menu:<10} {len(tiles)} bloco(s)")

    if not saida:
        sys.exit("nada gerado")
    print("\n".join(resumo))
    dest = root / "manifest_runtime" / "texture_tkmk00_tinted_manifest.json"
    dest.write_text(json.dumps(saida, indent=1), encoding="utf-8")
    shutil.copy2(dest, tex / dest.name)
    print(f"\ngravado: {dest}\nPNGs HD tingidos em {tex / 'generated/texture_tkmk00/tinted'}")
    print("Agora empacote:  py .\\PACK_TEXTURES.py --pak")


if __name__ == "__main__":
    main()
