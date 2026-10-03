#!/usr/bin/env python3
r"""
Empacota os PNGs HD editados em extracted_textures\ para o formato .tex
que o patch de gfx_pc.c le em runtime (game:\tex\<hash>.tex).

Reconhece 3 origens de PNG:
  1. extracted_textures\<hash>__nome.png              (texturas common_data)
  2. extracted_textures\generated\<banco>\<symbol>.png (via generated_texture_manifest.json)
  3. extracted_textures\karts\...\<symbol>.png         (via kart_sprite_manifest.json)

Para entradas com "tmem_tiles" no manifest (imagens grandes de menu, ex:
retratos de selecao de personagem, "press start"): o jogo ORIGINAL as
particiona em varias faixas de TMEM, cada faixa com hash proprio. Com o
patch x360_try_draw_hd_menu_quad aplicado (gfx_pc.c + menu_items.c), essas
imagens passam a ser desenhadas INTEIRAS de uma vez, usando o hash da
imagem completa -- que e o comportamento PADRAO deste script.
Use --tiles apenas se aquele patch NAO estiver aplicado (modo antigo, que
gera um .tex por faixa; nunca conseguimos casar o particionamento real,
por isso o patch existe).

Requer Pillow:  pip install pillow

Uso (dentro de mk64-master, apos rodar o extrator):
    py .\PACK_TEXTURES.py --in extracted_textures --out tex

Para empacotar so um subconjunto, use --only com um prefixo de caminho:
    py .\PACK_TEXTURES.py --only generated\course_player_selection
"""
from pathlib import Path
import argparse, hashlib, json, re, struct, sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Precisa do Pillow: pip install pillow")

PAK_MODE = False
PAK_MAGIC = 0x4844504B  # "HDPK"
MAGIC = 0x54584448  # 'HDXT', mesmo valor de X360_HDTEX_MAGIC no patch C
NAME_RE = re.compile(r"^([0-9a-fA-F]{8})__")


def load_manifest(path):
    if not path.is_file():
        return []
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []


PAK_ENTRIES = []   # usado no modo --pak: (hash_hex, w, h, bytes)
_PAK_SOURCE = {}   # hash -> de onde a textura veio (para o grupo de pre-carregamento)

# ---- Pre-carregamento dos menus --------------------------------------------
# A primeira entrada na selecao de modo travava por ler do disco, na hora, os
# blocos HD do menu. Agora o empacotador separa um GRUPO de texturas de menu
# (por prioridade, ate PRELOAD_MB), grava-o contiguo no inicio do tex.pak e o
# marca no indice; o jogo o le aos poucos enquanto a tela inicial esta parada.
# Fora deste grupo nada muda: as corridas carregam como antes.
PRELOAD_MB = 20
_PRELOAD_P0 = ("background_blue_sky.png", "background_blue_sky__modo")
_PRELOAD_P1 = ("texture_game_select", "texture_menu_1p", "texture_menu_2p",
               "texture_menu_3p", "texture_menu_4p", "texture_mode_",
               "texture_50cc", "texture_100cc", "texture_150cc", "texture_extra",
               "gtexturemenuwith", "texture_begin", "texture_menu_ghost",
               "texture_data", "texture_ok", "texture_l_option", "texture_r_data",
               "copyright", "push_start")


def _prioridade_preload(source):
    s = (source or "").lower().replace("\\", "/")
    if "texture_tkmk00" not in s and "push_start" not in s and "copyright" not in s:
        return None
    if any(k in s for k in _PRELOAD_P0):
        return 0
    if any(k in s for k in _PRELOAD_P1):
        return 1
    if "__personagem" in s or "__pista" in s:
        return 3          # fundos de telas posteriores: por ultimo
    return 2
_HASH_RECORDS = {}
_HASH_DEDUP_COUNT = 0
_HASH_COLLISIONS = []


def write_tex(outdir, hash_hex, img, source=""):
    global _HASH_DEDUP_COUNT
    w, h = img.size
    hh = str(hash_hex).lower()
    raw = img.tobytes()
    prev = _HASH_RECORDS.get(hh)
    if prev is not None:
        pw, ph, praw, psource = prev
        if pw == w and ph == h and praw == raw:
            _HASH_DEDUP_COUNT += 1
            return None
        _HASH_COLLISIONS.append({
            "hash": hh,
            "existing": {"width": pw, "height": ph, "source": psource,
                         "sha256": hashlib.sha256(praw).hexdigest()},
            "incoming": {"width": w, "height": h, "source": source,
                         "sha256": hashlib.sha256(raw).hexdigest()},
        })
        print(f"  ! COLISAO FNV32 {hh}: mantendo a primeira textura; ignorando a segunda: {source}")
        return None
    _HASH_RECORDS[hh] = (w, h, raw, source)
    if PAK_MODE:
        PAK_ENTRIES.append((hh, w, h, raw))
        _PAK_SOURCE[hh] = source
        return None
    sub = outdir / hh[:2]
    sub.mkdir(parents=True, exist_ok=True)
    out_path = sub / f"{hh}.tex"
    with open(out_path, "wb") as f:
        f.write(struct.pack(">III", MAGIC, w, h))
        f.write(raw)
    return out_path


_ARRAY_RE = re.compile(
    r"unsigned\s+char\s+([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\]\s*=\s*\{(.*?)\};", re.S)
_HEX_RE = re.compile(r"0x([0-9A-Fa-f]{1,2})")


def _fnv1a32(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def aplicar_geometria(geo_path, generated):
    """Para cada imagem da geometria, le os bytes do simbolo no banco gerado
    (descomprimindo MIO0), calcula o hash de cada pedaco do jeito que o port
    calcula (trecho continuo a partir do canto x0,y0) e injeta como
    tmem_halves na entrada do manifest."""
    try:
        doc = json.loads(geo_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return 0
    banco = Path(__file__).resolve().parent / "src" / "xbox360" / "generated_banks" / doc.get("bank", "")
    if not banco.is_file():
        print(f"  ! {banco} nao existe -- rode o prepare/build antes")
        return 0
    try:
        from EXTRACT_MK64_TEXTURES import mio0_decode
    except ImportError:
        mio0_decode = None
    arrays = {nome: corpo for nome, corpo in _ARRAY_RE.findall(
        banco.read_text(encoding="utf-8", errors="ignore"))}
    por_simbolo = {e.get("symbol"): k for k, e in generated.items()}
    feitos = 0
    for simbolo, info in doc.get("images", {}).items():
        chave = por_simbolo.get(simbolo)
        corpo = arrays.get(simbolo)
        if chave is None or corpo is None:
            continue
        dados = bytes(int(h, 16) for h in _HEX_RE.findall(corpo))
        if dados[:4] == b"MIO0" and mio0_decode:
            dados = mio0_decode(dados)
        W = int(info["width"])
        tiles = []
        for t in info["tiles"]:
            pw = t["x1"] - t["x0"]
            ph = t["y1"] - t["y0"]
            off = (t["y0"] * W + t["x0"]) * 2
            L = pw * ph * 2
            if off + L > len(dados):
                continue
            tiles.append(dict(t, hash=f"{_fnv1a32(dados[off:off + L]):08x}"))
        if tiles:
            generated[chave] = dict(generated[chave])
            generated[chave]["tmem_halves"] = tiles
            feitos += 1
    return feitos


def _diagnose_yoshi(indir, generated):
    for rel in ("generated/course_player_selection/gTextureYoshiFace08.png",
                "generated/course_player_selection/gTextureYoshiFace09.png"):
        print("\n" + rel)
        e = generated.get(rel.lower())
        if not e:
            print("  manifest: NAO ENCONTRADO")
            continue
        print("  symbol:", e.get("symbol"))
        print("  decoded_hash_fnv1a32:", e.get("decoded_hash_fnv1a32"))
        print("  tmem_halves:", len(e.get("tmem_halves") or []))
        for i, t in enumerate(e.get("tmem_halves") or []):
            print(f"    [{i}] ({t.get('x0',0)},{t.get('y0',0)})-({t.get('x1',e.get('width'))},{t.get('y1')}) hash={t.get('hash')}")



# ---- Compressao DXT (opcao --dxt) -----------------------------------------
# DXT1 (8 bytes / bloco 4x4) para texturas opacas ou com transparencia
# binaria; DXT5 (16 bytes / bloco) quando ha transparencia gradual. O jogo
# descomprime no console antes do envio a GPU; o ganho e ler 4-8x menos do
# disco e caber 4-8x mais texturas no cache em RAM.
# Texturas fora desta compressao (continuam RGBA32):
#   - largura/altura que nao sejam multiplos de 4;
#   - DFF91B13 (botao OK): o gfx_pc.c recorta seus pixels direto do cache.
DXT_NUNCA = {"dff91b13"}


def _expand565(c):
    import numpy as np
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def _pack565(rgb):
    import numpy as np
    rgb = np.clip(rgb, 0, 255)
    r = np.rint(rgb[..., 0] * 31 / 255).astype(np.uint32)
    g = np.rint(rgb[..., 1] * 63 / 255).astype(np.uint32)
    b = np.rint(rgb[..., 2] * 31 / 255).astype(np.uint32)
    return (r << 11) | (g << 5) | b


def _bloco_cor(px, opaco, modo3):
    """px: (N,16,3) int32; opaco: (N,16) bool. Devolve (c0,c1,idx) uint32
    usando exatamente a mesma paleta (inteira) do decodificador do console."""
    import numpy as np
    big = np.where(opaco[..., None], px, -1)
    sml = np.where(opaco[..., None], px, 999)
    mx = big.max(1).astype(np.float64); mn = sml.min(1).astype(np.float64)
    vazio = ~opaco.any(1)
    mx[vazio] = 0; mn[vazio] = 0
    inset = (mx - mn) / 16.0
    a = _pack565(mx - inset); b = _pack565(mn + inset)
    if modo3:   # 3 cores + transparente: exige c0 <= c1
        c0 = np.minimum(a, b); c1 = np.maximum(a, b)
    else:       # 4 cores: exige c0 > c1 (iguais -> indice 0 em tudo)
        c0 = np.maximum(a, b); c1 = np.minimum(a, b)
    e0 = _expand565(c0); e1 = _expand565(c1)
    if modo3:
        pal = np.stack([e0, e1, (e0 + e1) // 2], 1)                    # (N,3,3)
    else:
        pal = np.stack([e0, e1, (2 * e0 + e1) // 3, (e0 + 2 * e1) // 3], 1)
    d = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)       # (N,16,k)
    idx = d.argmin(-1).astype(np.uint32)
    if modo3:
        idx = np.where(opaco, idx, 3)
    else:
        idx = np.where((c0 == c1)[:, None], 0, idx)
    return c0, c1, idx


def dxt_encode(raw, w, h):
    """Devolve (fmt, bytes) com fmt 1 = DXT1, 2 = DXT5."""
    import numpy as np
    a = np.frombuffer(raw, np.uint8).reshape(h, w, 4).astype(np.int32)
    blk = a.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    alfa = blk[..., 3]
    if alfa.min() >= 250:
        fmt, modo3 = 1, False
        opaco = np.ones(alfa.shape, bool)
    elif ((alfa <= 8) | (alfa >= 247)).all():
        fmt, modo3 = 1, True
        opaco = alfa >= 128
    else:
        fmt, modo3 = 2, False
        opaco = np.ones(alfa.shape, bool)
    c0, c1, idx = _bloco_cor(blk[..., :3], opaco, modo3)
    sh2 = (np.arange(16, dtype=np.uint32) * 2)
    idx32 = (idx << sh2).sum(1).astype(np.uint32)
    cor = np.zeros((blk.shape[0], 8), np.uint8)
    cor[:, 0] = c0 & 255; cor[:, 1] = c0 >> 8
    cor[:, 2] = c1 & 255; cor[:, 3] = c1 >> 8
    for k in range(4):
        cor[:, 4 + k] = (idx32 >> (8 * k)) & 255
    if fmt == 1:
        return 1, cor.tobytes()
    a0 = alfa.max(1); a1 = alfa.min(1)
    k = np.arange(1, 7)
    palA = np.concatenate([a0[:, None], a1[:, None],
                           ((7 - k)[None, :] * a0[:, None] + k[None, :] * a1[:, None]) // 7], 1)
    dA = np.abs(alfa[:, :, None] - palA[:, None, :])
    aidx = dA.argmin(-1).astype(np.uint64)
    aidx = np.where((a0 == a1)[:, None], 0, aidx)
    bits = (aidx << (np.arange(16, dtype=np.uint64) * 3)).sum(1).astype(np.uint64)
    alfa_blk = np.zeros((blk.shape[0], 8), np.uint8)
    alfa_blk[:, 0] = a0; alfa_blk[:, 1] = a1
    for k2 in range(6):
        alfa_blk[:, 2 + k2] = ((bits >> np.uint64(8 * k2)) & np.uint64(255)).astype(np.uint8)
    return 2, np.concatenate([alfa_blk, cor], 1).tobytes()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="indir", default="extracted_textures")
    ap.add_argument("--out", dest="outdir", default="tex")
    ap.add_argument("--max", type=int, default=2048)
    ap.add_argument("--only", help="so empacota PNGs cujo caminho relativo comeca com este prefixo")
    ap.add_argument("--dxt", action="store_true",
                    help="com --pak: comprime as texturas em DXT1/DXT5 (4-8x menor). "
                         "Exige o gfx_pc.c com suporte a DXT.")
    ap.add_argument("--pak", action="store_true",
                    help="gera UM arquivo tex.pak em vez de milhares de .tex soltos. "
                         "Evita milhares de aberturas de arquivo em runtime (causa dos "
                         "engasgos de cache frio) e contorna o limite do FATX.")
    ap.add_argument("--tiles", action="store_true",
                    help="modo ANTIGO: fatia imagens grandes de menu em varios .tex (um por faixa "
                         "de TMEM). So necessario se o patch de menu_items.c (x360_try_draw_hd_menu_quad) "
                         "NAO estiver aplicado. Com o patch, o padrao (imagem inteira) e o correto.")
    ap.add_argument("--diagnose-yoshi", action="store_true",
                    help="mostra hashes/tiles de Yoshi sem gerar o PAK")
    a = ap.parse_args()

    indir = Path(a.indir)
    outdir = Path(a.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    global PAK_MODE
    PAK_MODE = a.pak
    only_prefix = a.only.replace("\\", "/").lower() if a.only else None

    # --- indices dos manifests, por caminho de PNG relativo -------------
    # IMPORTANTE: manter os manifests separados para preservar o diagnostico
    # e a contagem original. Eles continuam sendo empacotados juntos no
    # mesmo tex.pak; apenas nao sao artificialmente fundidos nos contadores.
    generated = {e["png"].replace("\\", "/").lower(): e
                 for e in load_manifest(indir / "generated_texture_manifest.json")}

    # Manifests adicionais de bancos generated: fallback, sem alterar a
    # categoria principal. Se o PNG ja existe no consolidado, ele vence.
    for name in (
        "other_textures_manifest.json",
        "texture_data_2_manifest.json",
        "course_player_selection_manifest.json",
    ):
        for e in load_manifest(indir / name):
            png_name = e.get("png")
            if png_name:
                generated.setdefault(png_name.replace("\\", "/").lower(), e)

    tkmk = {e["png"].replace("\\", "/").lower(): e
            for e in load_manifest(indir / "texture_tkmk00_manifest.json")}

    # TKMK00 runtime layout/hashes medidos do caminho real de func_80095E10().
    # O hash de runtime inclui a linha extra causada pelas coordenadas inclusivas
    # de G_LOADTILE; este banco foi gerado contra a ROM base e o trace TKMK.
    runtime_manifest = {}
    for e in load_manifest(indir / "texture_tkmk00_runtime_manifest.json"):
        if e.get("png"):
            runtime_manifest[e["png"].replace("\\", "/").lower()] = e

    # TKMK00 names: geometria exata medida no trace, separada do manifest
    # runtime geral. O renderer HD procura o hash dos bytes ORIGINAIS
    # (logical_hash), portanto nao usamos o runtime_hash aqui.
    names_runtime_manifest = {}
    names_manifest_path = indir / "texture_tkmk00_names_runtime_manifest.json"
    for e in load_manifest(names_manifest_path):
        if e.get("png") and e.get("symbol"):
            names_runtime_manifest[e["png"].replace("\\", "/").lower()] = e

    karts = {e["png"].replace("\\", "/").lower(): e
             for e in load_manifest(indir / "kart_sprite_manifest.json")}

    lakitu = {e["png"].replace("\\", "/").lower(): e
              for e in load_manifest(indir / "lakitu_sprite_manifest.json")}

    # Outros sprites de assets continuam como fallback do grupo de sprites,
    # mas nao entram no contador de Lakitu nem no de karts canonicos.
    asset_sprites = {e["png"].replace("\\", "/").lower(): e
                     for e in load_manifest(indir / "asset_sprite_manifest.json")}

    # Geometria PORTATIL dos pedacos de menu (menu_tiles_geometry.json, versionado
    # no repositorio). Tem so coordenadas; os hashes sao calculados aqui a
    # partir dos bancos gerados da ROM de quem esta usando -- assim funciona
    # com qualquer versao de ROM e sem precisar ligar o trace.
    geo_path = Path(__file__).resolve().parent / "menu_tiles_geometry.json"
    if geo_path.is_file():
        n_geo = aplicar_geometria(geo_path, generated)
        print(f"Geometria de menu: {n_geo} imagem(ns) (menu_tiles_geometry.json)")

    # Faixas MEDIDAS em runtime para as imagens grandes de menu (opcional).
    medido_path = indir / "menu_tiles_measured.json"
    if medido_path.is_file():
        try:
            medido = json.loads(medido_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            medido = {}
        aplicadas = 0
        for png_rel, info in medido.items():
            chave = png_rel.replace("\\", "/").lower()
            if chave in generated and info.get("tiles"):
                generated[chave] = dict(generated[chave])
                generated[chave]["tmem_halves"] = info["tiles"]
                aplicadas += 1
        print(f"Faixas medidas de menu: {aplicadas} imagem(ns) (menu_tiles_measured.json)")

    print(f"Manifests: {len(generated)} generated + {len(tkmk)} TKMK00 + "
          f"{len(karts)} karts + {len(lakitu)} lakitu")
    if a.diagnose_yoshi:
        _diagnose_yoshi(indir, generated)
        return

    n_files = n_tiles = 0
    missed = []

    for png in indir.rglob("*.png"):
        rel = png.relative_to(indir).as_posix()
        if only_prefix and not rel.lower().startswith(only_prefix):
            continue

        key = rel.lower()
        entry = (generated.get(key) or tkmk.get(key) or
                 karts.get(key) or lakitu.get(key) or asset_sprites.get(key))
        m = NAME_RE.match(png.name)

        if entry is None and m is None:
            missed.append(rel)
            continue

        img = Image.open(png).convert("RGBA")
        w, hgt = img.size

        # TKMK00 EXATO: texture_name_* e texture_ok foram medidos como
        # imagens RGBA16 de dimensao real 64x12 / 31x19. Para essas entradas
        # nao aplicamos a antiga regra de G_LOADTILE inclusivo (+1 linha):
        # o hash usado pelo carregador HD e o hash dos bytes originais
        # (logical_hash). A geometria do JSON e usada somente para recortar
        # a arte HD na mesma janela logica.
        special = None
        if entry and entry.get("symbol") in {
            "texture_name_dk", "texture_name_toad", "texture_name_bowser",
            "texture_name_luigi", "texture_name_mario", "texture_name_peach",
            "texture_name_wario", "texture_name_yoshi", "texture_ok"
        }:
            special = names_runtime_manifest.get(key) or runtime_manifest.get(key)
        if special and special.get("tmem_tiles_runtime"):
            orig_w = int(special.get("width", entry.get("width", w)))
            orig_h = int(special.get("height", entry.get("height", hgt)))
            for i, t in enumerate(special["tmem_tiles_runtime"]):
                x0e = round(int(t.get("x0", 0)) / orig_w * w)
                x1e = round(int(t.get("x1", orig_w)) / orig_w * w)
                y0e = round(int(t.get("y0", 0)) / orig_h * hgt)
                y1e = round(int(t.get("y1", orig_h)) / orig_h * hgt)
                if x1e <= x0e:
                    x1e = x0e + 1
                if y1e <= y0e:
                    y1e = y0e + 1
                x1e = min(x1e, img.size[0])
                y1e = min(y1e, img.size[1])
                crop = img.crop((x0e, y0e, x1e, y1e))
                hd_hash = t.get("logical_hash") or t.get("hash")
                if not hd_hash:
                    continue
                write_tex(
                    outdir, hd_hash, crop,
                    f"{rel} [TKMK exact geometry {i}; logical hash]"
                )
                n_tiles += 1
            n_files += 1
            continue

        # TKMK00 RUNTIME V1: usa os hashes que o func_80095E10/gfx_pc.c
        # realmente calcula, sem tocar no renderer e sem ativar --tiles global.
        # A imagem HD recebe a mesma janela logica do tile + 1 linha, porque
        # G_LOADTILE usa coordenadas inclusivas. Quando essa linha ultrapassa
        # a borda, repetimos a ultima linha da arte HD para fornecer bytes
        # validos ao upload (o hash original pode vir do trace).
        rt = runtime_manifest.get(key) if entry and entry.get("symbol") else None
        if rt and rt.get("tmem_tiles_runtime"):
            orig_w = int(rt.get("width", entry.get("width", w)))
            orig_h = int(rt.get("height", entry.get("height", hgt)))
            need_h = max(hgt, round((max(int(t.get("y1", 0)) for t in rt["tmem_tiles_runtime"]) + 1) / orig_h * hgt))
            if need_h > hgt:
                ext = Image.new("RGBA", (w, need_h))
                ext.paste(img, (0, 0))
                last = img.crop((0, hgt - 1, w, hgt))
                for yy in range(hgt, need_h):
                    ext.paste(last, (0, yy))
                img = ext
            for i, t in enumerate(rt["tmem_tiles_runtime"]):
                x0e = round(int(t.get("x0", 0)) / orig_w * w)
                x1e = round(int(t.get("x1", orig_w)) / orig_w * w)
                y0e = round(int(t.get("y0", 0)) / orig_h * hgt)
                y1e = round((int(t.get("y1", orig_h)) + 1) / orig_h * hgt)
                if x1e <= x0e: x1e = x0e + 1
                if y1e <= y0e: y1e = y0e + 1
                y1e = min(y1e, img.size[1])
                crop = img.crop((x0e, y0e, x1e, y1e))
                if crop.width > a.max or crop.height > a.max:
                    print(f"  ! {rel} TKMK runtime {i}: {crop.width}x{crop.height} excede --max={a.max}, pulando")
                    continue
                # A ultima faixa le uma linha ALEM do fim da imagem (G_LOADTILE
                # inclusivo): o hash completo inclui bytes de outra textura que
                # estiver depois dela na memoria. Esse "lixo" muda quando o
                # jogador entra em outro menu e volta, e a faixa de baixo
                # passava a aparecer na versao original. O gfx_pc.c ja tenta um
                # segundo hash sem a linha extra (logical_hash); gravamos a faixa
                # tambem sob ele, para essa tentativa de reserva encontrar a HD.
                hash_full = t.get("hash")
                hash_logico = t.get("logical_hash")
                borda_sem_trace = (t.get("hash_source") == "computed"
                                   and int(t.get("y1", 0)) >= orig_h)
                gravou = False
                # Sem trace, o hash completo da borda depende de bytes que nao
                # conhecemos: nao o inventamos, so usamos o logico.
                if hash_full and not borda_sem_trace:
                    write_tex(outdir, hash_full, crop,
                              f"{rel} [TKMK runtime {i}; {t.get('hash_source','computed')}]")
                    gravou = True
                if hash_logico and hash_logico != hash_full:
                    write_tex(outdir, hash_logico, crop,
                              f"{rel} [TKMK runtime {i}; logical]")
                    gravou = True
                if gravou:
                    n_tiles += 1
            n_files += 1
            continue

        # TESTE CIRURGICO: logo_mario_kart_64.
        #
        # O runtime original carrega esta RGBA32 256x128 em 32 blocos de
        # 256x4 (4096 bytes). O HD continua sendo uma unica imagem 1024x512,
        # mas o PAK precisa registrar a substituicao sob o hash ORIGINAL de
        # cada bloco. Assim nao alteramos o gfx_pc.c nem o comportamento das
        # outras texturas de menu.
        #
        # Os quatro blocos superiores/inferiores transparentes compartilham
        # o mesmo hash original; as faixas HD correspondentes tambem sao
        # identicas, portanto o mecanismo de deduplicacao do PAK e seguro.
        if entry and entry.get("symbol") == "logo_mario_kart_64" and entry.get("tmem_tiles"):
            orig_w = int(entry["width"])
            orig_h = int(entry["height"])
            for i, t in enumerate(entry["tmem_tiles"]):
                x0 = round(int(t.get("x0", 0)) / orig_w * w)
                x1 = round(int(t.get("x1", orig_w)) / orig_w * w)
                y0 = round(int(t["y0"]) / orig_h * hgt)
                y1 = round(int(t["y1"]) / orig_h * hgt)
                if x1 <= x0:
                    x1 = x0 + 1
                if y1 <= y0:
                    y1 = y0 + 1
                crop = img.crop((x0, y0, x1, y1))
                if crop.width > a.max or crop.height > a.max:
                    print(f"  ! {rel} bloco {i}: {crop.width}x{crop.height} excede --max={a.max}, pulando")
                    continue
                write_tex(outdir, t["hash"], crop, f"{rel} [logo bloco {i}]")
                n_tiles += 1
            n_files += 1
            continue

        # Sprites CI8 (karts) sao carregados em metades de 64x32 por causa do
        # limite de TMEM (a TLUT ocupa metade dos 4KB). Cada metade tem hash
        # proprio em runtime -- sempre honrado, nao depende de --tiles.
        if entry and entry.get("tmem_halves"):
            halves = entry["tmem_halves"]
            orig_h = int(entry["height"])
            orig_w = int(entry["width"])
            # Blocos que passam da borda (ex: 33 linhas a partir de y=32 numa
            # imagem de 64): estende a arte HD repetindo a ultima linha/coluna,
            # para o recorte manter a proporcao exata do bloco original em vez
            # de esticar (o que criaria emenda visivel).
            max_y1 = max(int(t["y1"]) for t in halves)
            max_x1 = max(int(t.get("x1", orig_w)) for t in halves)
            need_h = max(hgt, round(max_y1 / orig_h * hgt))
            need_w = max(w, round(max_x1 / orig_w * w))
            if need_h > hgt or need_w > w:
                ext = Image.new("RGBA", (need_w, need_h))
                ext.paste(img, (0, 0))
                if need_h > hgt:
                    ultima = img.crop((0, hgt - 1, w, hgt))
                    for yy in range(hgt, need_h):
                        ext.paste(ultima, (0, yy))
                if need_w > w:
                    col = ext.crop((w - 1, 0, w, need_h))
                    for xx in range(w, need_w):
                        ext.paste(col, (xx, 0))
                img = ext
            # Usa as LINHAS reais de cada janela (y0/y1 do manifest), nao uma
            # divisao ao meio: as duas janelas se sobrepoem em uma linha
            # (a segunda comeca em 1984 bytes, nao 2048).
            for i, t in enumerate(halves):
                y0e = round(int(t["y0"]) / orig_h * hgt)
                y1e = round(int(t["y1"]) / orig_h * hgt)
                if y1e <= y0e:
                    y1e = y0e + 1
                if y1e > img.size[1]:
                    y1e = img.size[1]
                # Pedacos 2D (menus): x0/x1 opcionais; sem eles, largura inteira.
                x0e = round(int(t.get("x0", 0)) / orig_w * w)
                x1e = round(int(t.get("x1", orig_w)) / orig_w * w)
                if x1e <= x0e:
                    x1e = x0e + 1
                if x1e > img.size[0]:
                    x1e = img.size[0]
                crop = img.crop((x0e, y0e, x1e, y1e))
                cw, ch = crop.size
                if cw > a.max or ch > a.max:
                    print(f"  ! {rel} metade {i}: {cw}x{ch} excede --max={a.max}, pulando")
                    continue
                write_tex(outdir, t["hash"], crop)
                n_tiles += 1
                # Grava tambem sob os hashes alternativos (variantes de layout
                # de TMEM). Sao poucos arquivos a mais e garante que a metade
                # case independentemente de qual variante o jogo usa.
                for alt in t.get("alt_hashes", []):
                    if alt and alt != t["hash"]:
                        write_tex(outdir, alt, crop)
                        n_tiles += 1
            n_files += 1
            continue

        if a.tiles and entry and entry.get("tmem_tiles"):
            orig_h = int(entry["height"])
            tiles = entry["tmem_tiles"]
            # Fronteiras proporcionais calculadas uma unica vez, para as
            # fatias ficarem contiguas (sem sobreposicao/gap) na imagem editada.
            bounds = [round(t["y0"] / orig_h * hgt) for t in tiles] + [hgt]
            for i, t in enumerate(tiles):
                y0e, y1e = bounds[i], bounds[i + 1]
                if y1e <= y0e:
                    y1e = y0e + 1
                crop = img.crop((0, y0e, w, y1e))
                cw, ch = crop.size
                if cw > a.max or ch > a.max:
                    print(f"  ! {rel} faixa {i}: {cw}x{ch} excede --max={a.max}, pulando")
                    continue
                write_tex(outdir, t["hash"], crop)
                n_tiles += 1
            n_files += 1
            continue

        h = m.group(1).lower() if m else entry.get("decoded_hash_fnv1a32")
        if h is None:
            missed.append(rel)
            continue
        if w > a.max or hgt > a.max:
            print(f"  ! {rel}: {w}x{hgt} excede --max={a.max}, pulando")
            continue
        write_tex(outdir, h, img)
        n_files += 1

    # Blocos usados durante a ANIMACAO DE GIRO das imagens TKMK00 (selecao de
    # modo, nomes dos personagens), medidos por SCAN_TKMK_ANIM.py. Para girar,
    # o jogo desenha a imagem em blocos com outros recortes -- outros hashes --
    # e sem isto a animacao aparecia na versao original.
    # Le tambem texture_tkmk00_tinted_manifest.json (fundos coloridos dos menus,
    # gerados pelo TINT_MENU_BACKGROUNDS.py), no mesmo formato.
    for _nome_man in ("texture_tkmk00_anim_manifest.json",
                      "texture_tkmk00_tinted_manifest.json"):
        anim_path = indir / _nome_man
        if not anim_path.is_file():
            anim_path = (Path(__file__).resolve().parent / "manifest_runtime"
                         / _nome_man)
        if anim_path.is_file():
            n_anim = 0
            for e in load_manifest(anim_path):
                png = indir / e.get("png", "")
                tiles = e.get("tiles") or []
                if not png.is_file() or not tiles:
                    continue
                img_a = Image.open(png).convert("RGBA")
                wa, ha = img_a.size
                W, H = int(e["width"]), int(e["height"])
                # blocos que passam da borda: estende repetindo a ultima linha/coluna
                need_w = max(wa, round(max(int(t["x1"]) for t in tiles) / W * wa))
                need_h = max(ha, round(max(int(t["y1"]) for t in tiles) / H * ha))
                if need_w > wa or need_h > ha:
                    ext = Image.new("RGBA", (need_w, need_h))
                    ext.paste(img_a, (0, 0))
                    if need_h > ha:
                        ultima = img_a.crop((0, ha - 1, wa, ha))
                        for yy in range(ha, need_h):
                            ext.paste(ultima, (0, yy))
                    if need_w > wa:
                        col = ext.crop((wa - 1, 0, wa, need_h))
                        for xx in range(wa, need_w):
                            ext.paste(col, (xx, 0))
                    img_a = ext
                for i, t in enumerate(tiles):
                    x0e = round(int(t["x0"]) / W * wa); x1e = round(int(t["x1"]) / W * wa)
                    y0e = round(int(t["y0"]) / H * ha); y1e = round(int(t["y1"]) / H * ha)
                    x1e = max(x1e, x0e + 1); y1e = max(y1e, y0e + 1)
                    crop = img_a.crop((x0e, y0e, min(x1e, img_a.size[0]), min(y1e, img_a.size[1])))
                    if crop.width > a.max or crop.height > a.max:
                        continue
                    gravados = set()
                    for chave in (t.get("hash"), t.get("logical_hash")):
                        if chave and chave not in gravados:
                            write_tex(outdir, chave, crop, f"{e.get('png')} [TKMK anim {i}]")
                            gravados.add(chave)
                    if gravados:
                        n_anim += 1
            print(f"Blocos TKMK00 extras: {n_anim} ({anim_path.name})")

    if PAK_MODE:
        # tex.pak: cabecalho + indice + dados. Um unico arquivo, aberto uma vez
        # pelo jogo; cada textura vira um seek+read em vez de abrir arquivo.
        pak = outdir / "tex.pak"
        if a.dxt:
            try:
                import numpy  # noqa: F401
            except ImportError:
                sys.exit("--dxt precisa do numpy: pip install numpy")

        # ordem dos dados: grupo de pre-carregamento primeiro (contiguo, por
        # prioridade), depois o resto na ordem original
        cand = []
        for k, (hh, w, h, raw) in enumerate(PAK_ENTRIES):
            pr = _prioridade_preload(_PAK_SOURCE.get(hh, ""))
            if pr is not None:
                cand.append((pr, _PAK_SOURCE.get(hh, ""), k))
        cand.sort()
        pre_idx, pre_bytes = [], 0
        for pr, _src, k in cand:
            tam = len(PAK_ENTRIES[k][3])
            if pre_bytes + tam > PRELOAD_MB * 1024 * 1024:
                continue
            pre_idx.append(k)
            pre_bytes += tam
        pre_set = set(pre_idx)
        ordem = pre_idx + [k for k in range(len(PAK_ENTRIES)) if k not in pre_set]

        # versao 2: 24 bytes por entrada; o ultimo campo leva o formato
        # (0 RGBA32, 1 DXT1, 2 DXT5) e, no bit 31, a marca de pre-carregamento
        header = struct.pack(">III", PAK_MAGIC, 2, len(PAK_ENTRIES))
        index_size = len(PAK_ENTRIES) * 24
        data_off = len(header) + index_size
        index, blob, cur = b"", [], data_off
        cont = {0: 0, 1: 0, 2: 0}
        antes = depois = 0
        if a.dxt:
            print(f"comprimindo {len(PAK_ENTRIES)} texturas em DXT...")
        for n_i, k in enumerate(ordem, 1):
            hh, w, h, raw = PAK_ENTRIES[k]
            fmt, dados = 0, raw
            if a.dxt and w % 4 == 0 and h % 4 == 0 and hh.lower() not in DXT_NUNCA:
                fmt, dados = dxt_encode(raw, w, h)
            cont[fmt] += 1
            antes += len(raw); depois += len(dados)
            campo = fmt | (0x80000000 if k in pre_set else 0)
            index += struct.pack(">IIIIII", int(hh, 16), cur, len(dados), w, h, campo)
            blob.append(dados)
            cur += len(dados)
            if a.dxt and n_i % 1000 == 0:
                print(f"  {n_i}/{len(PAK_ENTRIES)}")
        if a.dxt:
            print(f"DXT: {cont[1]} DXT1, {cont[2]} DXT5, {cont[0]} sem compressao "
                  f"| {antes/1024/1024:.0f} MB -> {depois/1024/1024:.0f} MB")
        print(f"pre-carregamento dos menus: {len(pre_idx)} texturas, "
              f"{pre_bytes/1024/1024:.1f} MB (limite {PRELOAD_MB} MB)")
        with open(pak, "wb") as f:
            f.write(header); f.write(index)
            for b in blob:
                f.write(b)
        print(f"\n{n_files} imagens processadas ({n_tiles} em faixas de TMEM)")
        print(f"tex.pak: {len(PAK_ENTRIES)} texturas unicas, "
              f"{pak.stat().st_size/1024/1024:.1f} MB em UM arquivo")
        print(f"  hashes duplicados byte-identicos deduplicados: {_HASH_DEDUP_COUNT}")
        if _HASH_COLLISIONS:
            report = outdir / "tex_hash_collisions.json"
            report.write_text(json.dumps(_HASH_COLLISIONS, indent=2), encoding="utf-8")
            print(f"  ! {len(_HASH_COLLISIONS)} colisao(oes) FNV32 diferentes: {report}")
        print("")
        print(f"  ATENCAO: copie o arquivo {pak.name} para a RAIZ da pasta do jogo")
        print("  no console, AO LADO do MK64.xex -- NAO dentro de uma pasta tex\\.")
        print("  Estrutura correta no console:")
        print("      MK64.xex")
        print("      baserom.us.z64")
        print("      tex.pak")
        return

    print(f"\n{n_files} imagens processadas ({n_tiles} delas expandidas em faixas de TMEM)")
    total = list(outdir.rglob("*.tex"))
    total_mb = sum(p.stat().st_size for p in total) / 1024 / 1024
    print(f"{len(total)} arquivos .tex em {outdir}\\  ({total_mb:.1f} MB)")
    print(f"Hashes duplicados byte-identicos deduplicados: {_HASH_DEDUP_COUNT}")
    if _HASH_COLLISIONS:
        report = outdir / "tex_hash_collisions.json"
        report.write_text(json.dumps(_HASH_COLLISIONS, indent=2), encoding="utf-8")
        print(f"! {len(_HASH_COLLISIONS)} colisao(oes) FNV32 diferentes: {report}")
    if missed:
        print(f"{len(missed)} PNGs sem hash conhecido (nao empacotados):")
        for r in missed[:15]:
            print("  -", r)
        if len(missed) > 15:
            print(f"  ... e mais {len(missed) - 15}")
    print("Copie a pasta para o Xbox 360 como game:\\tex\\")


if __name__ == "__main__":
    main()
