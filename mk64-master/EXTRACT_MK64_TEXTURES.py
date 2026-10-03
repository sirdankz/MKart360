#!/usr/bin/env python3
r"""
Extrai as texturas do MK64 para PNG usando PUBLIC_ASSET_RECIPES.json + a ROM,
com dimensoes e paleta lidas dos YAMLs do projeto (região selecionável; fallback para us).

Versão Final Otimizada e Corrigida com Suporte Completo ao Lakitu:
  1. Restauração completa de 100% dos 1041 bancos gerados (apenas 7 microcódigos/não-imagens ignorados).
  2. Extração completa de todos os frames e animações do Lakitu (assets/lakitu/*.json),
     gerando lakitu_sprite_manifest.json com cálculo exato de tmem_halves e hashes FNV1a32.
  3. Mapeamento de nomes amigáveis para other_textures (além de gTextureXXXX, gera também cópias com nomes identificáveis como lakitu_*).
  4. Transparência Universal (Global): fundos pretos indesejados eliminados em minimapas, velocímetro, HUD, placas e fontes (I8/I4/CI).
  5. Opção --force para proteger PNGs já editados e --keep-black caso deseje desativar a transparência.
  6. Eliminação do SyntaxWarning de escape (\E) no Windows/Python 3.12+.
  7. Geração de manifests no formato rom_offset/compression/decoded_size/hash
     usado pelo sistema HD, com manifest consolidado + manifests por banco.
  8. Manifest separado para TKMK00, incluindo hash dos pixels RGBA16 decodificados
     e hash de cada tile de 4KB realmente carregado pelo RDP.

  9. Fluxo integrado de fundos coloridos de menu HD: na primeira execução,
     preserva os fundos originais em extracted_textures\originais; nas
     execuções seguintes detecta a arte HD, pergunta se deve gerar os fundos
     rosa/verde/azul e reproduz a mesma lógica matemática do jogo.
 10. Todos os arquivos de manifest_runtime são sincronizados para
     extracted_textures, para que o PACK_TEXTURES.py encontre os manifests
     junto dos PNGs sem etapa manual.

Uso (dentro de mk64-master):
    py .\EXTRACT_MK64_TEXTURES.py --rom .\baserom.br.z64 --region br
    py .\EXTRACT_MK64_TEXTURES.py --rom .\baserom.us.z64 --region us
    py .\EXTRACT_MK64_TEXTURES.py --rom .\baserom.br.z64 --force
"""
from pathlib import Path
import argparse, hashlib, json, re, struct, sys, zlib
from collections import defaultdict
import shutil

# ---------------------------------------------------------------- Configuração global

SKIP_EXISTING_PNGS = False  # controlado por --protect-existing / --force
TRANSPARENT_BLACK = True    # controlado por --keep-black; aplica transparência automática em I8, I4, CI e minimapas

# ---------------------------------------------------------------- MIO0

def mio0_decode(src):
    if len(src) < 16 or src[:4] != b"MIO0":
        raise ValueError("not MIO0")
    out_size = int.from_bytes(src[4:8], "big")
    comp_pos = int.from_bytes(src[8:12], "big")
    raw_pos = int.from_bytes(src[12:16], "big")
    ctrl_pos, mask, ctrl = 16, 0, 0
    out = bytearray()
    while len(out) < out_size:
        if mask == 0:
            ctrl = src[ctrl_pos]; ctrl_pos += 1; mask = 0x80
        if ctrl & mask:
            out.append(src[raw_pos]); raw_pos += 1
        else:
            w = int.from_bytes(src[comp_pos:comp_pos + 2], "big"); comp_pos += 2
            length = (w >> 12) + 3
            dist = (w & 0xFFF) + 1
            start = len(out) - dist
            for i in range(length):
                out.append(out[start + i])
        mask >>= 1
    return bytes(out[:out_size])


def source_bytes(rom, offset, cache, raw_size=None):
    """Retorna um bloco MIO0 descomprimido ou bytes crus da ROM.

    Para dados crus, ``rom[offset:]`` e a ROM inteira restante. Armazenar isso
    no cache uma vez por paleta acabava consumindo varios GB ao exportar todos
    os karts. ``raw_size`` limita o bloco guardado ao tamanho realmente usado.
    """
    cache_key = (offset, raw_size) if raw_size is not None else (offset, None)
    if cache_key in cache:
        return cache[cache_key]
    if rom[offset:offset + 4] == b"MIO0":
        data = mio0_decode(rom[offset:])
    else:
        data = rom[offset:offset + raw_size] if raw_size is not None else rom[offset:]
    cache[cache_key] = data
    return data

# ---------------------------------------------------------------- PNG (puro, sem PIL)

def write_png(path, w, h, rgba, overwrite=True):
    # Os fundos de menu podem ser substituídos por arte HD entre duas
    # execuções. Nunca sobrescreva uma imagem que já tenha dimensões HD.
    if (path.is_file() and path.name in ("background_blue_sky.png", "background_sunset.png")
            and "generated" in path.parts and "texture_tkmk00" in path.parts):
        old_size = _png_size(path)
        if old_size and old_size != (w, h):
            return
    if not overwrite and path.is_file() and path.stat().st_size > 0:
        return
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)

# ---------------------------------------------------------------- fnv1a & TMEM Halves

def fnv1a32(data):
    """Hash FNV-1a 32-bit idêntico ao patch C e ao formato dos manifestos do port."""
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


RED_SHELL_HD_SALT = b"MK64_X360_RED_SHELL\x00"

def red_shell_hd_hash(data):
    """Identidade HD separada para o mesmo CI8 usado pelo Red Shell.

    O jogo original troca somente a TLUT; os indices CI8 permanecem iguais.
    Portanto o lookup normal teria exatamente o mesmo hash do Green Shell.
    Esta variante adiciona um domínio fixo somente quando a TLUT vermelha esta
    ativa no renderer Xbox 360. Karts/Lakitu nao passam por este caminho.
    """
    return fnv1a32(RED_SHELL_HD_SALT + bytes(data))


def red_shell_rgba(rgba):
    """Reproduz a transformacao da TLUT do jogo sobre o PNG decodificado.

    init_red_shell_texture() troca os canais R/G de cada entrada RGBA16
    (5 bits por canal). Como o PNG ja foi decodificado a partir da TLUT,
    a mesma operacao no RGBA resultante e simplesmente R <-> G.
    """
    out = bytearray(rgba)
    for i in range(0, len(out), 4):
        out[i], out[i + 1] = out[i + 1], out[i]
    return bytes(out)


def tmem_halves_for(pixels, w, h=None, is_kart=False):
    """Calcula os dois blocos de carregamento no TMEM do N64 (com 1 linha de sobreposição).

    Para Lakitu (ex.: 56x72):
      - Top half:    y0=0,  y1=36 (half_h)
      - Bottom half: y0=35 (half_h - 1), y1=72 (h)
    Para Karts (ex.: 64x64):
      - Top half:    y0=0,  y1=32
      - Bottom half: y0=31, y1=63
    """
    if h is None:
        h = len(pixels) // w if w > 0 else 0
    if h <= 0 or w <= 0:
        return []

    half_h = h // 2
    # TMEM uses two equal-sized chunks with one overlapping row.
    # The end coordinate is EXCLUSIVE because the pixel buffer is sliced
    # as pixels[y0 * w : y1 * w].
    #
    # Example 64x64:
    #   top    = 0:32  -> 32 rows
    #   bottom = 31:63 -> 32 rows
    y0_0, y1_0 = 0, half_h
    y0_1, y1_1 = max(0, half_h - 1), max(0, h - 1)

    half0_data = pixels[0 : y1_0 * w]
    half1_data = pixels[max(0, half_h - 1) * w : max(0, h - 1) * w]

    return [
        {
            "y0": y0_0,
            "y1": y1_0,
            "hash": f"{fnv1a32(half0_data):08x}"
        },
        {
            "y0": y0_1,
            "y1": y1_1,
            "hash": f"{fnv1a32(half1_data):08x}"
        }
    ]


# ---------------------------------------------------------------- manifest dos bancos gerados

def tmem_layout_for(width, height, fmt):
    """Calcula a divisao real necessaria para carregar uma textura na TMEM.

    A regra aqui segue a tabela de limites do N64:
      RGBA32      32x32
      RGBA16/IA16 64x32 ou 32x64
      I8/IA8      64x64
      I4/IA4      128x64 ou 64x128
      CI8         64x32 (2048 bytes de texels + 2048 de TLUT)
      CI4         64x64 (2048 bytes de texels + 2048 de TLUT)

    Importante: nao usamos somente ``bytes <= 4096`` para decidir. Uma
    textura pode ter poucos bytes e ainda assim ultrapassar o limite de
    dimensao/stride do formato (por exemplo RGBA16 45x45). Nesses casos ela
    tambem precisa ser particionada.
    """
    fmt = str(fmt).lower()
    bits = BPP.get(fmt)
    if bits is None:
        return None

    width = int(width)
    height = int(height)
    indexed = fmt in ("ci4", "ci8")
    capacity = 2048 if indexed else 4096
    bpp = bits / 8.0

    # Cada tupla representa uma orientacao maxima de um carregamento TMEM.
    orientations = {
        "rgba32": [(32, 32)],
        "rgba16": [(64, 32), (32, 64)],
        "ia16":   [(64, 32), (32, 64)],
        "ia8":    [(64, 64)],
        "i8":     [(64, 64)],
        "ci8":    [(64, 32)],
        "ia4":    [(128, 64), (64, 128)],
        "i4":     [(128, 64), (64, 128)],
        "ci4":    [(64, 64)],
    }
    candidates = orientations.get(fmt)
    if not candidates:
        return None

    # Se uma orientacao inteira cabe, nao ha motivo para criar tiles.
    for max_w, max_h in candidates:
        if width <= max_w and height <= max_h:
            return None

    # Escolhe a orientacao que produz o maior tile valido para esta textura.
    valid = []
    for max_w, max_h in candidates:
        tw = min(width, max_w)
        th = min(height, max_h)
        # Ajusta a altura para garantir o limite de bytes mesmo nos casos
        # de dimensoes nao padrao.
        max_rows_by_tmem = max(1, int(capacity // (tw * bpp)))
        th = min(th, max_rows_by_tmem)
        if tw > 0 and th > 0:
            valid.append((tw * th, tw, th, max_w, max_h))

    if not valid:
        return None

    _, tile_w, tile_h, _, _ = max(valid, key=lambda v: v[0])

    tiles = []
    y = 0
    while y < height:
        y1 = min(height, y + tile_h)
        x = 0
        while x < width:
            x1 = min(width, x + tile_w)
            tiles.append({"x0": x, "y0": y, "x1": x1, "y1": y1})
            x = x1
        y = y1

    return {
        "capacity_bytes": capacity,
        "palette_bytes": 2048 if indexed else 0,
        "indexed": indexed,
        "tile_width": tile_w,
        "tile_height": tile_h,
        "tile_count": len(tiles),
        "tiles": tiles,
    }

def _tile_bytes(data, width, fmt, x0, y0, x1, y1):
    """Extrai os bytes de um tile mantendo a ordem de texels da ROM."""
    bits = BPP[fmt]
    if bits >= 8:
        bpp = bits // 8
        row_bytes = int(width) * bpp
        out = bytearray()
        for y in range(y0, y1):
            a = y * row_bytes + x0 * bpp
            b = y * row_bytes + x1 * bpp
            out += data[a:b]
        return bytes(out)

    # I4/IA4/CI4: dois texels por byte. Os limites produzidos acima sao pares
    # para os formatos em que a largura pode precisar de divisao.
    out = bytearray()
    row_bytes = (int(width) + 1) // 2
    for y in range(y0, y1):
        row = data[y * row_bytes:(y + 1) * row_bytes]
        nibbles = []
        for value in row:
            nibbles.append((value >> 4) & 0xF)
            nibbles.append(value & 0xF)
        nibbles = nibbles[x0:x1]
        for i in range(0, len(nibbles), 2):
            hi = nibbles[i] << 4
            lo = nibbles[i + 1] if i + 1 < len(nibbles) else 0
            out.append(hi | lo)
    return bytes(out)


def menu_tmem_layout_for(width, height):
    """Replicate func_80095E10(), used by render_menu_textures().

    The N64 menu renderer does not use a generic 64x32 TMEM grid. It chooses
    the load width as the next power of two >= texture width and the load
    height as 0x400 / load_width, then emits gDPLoadTextureTile() calls.
    This is the actual partition that gfx_pc.c hashes at runtime.
    """
    width, height = int(width), int(height)
    if width <= 0 or height <= 0:
        return None
    tile_w = 1
    while tile_w < width:
        tile_w <<= 1
    tile_h = 0x400 // tile_w
    while (tile_h // 2) > height:
        tile_h //= 2
    tile_h = max(1, tile_h)
    tiles = []
    y = 0
    while y < height:
        y1 = min(height, y + tile_h)
        x = 0
        while x < width:
            x1 = min(width, x + tile_w)
            tiles.append({"x0": x, "y0": y, "x1": x1, "y1": y1})
            x = x1
        y = y1
    return {"tile_width": tile_w, "tile_height": tile_h, "tile_count": len(tiles), "tiles": tiles}

def rgba32_block_layout(width, height):
    """Replicate render_texture_tile_rgba32_block()."""
    tex_size = int(width) * int(height) * 4
    num_blocks = (tex_size + 4095) // 4096
    if num_blocks <= 0:
        return None
    block_h = max(1, int(height) // num_blocks)
    tiles = []
    y = 0
    remaining = int(height)
    for _ in range(num_blocks):
        h = block_h if remaining > block_h else remaining
        if h <= 0:
            break
        tiles.append({"x0": 0, "y0": y, "x1": int(width), "y1": y + h})
        y += h
        remaining -= h
    return {"tile_width": int(width), "tile_height": block_h, "tile_count": len(tiles), "tiles": tiles}

def fixed_frame_layout(width, height, frame_height):
    """Layout for vertically packed animated sprites such as gTextureGhosts."""
    width, height, frame_height = int(width), int(height), int(frame_height)
    if width <= 0 or height <= 0 or frame_height <= 0 or height % frame_height:
        return None
    tiles = []
    for y in range(0, height, frame_height):
        tiles.append({"x0": 0, "y0": y, "x1": width, "y1": min(height, y + frame_height)})
    return {"tile_width": width, "tile_height": frame_height, "tile_count": len(tiles), "tiles": tiles}

def add_specific_tiles(item, data, width, height, fmt, mode):
    if mode == "menu_80095E10":
        layout = menu_tmem_layout_for(width, height)
    elif mode == "rgba32_block":
        layout = rgba32_block_layout(width, height)
    elif mode == "ghost_frames_48x40":
        layout = fixed_frame_layout(width, height, 40)
    else:
        layout = None
    if not layout:
        return
    tiles = []
    for t in layout["tiles"]:
        raw = _tile_bytes(data, int(width), fmt, t["x0"], t["y0"], t["x1"], t["y1"])
        e = dict(t)
        e["decoded_size"] = len(raw)
        e["hash"] = f"{fnv1a32(raw):08x}"
        tiles.append(e)
    item["tmem_mode"] = mode
    item["tmem"] = {"capacity_bytes": 4096, "palette_bytes": 0, "indexed": fmt in ("ci4", "ci8"),
                    "tile_width": layout["tile_width"], "tile_height": layout["tile_height"],
                    "tile_count": len(tiles)}
    item["tmem_tiles"] = tiles

def add_tmem_tiles_to_manifest(item, data, width, height, fmt):
    """Adiciona tiles + hash FNV1a de cada faixa quando a imagem excede a TMEM."""
    layout = tmem_layout_for(width, height, fmt)
    if not layout:
        return
    tiles = []
    for t in layout["tiles"]:
        raw = _tile_bytes(data, int(width), fmt, t["x0"], t["y0"], t["x1"], t["y1"])
        entry = dict(t)
        entry["decoded_size"] = len(raw)
        entry["hash"] = f"{fnv1a32(raw):08x}"
        tiles.append(entry)

    item["tmem"] = {
        "capacity_bytes": layout["capacity_bytes"],
        "palette_bytes": layout["palette_bytes"],
        "indexed": layout["indexed"],
        "tile_width": layout["tile_width"],
        "tile_height": layout["tile_height"],
        "tile_count": len(tiles),
    }
    item["tmem_tiles"] = tiles


def build_generated_manifest_entry(out_rel, bank, symbol, rom_offset, data,
                                   width, height, fmt, compression,
                                   include_tmem=False):
    """Monta uma entrada no formato dos manifests usados pelo PACK_TEXTURES."""
    item = {
        "png": str(out_rel).replace("\\", "/"),
        "bank": bank,
        "symbol": symbol,
        "rom_offset": f"0x{int_value(rom_offset):X}",
        "width": int(width),
        "height": int(height),
        "format": str(fmt).lower(),
        "compression": compression,
        "decoded_size": len(data),
        "decoded_hash_fnv1a32": f"{fnv1a32(data):08x}",
    }

    needed = (int(width) * int(height) * BPP[item["format"]] + 7) // 8 \
        if item["format"] in BPP else None

    # Os seis frames CI8 crus do Lakitu já fazem parte do banco
    # other_textures.c e são mantidos no formato histórico do manifest:
    # sem dimensions_size_verified/verification_note.
    special_raw_ci8 = (
        bank == "other_textures.c"
        and item["format"] == "ci8"
        and compression == "raw"
    )

    if not special_raw_ci8:
        item["dimensions_size_verified"] = (needed == len(data)) if needed is not None else False
        item["verification_note"] = "ROM-decoded"

    if include_tmem:
        item["tmem_halves"] = tmem_halves_for(data, int(width), int(height))

    # Only add partitions when we know the actual renderer that consumes the
    # texture. Generic TMEM geometry is NOT sufficient because MK64 uses
    # several different gDPLoad* paths.
    mode = None
    if bank in ("course_player_selection.c", "texture_data_2.c") and item["format"] == "rgba16":
        mode = "menu_80095E10"
    elif bank == "other_textures.c" and symbol == "logo_mario_kart_64" and item["format"] == "rgba32":
        mode = "rgba32_block"
    elif bank == "other_textures.c" and symbol == "gTextureGhosts" and item["format"] == "ci8":
        mode = "ghost_frames_48x40"
    if mode:
        add_specific_tiles(item, data, int(width), int(height), item["format"], mode)
    return item


# ---------------------------------------------------------------- parser YAML minimo

def parse_yaml_symbols(text):
    """Parser simples para o formato usado pelos yamls/us/*.yml."""
    out = {}
    current = None
    for raw_line in text.splitlines():
        if not raw_line.strip() or raw_line.strip().startswith("#"):
            continue
        if raw_line[0] not in (" ", "\t") and raw_line.rstrip().endswith(":"):
            current = raw_line.strip()[:-1]
            out[current] = {}
            continue
        m = re.match(r"^\s+([A-Za-z_][A-Za-z0-9_]*):\s*(.*)$", raw_line)
        if m and current is not None:
            key, val = m.group(1), m.group(2).strip().strip('"').strip("'")
            out[current][key] = val
    return out


def load_all_yaml_symbols(root, region="auto"):
    """Carrega os YAMLs da região pedida. Se ela não existir, usa US."""
    symbols = {}
    dirs = []
    if region and region != "auto":
        dirs.append(root / "yamls" / region)
    dirs.append(root / "yamls" / "us")
    seen = set()
    for yaml_dir in dirs:
        if not yaml_dir.is_dir() or yaml_dir in seen:
            continue
        seen.add(yaml_dir)
        for p in yaml_dir.glob("*.yml"):
            try:
                text = p.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            symbols.update(parse_yaml_symbols(text))
    return symbols


def load_asset_json_symbols(root):
    """Carrega metadados de todos os JSONs, inclusive o assets.json da raiz e assets/**/*.json."""
    symbols = {}

    def add(symbol, info, fmt_hint=None):
        if not isinstance(symbol, str) or not isinstance(info, dict):
            return
        width = height = None
        fmt = None
        if 'width' in info and 'height' in info:
            width, height = info['width'], info['height']
            fmt = info.get('type', info.get('format'))
        meta = info.get('meta')
        if isinstance(meta, dict) and isinstance(meta.get('dims'), (list, tuple)) and len(meta['dims']) >= 2:
            width, height = meta['dims'][0], meta['dims'][1]
            fmt = fmt or fmt_hint
        try:
            width, height = int(width), int(height)
        except (TypeError, ValueError):
            return
        if width <= 0 or height <= 0:
            return
        item = {'width': width, 'height': height}
        if fmt:
            item['type'] = str(fmt).lower()
        for key in ('tlut', 'tlut_symbol', 'rom_offset', 'block_offset', 'size', 'output_dir'):
            if key in info:
                item[key] = info[key]
        symbols[symbol] = item

    paths = []
    root_assets = root / 'assets.json'
    if root_assets.is_file():
        paths.append(root_assets)
    assets_dir = root / 'assets'
    if assets_dir.is_dir():
        paths.extend(assets_dir.rglob('*.json'))

    for path in paths:
        try:
            entries = json.loads(path.read_text(encoding='utf-8'))
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(entries, dict):
            continue
        for key, info in entries.items():
            if not isinstance(info, dict):
                continue
            if 'width' in info and 'height' in info:
                add(key, info)
                continue
            fmt_hint = None
            m = SOURCE_FMT_RE.search(str(key).replace('.png', '.inc.c'))
            if m:
                fmt_hint = m.group(1).lower()
            elif isinstance(key, str):
                fm = re.search(r'\.(rgba32|rgba16|ia16|ia8|ia4|i8|i4|ci8|ci4)(?:\.png)?$', key, re.I)
                if fm:
                    fmt_hint = fm.group(1).lower()
            if isinstance(key, str):
                base = Path(key).name
                base = re.sub(r'\.(png|inc\.c)$', '', base, flags=re.I)
                base = re.sub(r'\.(rgba32|rgba16|ia16|ia8|ia4|i8|i4|ci8|ci4)$', '', base, flags=re.I)
                add(base, info, fmt_hint)

    other_s = root / "data" / "other_textures.s"
    if other_s.is_file():
        try:
            text = other_s.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            text = ""
        for generated_symbol, inc in re.findall(
                r"glabel\s+([A-Za-z_][A-Za-z0-9_]*)\s*\n\s*\.incbin\s+\"([^\"]+)\"",
                text):
            base = Path(inc.replace("\\", "/")).name
            base = re.sub(r"\.(png|mio0|inc\.c)$", "", base, flags=re.I)
            base = re.sub(
                r"\.(rgba32|rgba16|ia16|ia8|ia4|i8|i4|ci8|ci4|tlut)$",
                "", base, flags=re.I)
            info = symbols.get(base)
            if info is not None and generated_symbol not in symbols:
                symbols[generated_symbol] = dict(info)

    return symbols


def load_other_textures_file_map(root):
    """Mapeia símbolos de other_textures para seus caminhos/nomes originais em incbin."""
    other_s = root / "data" / "other_textures.s"
    mapping = {}
    if not other_s.is_file():
        return mapping
    try:
        text = other_s.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return mapping
    for sym, inc in re.findall(r"glabel\s+([A-Za-z_][A-Za-z0-9_]*)\s*\n\s*\.incbin\s+\"([^\"]+)\"", text):
        clean_path = inc.replace("\\", "/")
        stem = Path(clean_path).name
        stem = re.sub(r"\.(mio0|png|inc\.c)$", "", stem, flags=re.I)
        stem = re.sub(r"\.(rgba32|rgba16|ia16|ia8|ia4|i8|i4|ci8|ci4)$", "", stem, flags=re.I)
        mapping[sym] = {"path": clean_path, "stem": stem}
    return mapping


# ---------------------------------------------------------------- bancos gerados do port 360

MENU_TEXTURE_RE = re.compile(
    r"\{\s*(-?\d+)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(\d+)\s*,\s*(\d+)\s*,"
)


def load_menu_texture_metadata(root):
    """Lê tipo/largura/altura das entradas MenuTexture do próprio jogo."""
    table = root / "src" / "data" / "textures.c"
    if not table.is_file():
        return {}
    text = table.read_text(encoding="utf-8", errors="ignore")
    metadata = {}
    for tex_type, symbol, w, h in MENU_TEXTURE_RE.findall(text):
        metadata[symbol] = {
            "type": int(tex_type),
            "width": int(w),
            "height": int(h),
        }
    return metadata


def load_menu_texture_dims(root):
    """Lê largura/altura da tabela MenuTexture do próprio jogo."""
    return {symbol: (info["width"], info["height"])
            for symbol, info in load_menu_texture_metadata(root).items()}


# ---------------------------------------------------------------- análise de uso no código-fonte dos bancos gerados

LOAD_TEXTURE_RE = re.compile(
    r"gDPLoadTexture(?:Block|Tile|MultiBlock)\s*\([^;]*?\b"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"
    r"G_IM_FMT_([A-Za-z0-9_]+)\s*,\s*G_IM_SIZ_([A-Za-z0-9_]+)\s*,\s*"
    r"(\d+)\s*,\s*(\d+)\s*,",
    re.S,
)

DMA_ALIAS_RE = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?:\([^)]*\)\s*)?"
    r"dma_textures\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,",
)

FMT_MAP = {
    "RGBA": "rgba16", "IA": "ia8", "I": "i8", "CI": "ci8",
}
SIZ_MAP = {"4b": 4, "8b": 8, "16b": 16, "32b": 32}

COURSE_TEX_INCLUDE_RE = re.compile(
    r"\bu8\s+([A-Za-z_][A-Za-z0-9_]*)\s*\[\]\s*=\s*\{\s*"
    r"#include\s+\"([^\"]+)\"\s*\}", re.S)
COURSE_TEX_IMAGE_RE = re.compile(
    r"gsDPSetTextureImage\s*\(\s*G_IM_FMT_([A-Za-z0-9_]+)\s*,\s*"
    r"G_IM_SIZ_([A-Za-z0-9_]+)\s*,\s*[^,]+,\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*\)")
COURSE_TILE_RE = re.compile(
    r"gsDPSetTileSize\s*\(\s*[^,]+,\s*[^,]+,\s*[^,]+,\s*"
    r"(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)")
OTHER_TEXTURE_INC_RE = re.compile(
    r"glabel\s+([A-Za-z_][A-Za-z0-9_]*)\s*\n\s*"
    r"\.incbin\s+\"([^\"]+)\"", re.M)


def _fmt_from_gbi(fmt_name, siz_name):
    fmt_name = fmt_name.upper()
    siz_name = siz_name.lower()
    bits = SIZ_MAP.get(siz_name)
    if fmt_name == "RGBA" and bits == 16: return "rgba16"
    if fmt_name == "RGBA" and bits == 32: return "rgba32"
    if fmt_name == "IA" and bits == 4: return "ia4"
    if fmt_name == "IA" and bits == 8: return "ia8"
    if fmt_name == "IA" and bits == 16: return "ia16"
    if fmt_name == "I" and bits == 4: return "i4"
    if fmt_name == "I" and bits == 8: return "i8"
    if fmt_name == "CI" and bits == 4: return "ci4"
    if fmt_name == "CI" and bits == 8: return "ci8"
    return None


def load_course_texture_usage(root):
    """Resolve dimensões de texturas de percurso pelos display lists."""
    data_s = root / "data" / "other_textures.s"
    if not data_s.is_file():
        return {}, {}
    try:
        text = data_s.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return {}, {}
    file_to_symbol = {}
    file_to_fmt = {}
    for sym, inc in OTHER_TEXTURE_INC_RE.findall(text):
        name = Path(inc.replace("\\", "/")).name
        file_to_symbol[name] = sym
        m = re.search(r"\.(rgba32|rgba16|ia16|ia8|ia4|i8|i4|ci8|ci4)\.mio0$", name, re.I)
        if m:
            file_to_fmt[sym] = m.group(1).lower()

    alias_to_symbol = {}
    for path in (root / "courses").glob("*/course_textures.linkonly.c"):
        try: t = path.read_text(encoding="utf-8", errors="ignore")
        except OSError: continue
        for alias, inc in COURSE_TEX_INCLUDE_RE.findall(t):
            name = Path(inc.replace("\\", "/")).name
            sym = file_to_symbol.get(name)
            if sym is None:
                stem = re.sub(r"\.(?:inc\.c|mio0)$", "", name, flags=re.I)
                for fn, fsym in file_to_symbol.items():
                    if re.sub(r"\.mio0$", "", fn, flags=re.I) == stem:
                        sym = fsym
                        break
            if sym:
                alias_to_symbol[alias] = sym

    usages = defaultdict(list)
    for path in (root / "courses").glob("*/course_displaylists.inc.c"):
        try: t = path.read_text(encoding="utf-8", errors="ignore")
        except OSError: continue
        images = list(COURSE_TEX_IMAGE_RE.finditer(t))
        for i, m in enumerate(images):
            fmt = _fmt_from_gbi(m.group(1), m.group(2))
            alias = m.group(3)
            sym = alias_to_symbol.get(alias)
            if not sym or not fmt:
                continue
            end = images[i+1].start() if i+1 < len(images) else min(len(t), m.end()+1800)
            segment = t[m.end():end]
            tm = COURSE_TILE_RE.search(segment)
            if not tm:
                continue
            lrs = int(tm.group(1), 0)
            lrt = int(tm.group(2), 0)
            w = (lrs >> 2) + 1
            h = (lrt >> 2) + 1
            if w > 0 and h > 0 and w <= 1024 and h <= 1024:
                usages[sym].append((w, h, fmt, alias))

    resolved, conflicts = {}, {}
    for sym, vals in usages.items():
        uniq = {(w,h,fmt) for w,h,fmt,_ in vals}
        if len(uniq) == 1:
            resolved[sym] = next(iter(uniq))
        elif len(uniq) > 1:
            conflicts[sym] = vals
    return resolved, conflicts


def load_source_texture_usage(root):
    """Resolve dimensoes/formato pelo uso real, inclusive atraves de aliases."""
    source_dir = root / "src"
    if not source_dir.is_dir():
        return {}, {}

    texts = []
    for path in source_dir.rglob("*.c"):
        try:
            texts.append(path.read_text(encoding="utf-8", errors="ignore"))
        except OSError:
            pass

    aliases = defaultdict(set)
    for text in texts:
        for alias, source in DMA_ALIAS_RE.findall(text):
            aliases[alias].add(source)

    for _ in range(8):
        changed = False
        for alias, sources in list(aliases.items()):
            expanded = set()
            for source in sources:
                expanded.update(aliases.get(source, {source}))
            if expanded != sources:
                aliases[alias] = expanded
                changed = True
        if not changed:
            break

    usages = defaultdict(set)

    def add(symbol, w, h, fmt):
        if symbol in aliases:
            for original in aliases[symbol]:
                usages[original].add((w, h, fmt))
        else:
            usages[symbol].add((w, h, fmt))

    for text in texts:
        for m in LOAD_TEXTURE_RE.finditer(text):
            expr, fmt_name, siz_name, w, h = m.groups()
            fmt_name = fmt_name.upper()
            bits = SIZ_MAP.get(siz_name.lower())
            if fmt_name not in FMT_MAP or bits is None:
                continue
            fmt = FMT_MAP[fmt_name]
            if bits == 4:
                fmt = "i4" if fmt == "i8" else ("ci4" if fmt == "ci8" else fmt)
            elif bits == 8:
                fmt = "i8" if fmt == "i8" else ("ci8" if fmt == "ci8" else fmt)
            elif bits == 16:
                fmt = "rgba16" if fmt == "rgba16" else ("ia16" if fmt == "ia8" else fmt)
            elif bits == 32:
                fmt = "rgba32" if fmt == "rgba16" else fmt
            add(expr, int(w), int(h), fmt)

        for helper_re, helper_fmt in SOURCE_HELPER_TEX_RES:
            for m in helper_re.finditer(text):
                add(m.group('symbol'), int(m.group('w'), 0), int(m.group('h'), 0), helper_fmt)

        init_re = re.compile(
            r'\binit_texture_object\s*\(\s*[^,]+,\s*(?P<tlut>[^,]+),\s*'
            r'(?:\(\s*[A-Za-z_][A-Za-z0-9_]*\s*\*?\s*\)\s*)?'
            r'(?P<tex>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*'
            r'(?P<w>0x[0-9A-Fa-f]+|\d+)\s*,\s*'
            r'(?P<h>0x[0-9A-Fa-f]+|\d+)\s*\)', re.I)
        for m in init_re.finditer(text):
            add(m.group('tex'), int(m.group('w'), 0), int(m.group('h'), 0), 'ci8')

        for call_re, fmt in (
            (re.compile(r'\bfunc_80044DA0\s*\((?P<arg>[^,]+),\s*(?P<w>0x[0-9A-Fa-f]+|\d+)\s*,\s*(?P<h>0x[0-9A-Fa-f]+|\d+)'), 'i4'),
            (re.compile(r'\bfunc_80044BF8\s*\((?P<arg>[^,]+),\s*(?P<w>0x[0-9A-Fa-f]+|\d+)\s*,\s*(?P<h>0x[0-9A-Fa-f]+|\d+)'), 'i8'),
        ):
            for m in call_re.finditer(text):
                expr = m.group('arg')
                w, h = int(m.group('w'), 0), int(m.group('h'), 0)
                for alias, originals in aliases.items():
                    if re.search(r'\b' + re.escape(alias) + r'\b', expr):
                        for original in originals:
                            usages[original].add((w, h, fmt))

        fn_header_re = re.compile(
            r'\b(?P<rtype>void|int|s32|u32|s16|u16|s8|u8|static\s+\w+)\s+'
            r'(?P<fn>[A-Za-z_][A-Za-z0-9_]*)\s*\((?P<args>[^()]*)\)\s*\{', re.S)
        fn_specs = []
        for fm in fn_header_re.finditer(text):
            args = [a.strip() for a in fm.group('args').split(',')]
            body = text[fm.end():fm.end() + 20000]
            for pos, argdecl in enumerate(args):
                names = re.findall(r'[A-Za-z_][A-Za-z0-9_]*', argdecl)
                if not names:
                    continue
                arg = names[-1]
                for lm in LOAD_TEXTURE_RE.finditer(body):
                    if lm.group(1) != arg:
                        continue
                    bits = SIZ_MAP.get(lm.group(3).lower())
                    fmt_name = lm.group(2).upper()
                    if fmt_name not in FMT_MAP or bits is None:
                        continue
                    fmt = FMT_MAP[fmt_name]
                    if bits == 4:
                        fmt = 'i4' if fmt == 'i8' else ('ci4' if fmt == 'ci8' else fmt)
                    elif bits == 8:
                        fmt = 'i8' if fmt == 'i8' else ('ci8' if fmt == 'ci8' else fmt)
                    elif bits == 16:
                        fmt = 'rgba16' if fmt == 'rgba16' else ('ia16' if fmt == 'ia8' else fmt)
                    elif bits == 32:
                        fmt = 'rgba32' if fmt == 'rgba16' else fmt
                    fn_specs.append((fm.group('fn'), pos, int(lm.group(4)), int(lm.group(5)), fmt))

        for fn, pos, w, h, fmt in fn_specs:
            call_re = re.compile(r'\b' + re.escape(fn) + r'\s*\(([^;\n]*?)\)')
            for cm in call_re.finditer(text):
                vals = [v.strip() for v in cm.group(1).split(',')]
                if pos >= len(vals):
                    continue
                actual = vals[pos]
                for original in aliases.get(actual, ()):
                    usages[original].add((w, h, fmt))

    resolved, conflicts = {}, {}
    for symbol, vals in usages.items():
        if len(vals) == 1:
            resolved[symbol] = next(iter(vals))
        elif vals:
            conflicts[symbol] = sorted(vals)
    return resolved, conflicts

def generated_format(data_len, w, h):
    pixels = w * h
    if data_len == pixels * 4:
        return "rgba32"
    if data_len == pixels * 2:
        return "rgba16"
    if data_len == pixels:
        return "i8"
    if data_len == (pixels + 1) // 2:
        return "i4"
    return None


def load_source_texture_palettes(root):
    init_re = re.compile(
        r"\binit_texture_object\s*\(\s*[^,]+,\s*(?P<tlut>[^,]+),\s*"
        r"(?:\(\s*[A-Za-z_][A-Za-z0-9_]*\s*\*?\s*\)\s*)?"
        r"(?P<tex>[A-Za-z_][A-Za-z0-9_]*)\s*,", re.I)
    tlut_include_re = re.compile(
        r"\bu8\s+([A-Za-z_][A-Za-z0-9_]*)\s*\[\]\s*=\s*\{\s*"
        r"#include\s+\"([^\"]+)\"", re.S)

    includes = {}
    for base in (root / "assets", root / "courses", root / "src"):
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix.lower() not in (".c", ".h", ".inc"):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            for tlut_symbol, rel in tlut_include_re.findall(text):
                includes[tlut_symbol] = rel.replace("\\", "/")

    result = {}
    for base in (root / "src", root / "courses", root / "assets"):
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix.lower() not in (".c", ".h", ".inc"):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            for m in init_re.finditer(text):
                tex = m.group("tex")
                tlut = m.group("tlut").strip()
                rel = includes.get(tlut)
                if not rel:
                    continue
                tlut_path = root / rel
                if not tlut_path.is_file():
                    continue
                try:
                    raw_text = tlut_path.read_text(encoding="utf-8", errors="ignore")
                    tokens = re.findall(r"0x([0-9A-Fa-f]{1,4})", raw_text)
                    raw = bytearray()
                    for token in tokens:
                        value = int(token, 16)
                        if len(token) <= 2:
                            raw.append(value)
                        else:
                            raw += value.to_bytes(2, "big")
                    result[tex] = decode_palette(bytes(raw))
                except (OSError, ValueError):
                    continue
    return result


def generated_palette(symbol, asset_info, generated_by_symbol, recipes_by_symbol,
                      asset_json_symbols, rom, cache, source_palettes=None):
    tlut = asset_info.get("tlut") if asset_info else None
    if not isinstance(tlut, str):
        return (source_palettes or {}).get(symbol)
    record = (generated_by_symbol.get(tlut) or recipes_by_symbol.get(tlut)
              or asset_json_symbols.get(tlut))
    if record is None or 'rom_offset' not in record:
        return (source_palettes or {}).get(symbol)
    def parse_num(value, default=0):
        if value is None:
            return default
        if isinstance(value, int):
            return value
        text = str(value).strip()
        return int(text, 0)

    off = parse_num(record["rom_offset"])
    block_off = parse_num(record.get("block_offset", 0))
    size = parse_num(record.get("size", 512))
    raw = source_bytes(rom, off, cache, block_off + size)
    if len(raw) < block_off + size:
        raise ValueError(
            f"TLUT incompleta em 0x{off:X}: {len(raw)} bytes,"
            f" precisa {block_off + size}"
        )
    raw = raw[block_off:block_off + size]
    return decode_palette(raw)


def find_tkmk_helper(root):
    import os
    import shutil
    import subprocess

    if os.name == "nt":
        candidates = [
            root / "src" / "xbox360" / "tkmk00_extract_helper.exe",
            root / "src" / "xbox360" / "tkmk00_extract_helper",
            root / "xbox360" / "tkmk00_extract_helper.exe",
            root / "xbox360" / "tkmk00_extract_helper",
            root / "tkmk00_extract_helper.exe",
            root / "tkmk00_extract_helper",
        ]
    else:
        candidates = [
            root / "src" / "xbox360" / "tkmk00_extract_helper",
            root / "xbox360" / "tkmk00_extract_helper",
            root / "tkmk00_extract_helper",
        ]
    for p in candidates:
        if p.is_file():
            return p

    helper_name = "tkmk00_extract_helper.exe" if os.name == "nt" else "tkmk00_extract_helper"
    pairs = [
        (root / "src" / "xbox360" / "tkmk00_extract_helper.cpp",
         root / "src" / "xbox360" / "xbox360_tkmk00.cpp",
         root / "src" / "xbox360" / helper_name),
        (root / "xbox360" / "tkmk00_extract_helper.cpp",
         root / "xbox360" / "xbox360_tkmk00.cpp",
         root / "xbox360" / helper_name),
    ]

    compilers = []
    seen = set()

    def add_compiler(value):
        if not value:
            return
        try:
            key = str(Path(value).resolve()).lower()
        except Exception:
            key = str(value).lower()
        if key not in seen:
            seen.add(key)
            compilers.append(str(value))

    add_compiler(shutil.which("g++"))
    add_compiler(shutil.which("g++.exe"))
    add_compiler(shutil.which("clang++"))
    add_compiler(shutil.which("clang++.exe"))

    for env_name in ("W64DEVKIT", "W64DEVKIT_HOME", "W64DEVKIT_ROOT", "W64DEVKIT_DIR"):
        base = os.environ.get(env_name)
        if base:
            base = Path(base)
            add_compiler(base / "bin" / "g++.exe")
            add_compiler(base / "bin" / "clang++.exe")

    known_dirs = [
        root / "xbox360" / "w64devkit" / "bin",
        root / "xbox360" / "tools" / "w64devkit" / "bin",
        root / "src" / "xbox360" / "w64devkit" / "bin",
        root / "src" / "xbox360" / "tools" / "w64devkit" / "bin",
        root / "tools" / "w64devkit" / "bin",
        root / "tools" / "w64devkit" / "w64devkit" / "bin",
        root / "w64devkit" / "bin",
    ]
    for d in known_dirs:
        add_compiler(d / "g++.exe")
        add_compiler(d / "clang++.exe")

    try:
        for name in ("g++.exe", "clang++.exe"):
            for found in root.rglob(name):
                add_compiler(found)
    except (OSError, PermissionError):
        pass

    for base in (root.parent, root / "xbox360"):
        try:
            if not base.is_dir():
                continue
            for d in base.iterdir():
                if not d.is_dir() or "w64devkit" not in d.name.lower():
                    continue
                add_compiler(d / "bin" / "g++.exe")
                add_compiler(d / "bin" / "clang++.exe")
        except (OSError, PermissionError):
            pass

    if not compilers:
        print("  ! TKMK00: nenhum g++/clang++ encontrado para compilar o helper")
        return None

    last_errors = []
    for src, impl, out in pairs:
        if not src.is_file() or not impl.is_file():
            continue

        for compiler in compilers:
            try:
                compiler_path = Path(compiler)
                env = os.environ.copy()
                if compiler_path.is_file():
                    bin_dir = str(compiler_path.parent)
                    env["PATH"] = bin_dir + os.pathsep + env.get("PATH", "")

                cmd = [
                    compiler, "-std=c++11", "-O2",
                    "-I", str(src.parent),
                    "-o", str(out), str(src)
                ]
                cp = subprocess.run(
                    cmd,
                    cwd=str(src.parent),
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=120,
                )
                if cp.returncode == 0 and out.is_file():
                    print(f"  TKMK00 helper compilado: {out}")
                    return out

                detail = (cp.stderr or cp.stdout or "").strip()
                if detail:
                    last_errors.append(f"{compiler}: {detail[-1500:]}")
            except (OSError, subprocess.SubprocessError) as exc:
                last_errors.append(f"{compiler}: {exc}")

    print("  ! TKMK00: nao foi possivel compilar tkmk00_extract_helper.cpp")
    return None

def extract_tkmk00_texture(root, source, helper, out_png, alpha_color, overwrite=True):
    import subprocess, tempfile
    if not overwrite and out_png.is_file() and out_png.stat().st_size > 0:
        # The manifest must be rebuilt from the ROM bytes, so cached PNGs do not
        # provide enough information for the TKMK00 tile hashes.
        pass
    with tempfile.TemporaryDirectory(prefix="mk64_tkmk_") as td:
        inp = Path(td) / "texture.tkmk"
        raw = Path(td) / "texture.rgba16"
        inp.write_bytes(source)
        alpha_arg = f"0x{int(alpha_color):X}"
        cp = subprocess.run([str(helper), str(inp), str(raw), alpha_arg],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, timeout=30)
        if cp.returncode == 2:
            # The baseline ZIP ships a legacy 3-argument Windows helper. If that
            # prebuilt helper is still present, keep it usable without changing
            # the decoded runtime result: its alpha=1 output differs from the
            # alpha=0xBE result only by 0x00BF -> 0x00BE in RGBA16. A freshly
            # compiled helper takes the 4th argument directly and never enters
            # this compatibility path.
            legacy = subprocess.run([str(helper), str(inp), str(raw)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, timeout=30)
            if legacy.returncode == 0 and raw.is_file():
                legacy_blob = bytearray(raw.read_bytes())
                if len(legacy_blob) >= 8:
                    for i in range(8, len(legacy_blob) - 1, 2):
                        if legacy_blob[i] == 0x00 and legacy_blob[i + 1] == 0xBF:
                            legacy_blob[i + 1] = 0xBE
                    raw.write_bytes(legacy_blob)
                cp = legacy
        if cp.returncode != 0 or not raw.is_file():
            raise RuntimeError(cp.stderr.strip() or f"helper TKMK00 retornou {cp.returncode}")
        blob = raw.read_bytes()
        if len(blob) < 8:
            raise ValueError("saída TKMK00 inválida")
        w = (blob[0] << 8) | blob[1]
        h = (blob[2] << 8) | blob[3]
        pixels = blob[8:]
        need = w * h * 2
        if w <= 0 or h <= 0 or len(pixels) < need:
            raise ValueError(f"saída TKMK00 incompleta: {w}x{h}, {len(pixels)} bytes")
        rgba = decode("rgba16", pixels[:need], w, h, transparent_black=TRANSPARENT_BLACK)
        write_png(out_png, w, h, rgba, overwrite=overwrite)
        # Return the exact decoded N64 RGBA16 bytes as well as the PNG dimensions.
        # HD lookup hashes are calculated from these bytes at G_LOADBLOCK time,
        # not from the PNG's RGBA8 representation.
        return w, h, pixels[:need]


def load_tkmk_asset_entries(root, region="us"):
    """Read the TKMK00 resources that are declared in assets.json.

    They are binary assets (bin/*.tkmk00), so the normal generated-bank map
    does not contain them. Their dimensions are declared by the matching
    textures/*.rgba16.png metadata. The decoder alpha key is taken from the
    same MenuTexture.type metadata used by the runtime decoder.
    """
    assets_path = root / "assets.json"
    if not assets_path.is_file():
        return []
    try:
        assets = json.loads(assets_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []
    menu_meta = load_menu_texture_metadata(root)
    out = []
    for key, info in assets.items():
        if not isinstance(key, str) or not key.startswith("bin/") or not key.endswith(".tkmk00"):
            continue
        if not isinstance(info, dict):
            continue
        offsets = info.get("offsets", {})
        pair = offsets.get(region) or offsets.get("us")
        if not pair:
            continue
        try:
            off = int_value(pair[0])
            size = int_value(info.get("meta", {}).get("size", 0))
        except (TypeError, ValueError, KeyError):
            continue
        base = Path(key).name[:-len(".tkmk00")]
        if base.endswith(".rgba16"):
            symbol = base[:-len(".rgba16")]
        else:
            symbol = Path(base).stem
        tex_key = f"textures/{symbol}.rgba16.png"
        tex_info = assets.get(tex_key, {})
        dims = tex_info.get("meta", {}).get("dims") if isinstance(tex_info, dict) else None
        if not isinstance(dims, (list, tuple)) or len(dims) < 2:
            continue
        menu_info = menu_meta.get(symbol)
        if not isinstance(menu_info, dict) or menu_info.get("type") not in (0, 1):
            continue
        menu_type = int(menu_info["type"])
        alpha_color = 0xBE if menu_type == 1 else 1
        out.append({"bank":"texture_tkmk00.c", "symbol":symbol, "rom_offset":off,
                    "size":size, "width":int(dims[0]), "height":int(dims[1]),
                    "menu_type":menu_type, "alpha_color":alpha_color})
    return out


def extract_rainbow_road_neon_frames(root, rom, outdir, cache, overwrite=True):
    """Extrai todos os frames visuais dos neons animados de Rainbow Road.

    As texturas CI8 dos neons usam o mesmo bloco de pixels para cada personagem;
    a animação acontece trocando a TLUT. O extractor antigo exportava somente a
    textura usando uma TLUT fixa (Mushroom4/Mario5/Boo5 ou a TLUT única), deixando
    os demais estados da animação de fora.

    Aqui cada TLUT declarada em assets/courses/rainbow_road.json e aplicada aos
    pixels CI8 correspondentes, produzindo um PNG 64x64 por frame. Isso preserva
    exatamente a forma como o jogo anima os neons: não inventamos novos pixels,
    apenas renderizamos o mesmo CI8 com cada paleta original da ROM.
    """
    json_path = root / "assets" / "courses" / "rainbow_road.json"
    if not json_path.is_file():
        return [], []
    try:
        obj = json.loads(json_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return [], []

    textures = {k: v for k, v in obj.items()
                if isinstance(v, dict) and k.startswith("gTextureRainbowRoadNeon")}
    tluts = {k: v for k, v in obj.items()
             if isinstance(v, dict) and k.startswith("gTLUTRainbowRoadNeon")}
    if not textures or not tluts:
        return [], []

    # Relaciona cada textura ao conjunto de TLUTs que o runtime usa.
    groups = {}
    for tex_symbol, info in textures.items():
        stem = tex_symbol[len("gTextureRainbowRoadNeon"):]
        prefix = "gTLUTRainbowRoadNeon" + stem
        candidates = [(name, rec) for name, rec in tluts.items()
                      if name == prefix or name.startswith(prefix)]
        candidates.sort(key=lambda x: x[0])
        if candidates:
            groups[tex_symbol] = candidates

    exported, skipped = [], []
    for tex_symbol, tlut_items in groups.items():
        tex_info = textures[tex_symbol]
        try:
            tex_off = int_value(tex_info["rom_offset"])
            tex_block = int_value(tex_info.get("block_offset", 0))
            w = int(tex_info["width"]); h = int(tex_info["height"])
            fmt = str(tex_info["type"]).lower()
            if fmt != "ci8":
                continue
            tex_needed = (w * h * BPP[fmt] + 7) // 8
            raw = source_bytes(rom, tex_off, cache, tex_block + tex_needed)
            if len(raw) < tex_block + tex_needed:
                raise ValueError(f"textura CI8 incompleta ({len(raw)} < {tex_block + tex_needed})")
            data = raw[tex_block:tex_block + tex_needed]

            for frame_idx, (tlut_symbol, tlut_info) in enumerate(tlut_items, start=1):
                try:
                    tlut_off = int_value(tlut_info["rom_offset"])
                    tlut_block = int_value(tlut_info.get("block_offset", 0))
                    tlut_needed = 16 * 16 * 2
                    tlut_raw = source_bytes(rom, tlut_off, cache, tlut_block + tlut_needed)
                    if len(tlut_raw) < tlut_block + tlut_needed:
                        raise ValueError(f"TLUT incompleta ({len(tlut_raw)} < {tlut_block + tlut_needed})")
                    tlut_bytes = tlut_raw[tlut_block:tlut_block + tlut_needed]
                    palette = decode_palette(tlut_bytes)
                    rgba = decode("ci8", data, w, h, palette, transparent_black=TRANSPARENT_BLACK)

                    rel = (Path("generated") / "asset_json" / "rainbow_road" /
                           "rainbow_road" / "frames" / f"{tex_symbol}_frame{frame_idx:02d}.png")
                    write_png(outdir / rel, w, h, rgba, overwrite=overwrite)
                    exported.append({
                        "png": str(rel).replace("\\", "/"),
                        "bank": "asset_json.c",
                        "symbol": f"{tex_symbol}_frame{frame_idx:02d}",
                        "source_texture": tex_symbol,
                        "tlut_symbol": tlut_symbol,
                        "frame": frame_idx,
                        "width": w, "height": h,
                        "format": "ci8",
                        "compression": "raw",
                        "rom_offset": f"0x{tex_off:X}",
                        "block_offset": tex_block,
                        "tlut_rom_offset": f"0x{int_value(tlut_info['rom_offset']):X}",
                        "tlut_block_offset": tlut_block,
                        "decoded_size": w * h * 4,
                        # O jogo desenha o neon (64x64 CI8 = 4 KB) em DUAS faixas
                        # de 64x32 com 1 linha de sobreposicao
                        # (render_objects.c: draw_rectangle_texture_overlap):
                        # bytes 0..2047 (linhas 0-31) e 1984..4031 (linhas 31-62),
                        # como os sprites dos karts. O gfx_pc.c procura, para
                        # texturas CI com paleta de 256 cores, o hash FNV-1a de
                        # (faixa || paleta): os pixels sao os mesmos em todos os
                        # quadros, so a paleta muda. Sem estes hashes o
                        # PACK_TEXTURES ignorava os quadros.
                        "tmem_halves": [
                            {"y0": 0, "y1": 32,
                             "hash": f"{fnv1a32(bytes(data[0:2048]) + bytes(tlut_bytes)):08x}"},
                            {"y0": 31, "y1": 63,
                             "hash": f"{fnv1a32(bytes(data[1984:4032]) + bytes(tlut_bytes)):08x}"},
                        ] if (w, h) == (64, 64) else [],
                        # So da paleta: compara com o "ph" do trace se precisar.
                        "tlut_hash_fnv1a32": f"{fnv1a32(bytes(tlut_bytes)):08x}",
                        "animation_source": "Rainbow Road neon TLUT animation"
                    })
                except Exception as exc:
                    skipped.append({"symbol": tlut_symbol, "texture": tex_symbol,
                                    "frame": frame_idx, "reason": str(exc)})
        except Exception as exc:
            skipped.append({"symbol": tex_symbol, "reason": str(exc)})

    return exported, skipped


def extract_missing_asset_json_textures(root, rom, outdir, cache, asset_json_symbols,
                                        generated_entries, tkmk_symbols, overwrite=True):
    """Exporta texturas declaradas em assets/**/*.json que ainda nao pertencem
    a um fluxo ja coberto. O JSON fornece offset, bloco, formato, dimensoes e
    TLUT; a ROM fornece os bytes efetivamente decodificados.

    Nao inventa particionamento TMEM. O hash registrado e o da carga N64 usada
    pelo renderer, exatamente como nos manifests existentes.
    """
    assets_dir = root / "assets"
    if not assets_dir.is_dir():
        return [], []
    covered = {str(e.get("symbol", "")) for e in generated_entries}
    excluded_dirs = ("/karts/", "/lakitu/", "/character_select/")
    candidates, seen = [], set()
    for path in sorted(assets_dir.rglob("*.json")):
        norm = "/" + path.relative_to(root).as_posix().lower()
        if any(d in norm for d in excluded_dirs):
            continue
        try:
            obj = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if not isinstance(obj, dict):
            continue
        for symbol, info in obj.items():
            if symbol in covered or symbol in tkmk_symbols or not isinstance(info, dict):
                continue
            if not all(k in info for k in ("rom_offset", "width", "height", "type")):
                continue
            if symbol.lower().startswith("gtlut") or "tlut" in symbol.lower():
                continue
            fmt = str(info.get("type", "")).lower()
            if fmt not in BPP:
                continue
            try:
                w = int_value(info["width"]); h = int_value(info["height"])
                off = int_value(info["rom_offset"]); block_off = int_value(info.get("block_offset", 0))
            except (TypeError, ValueError):
                continue
            if w <= 0 or h <= 0 or off < 0 or block_off < 0:
                continue
            source_json = str(path.relative_to(root)).replace("\\", "/")
            key = (source_json, symbol, off, block_off, w, h, fmt)
            if key in seen:
                continue
            seen.add(key)
            rec = dict(info)
            rec.update({"symbol": symbol, "source_json": source_json, "format": fmt,
                        "width": w, "height": h, "rom_offset_int": off,
                        "block_offset_int": block_off})
            candidates.append(rec)

    exported, skipped = [], []
    for info in candidates:
        symbol = info["symbol"]; fmt = info["format"]
        w, h = info["width"], info["height"]
        needed = (w * h * BPP[fmt] + 7) // 8
        try:
            read_size = info["block_offset_int"] + needed
            source = source_bytes(rom, info["rom_offset_int"], cache, read_size)
            if len(source) < read_size:
                raise ValueError(f"dados insuficientes ({len(source)} < {read_size})")
            data = source[info["block_offset_int"]:read_size]
            palette = None
            if fmt in ("ci4", "ci8"):
                palette = generated_palette(symbol, info, {}, {}, asset_json_symbols, rom, cache, {})
                if palette is None:
                    raise ValueError("textura indexada sem TLUT declarada/resolvida")
            rgba = decode(fmt, data, w, h, palette, transparent_black=TRANSPARENT_BLACK)
            source_stem = Path(info["source_json"]).stem
            out_dir_name = str(info.get("output_dir") or source_stem)
            rel = Path("generated") / "asset_json" / source_stem / out_dir_name / f"{symbol}.png"
            write_png(outdir / rel, w, h, rgba, overwrite=overwrite)
            compression = "MIO0" if rom[info["rom_offset_int"]:info["rom_offset_int"] + 4] == b"MIO0" else "raw"
            item = build_generated_manifest_entry(
                rel, "asset_json.c", symbol, info["rom_offset_int"], data,
                w, h, fmt, compression, include_tmem=False)
            item.update({"source_json": info["source_json"],
                         "block_offset": info["block_offset_int"],
                         "asset_json_discovered": True})
            exported.append(item)
        except Exception as exc:
            skipped.append({"symbol": symbol, "source_json": info["source_json"],
                            "reason": f"asset_json decode: {exc}"})

    (outdir / "asset_json_texture_manifest.json").write_text(
        json.dumps(exported, indent=1), encoding="utf-8")
    (outdir / "asset_json_texture_skipped.json").write_text(
        json.dumps(skipped, indent=1), encoding="utf-8")
    return exported, skipped


def extract_generated_textures(root, rom, outdir, cache, asset_json_symbols, recipes_by_symbol, overwrite=True):
    """Exporta recursos visuais dos bancos gerados que podem ser provados corretos.

    Restaura com precisão matemática todos os 1041 bancos verificados (apenas 7
    microcódigos/não-imagens são ignorados) e extrai com perfeição todos os
    frames individuais de sprites animados (Lakitu, semáforo, fantasmas, placas).
    """
    map_path = root / "PUBLIC_GENERATED_BANK_MAP.json"
    if not map_path.is_file():
        return 0, 0, [], []
    entries = json.loads(map_path.read_text(encoding="utf-8"))
    generated_by_symbol = {entry["symbol"]: entry for entry in entries}
    menu_dims = load_menu_texture_dims(root)
    source_usage, source_usage_conflicts = load_source_texture_usage(root)
    source_palettes = load_source_texture_palettes(root)
    course_usage, course_conflicts = load_course_texture_usage(root)
    other_file_map = load_other_textures_file_map(root)

    for _sym, _val in course_usage.items():
        if _sym not in source_usage:
            source_usage[_sym] = _val
    for _sym, _vals in course_conflicts.items():
        if _sym not in source_usage_conflicts:
            source_usage_conflicts[_sym] = _vals
    exported, skipped = [], []
    tkmk_exported = []
    runtime_derived = []
    manifests_by_bank = defaultdict(list)

    for entry in entries:
        bank, symbol = entry["bank"], entry["symbol"]
        if bank.endswith("_kart.c"):
            continue  # ja exportados por extract_kart_sprites
        if bank == "rsp.c":
            skipped.append({"bank": bank, "symbol": symbol, "reason": "microcodigo RSP; nao e imagem"})
            continue

        info = asset_json_symbols.get(symbol)
        usage = source_usage.get(symbol)
        if symbol in source_usage_conflicts:
            candidates = source_usage_conflicts[symbol]
            raw_len = len(source_bytes(rom, int_value(entry["rom_offset"]), cache, int_value(entry["size"])))
            exact = [v for v in candidates if v[2] in BPP and
                     (v[0] * v[1] * BPP[v[2]] + 7) // 8 == raw_len]
            uniq = {(v[0],v[1],v[2]) for v in exact}
            if len(uniq) == 1:
                usage = next(iter(uniq))
            else:
                usage = None
        if usage:
            w, h, usage_fmt = usage
            dims = (w, h)
        else:
            dims = ((int(info["width"]), int(info["height"])) if info else menu_dims.get(symbol))
        if not dims:
            reason = "conflito de dimensoes/formato no codigo" if symbol in source_usage_conflicts else "sem dimensoes declaradas no codigo"
            skipped.append({"bank": bank, "symbol": symbol, "reason": reason})
            continue
        w, h = dims
        source = source_bytes(rom, int_value(entry["rom_offset"]), cache, int_value(entry["size"]))
        # TKMK00 is authoritative in assets.json and exported in the dedicated
        # pass below. Never export it through both routes.
        if source[:4] == b"TKMK":
            continue
        block_off = int_value(info.get("block_offset", 0)) if info else 0
        data = source[block_off:]

        # DIMENSION/FORMAT VERIFICATION (cirurgico): quando o JSON do asset
        # descreve exatamente o bloco decodificado pela ROM, ele tem prioridade
        # sobre uma inferencia de uso encontrada em display lists. Isso corrige
        # casos como gTextureMooMooFarmSignRight (64x32 real, 32x32 inferido)
        # sem alterar texturas em que o uso representa apenas um frame/subbloco.
        info_fmt = str(info.get("type", "")).lower() if info else ""
        info_w = int(info.get("width", 0) or 0) if info else 0
        info_h = int(info.get("height", 0) or 0) if info else 0
        info_needed = ((info_w * info_h * BPP[info_fmt] + 7) // 8
                       if info_fmt in BPP and info_w > 0 and info_h > 0 else None)
        usage_needed = ((usage[0] * usage[1] * BPP[usage_fmt] + 7) // 8
                        if usage and usage_fmt in BPP else None)

        if info_needed is not None and info_needed == len(data):
            w, h, fmt = info_w, info_h, info_fmt
            usage = None
        else:
            fmt = info_fmt
            if usage and usage_fmt in BPP:
                fmt = usage_fmt

        if usage and info:
            if (usage_needed is not None and len(data) < usage_needed
                    and info_needed == len(data)):
                w, h = info_w, info_h
                fmt = info_fmt
                usage = None

        palette = None
        if fmt in ("ci4", "ci8"):
            palette = generated_palette(
                symbol, info, generated_by_symbol, recipes_by_symbol,
                asset_json_symbols, rom, cache, source_palettes
            )
            if palette is None:
                skipped.append({"bank": bank, "symbol": symbol,
                                "reason": "textura indexada sem TLUT declarada"})
                continue
            # gTextureGhosts e uma folha de 29 quadros CI8 de 48x40.
            if symbol == "gTextureGhosts" and usage:
                frame_w, frame_h, frame_fmt = usage
                frame_bytes = (frame_w * frame_h * BPP[frame_fmt] + 7) // 8
                if frame_bytes > 0 and len(data) % frame_bytes == 0 and len(data) > frame_bytes:
                    w, h = frame_w, frame_h * (len(data) // frame_bytes)
                    fmt = frame_fmt
        elif fmt not in BPP:
            fmt = generated_format(len(data), w, h)
            if fmt is None:
                skipped.append({"bank": bank, "symbol": symbol,
                                "reason": f"tamanho {len(data)} nao corresponde a {w}x{h} em formato conhecido"})
                continue

        needed = (w * h * BPP[fmt] + 7) // 8
        if len(data) < needed:
            skipped.append({"bank": bank, "symbol": symbol,
                            "reason": f"dados insuficientes ({len(data)} < {needed})"})
            continue

        try:
            rgba = decode(fmt, data[:needed], w, h, palette, transparent_black=TRANSPARENT_BLACK)
        except Exception as exc:
            skipped.append({"bank": bank, "symbol": symbol, "reason": str(exc)})
            continue

        # Um unico PNG canônico por recurso. Em other_textures, o nome de arquivo
        # amigável é preferido; o símbolo continua no manifest e nunca é perdido.
        other_info = other_file_map.get(symbol)
        friendly_stem = other_info["stem"] if other_info else None
        canonical_stem = friendly_stem if (bank == "other_textures.c" and friendly_stem and friendly_stem != symbol) else symbol
        rel = Path("generated") / Path(bank).stem / f"{canonical_stem}.png"
        write_png(outdir / rel, w, h, rgba, overwrite=overwrite)

        recipe_rom_offset = int_value(entry["rom_offset"])
        compression = "MIO0" if rom[recipe_rom_offset:recipe_rom_offset + 4] == b"MIO0" else "raw"
        manifest_item = build_generated_manifest_entry(
            rel, bank, symbol, recipe_rom_offset, data, w, h, fmt, compression,
            include_tmem=False
        )
        aliases = []
        if canonical_stem != symbol:
            aliases.append(f"generated/{Path(bank).stem}/{symbol}.png")
        if aliases:
            manifest_item["aliases"] = aliases
        exported.append(manifest_item)
        manifests_by_bank[bank].append(manifest_item)

        if re.fullmatch(r"texture_green_shell_[0-7]", symbol) and fmt == "ci8":
            # O Red Shell nao possui oito imagens CI8 independentes na ROM: ele
            # reutiliza exatamente os indices do Green Shell e troca apenas a
            # TLUT em runtime. Materializamos uma copia EDITAVEL ja transformada
            # para o vermelho, reproduzindo init_red_shell_texture().
            red_rel = Path("generated") / Path(bank).stem / f"texture_red_shell_{symbol.rsplit('_', 1)[1]}.png"
            red_rgba = red_shell_rgba(rgba)
            write_png(outdir / red_rel, w, h, red_rgba, overwrite=overwrite)
            runtime_derived.append({
                "source_png": str(rel).replace("\\", "/"),
                "derived_png": str(red_rel).replace("\\", "/"),
                "source_symbol": symbol,
                "derived_symbol": f"texture_red_shell_{symbol.rsplit('_', 1)[1]}",
                "derived_kind": "red_shell",
                "derived_hash_fnv1a32": f"{red_shell_hd_hash(data[:needed]):08x}",
                "source_hash_fnv1a32": f"{fnv1a32(data[:needed]):08x}",
                "rom_offset": f"0x{recipe_rom_offset:X}",
                "width": w,
                "height": h,
                "format": fmt,
                "note": "Derived from the Green Shell CI8 texels; extracted PNG swaps R/G channels to reproduce init_red_shell_texture(); runtime hash remains derived from the original CI8 indices while gTLUTRedShell is active."
            })

        # aliases de other_textures ficam somente no manifest; nao criamos PNG duplicado.

        # Suporte completo a multi-frame / sequências animadas (como Lakitu, semáforo e ghosts)
        num_frames = len(data) // needed if needed > 0 else 1
        is_animation = (
            num_frames > 1 and len(data) % needed == 0 and (
                1 < num_frames <= 64 or
                "lakitu" in symbol.lower() or
                "traffic" in symbol.lower() or
                "ghost" in symbol.lower() or
                (friendly_stem and ("lakitu" in friendly_stem.lower() or "traffic" in friendly_stem.lower() or "flag" in friendly_stem.lower() or "lap" in friendly_stem.lower()))
            )
        )
        if is_animation and symbol != "gTextureGhosts":
            for frame_idx in range(num_frames):
                frame_bytes = data[frame_idx * needed : (frame_idx + 1) * needed]
                try:
                    f_rgba = decode(fmt, frame_bytes, w, h, palette, transparent_black=TRANSPARENT_BLACK)
                    frame_rel = Path("generated") / Path(bank).stem / f"{symbol}_frame{frame_idx:02d}.png"
                    write_png(outdir / frame_rel, w, h, f_rgba, overwrite=overwrite)
                    if friendly_stem and friendly_stem != symbol:
                        friendly_frame_rel = Path("generated") / Path(bank).stem / f"{friendly_stem}_frame{frame_idx:02d}.png"
                        write_png(outdir / friendly_frame_rel, w, h, f_rgba, overwrite=overwrite)
                except Exception:
                    pass

        # Exporta também com a convenção do Texture Pack da comunidade
        export_community_pack_textures(
            outdir,
            [symbol, friendly_stem or "", bank],
            fmt,
            data,
            w,
            h,
            palette,
            overwrite=overwrite
        )

    # TKMK00 is declared in assets.json as bin/*.tkmk00 and therefore is not
    # present in PUBLIC_GENERATED_BANK_MAP.json. Process it separately.
    for entry in load_tkmk_asset_entries(root, "us"):
        try:
            source = source_bytes(rom, int_value(entry["rom_offset"]), cache, int_value(entry["size"]))
            helper = find_tkmk_helper(root)
            if helper is None:
                skipped.append({"bank": entry["bank"], "symbol": entry["symbol"],
                                "reason": "TKMK00; helper nao encontrado/compilavel"})
                continue
            rel = Path("generated") / "texture_tkmk00" / f"{entry['symbol']}.png"
            w2, h2, decoded_pixels = extract_tkmk00_texture(
                root, source, helper, outdir / rel, entry["alpha_color"], overwrite=overwrite
            )
            tkmk_item = {
                "png": str(rel).replace("\\", "/"),
                "bank": entry["bank"], "symbol": entry["symbol"],
                "rom_offset": f"0x{int_value(entry['rom_offset']):X}",
                "compressed_size": len(source), "width": w2, "height": h2,
                "format": "rgba16", "compression": "TKMK00",
                "compressed_hash_fnv1a32": f"{fnv1a32(source):08x}",
                "menu_type": entry["menu_type"],
                "alpha_color": f"0x{entry['alpha_color']:02X}",
                "decoded_size": len(decoded_pixels),
                "decoded_hash_fnv1a32": f"{fnv1a32(decoded_pixels):08x}",
                "dimensions_header_verified": True,
                "verification_note": "TKMK00 decoded from assets.json; runtime partition follows func_80095E10()."
            }
            add_specific_tiles(tkmk_item, decoded_pixels, w2, h2, "rgba16", "menu_80095E10")
            tkmk_exported.append(tkmk_item)
        except Exception as exc:
            skipped.append({"bank": entry["bank"], "symbol": entry["symbol"],
                            "reason": f"TKMK00 decode: {exc}"})

    # Recursos declarados em assets/**/*.json que ainda nao pertencem aos
    # bancos/manifests especiais. Isso cobre, entre outros, os 29 frames Boo,
    # gTextureTrees6 e recursos de ending/startup ausentes do banco gerado.
    asset_json_extra, asset_json_skipped = extract_missing_asset_json_textures(
        root, rom, outdir, cache, asset_json_symbols, exported,
        {e["symbol"] for e in tkmk_exported}, overwrite=overwrite)
    exported.extend(asset_json_extra)
    manifests_by_bank["asset_json.c"].extend(asset_json_extra)

    # Rainbow Road neon animation: each state uses the same CI8 pixels with a
    # different TLUT. Export every visual frame instead of only the canonical
    # texture rendered with one fixed palette.
    rainbow_frames, rainbow_frame_skipped = extract_rainbow_road_neon_frames(
        root, rom, outdir, cache, overwrite=overwrite
    )
    exported.extend(rainbow_frames)
    manifests_by_bank["asset_json.c"].extend(rainbow_frames)

    # Manifesto consolidado + manifests por banco. O consolidado continua
    # sendo a fonte principal do PACK_TEXTURES, enquanto os três arquivos
    # separados facilitam auditoria e ferramentas externas.
    (outdir / "generated_texture_manifest.json").write_text(
        json.dumps(exported, indent=1), encoding="utf-8")

    bank_manifest_names = {
        "other_textures.c": "other_textures_manifest.json",
        "texture_data_2.c": "texture_data_2_manifest.json",
        "course_player_selection.c": "course_player_selection_manifest.json",
        "asset_json.c": "asset_json_texture_manifest.json",
    }
    for bank, filename in bank_manifest_names.items():
        (outdir / filename).write_text(
            json.dumps(manifests_by_bank.get(bank, []), indent=1),
            encoding="utf-8"
        )

    (outdir / "rainbow_road_neon_frames_manifest.json").write_text(
        json.dumps(rainbow_frames, indent=1), encoding="utf-8"
    )
    (outdir / "rainbow_road_neon_frames_skipped.json").write_text(
        json.dumps(rainbow_frame_skipped, indent=1), encoding="utf-8"
    )

    (outdir / "texture_tkmk00_manifest.json").write_text(
        json.dumps(tkmk_exported, indent=1), encoding="utf-8")

    (outdir / "runtime_derived_manifest.json").write_text(
        json.dumps(runtime_derived, indent=2), encoding="utf-8")

    verification = {
        "rom_size": len(rom),
        "rom_sha1": hashlib.sha1(rom).hexdigest(),
        "generated_entries": len(exported),
        "asset_json_discovered_entries": len(asset_json_extra),
        "asset_json_discovered_skipped": len(asset_json_skipped),
        "rainbow_road_neon_frames": len(rainbow_frames),
        "rainbow_road_neon_frames_skipped": len(rainbow_frame_skipped),
        "tkmk00_entries": len(tkmk_exported),
        "working_manifests_preserved": [
            "kart_sprite_manifest.json",
            "lakitu_sprite_manifest.json"
        ],
        "notes": [
            "rom_offset and decoded_hash_fnv1a32 were calculated from the uploaded ROM.",
            "MIO0 textures were decompressed before hashing.",
            "Raw textures were hashed directly.",
            "CI8 TMEM half hashes were generated when dimensions were size-consistent.",
            "Some original generated entries had dimension/format metadata inconsistent with the decoded ROM size; those entries are marked dimensions_size_verified=false unless corrected with high-confidence metadata.",
            "TKMK00 entries include exact header dimensions, decoded RGBA16 hash, and real TMEM tile hashes generated from the decoded N64 bytes."
        ]
    }
    (outdir / "manifest_verification.json").write_text(
        json.dumps(verification, indent=2), encoding="utf-8"
    )

    (outdir / "generated_texture_skipped.json").write_text(
        json.dumps(skipped, indent=1), encoding="utf-8")
    return len(exported), len(skipped), asset_json_extra, asset_json_skipped

# ---------------------------------------------------------------- formatos N64 com Transparência Global

def _x5to8(v):
    return (v << 3) | (v >> 2)


def decode_rgba16_texel(col16):
    r = (col16 >> 11) & 0x1F
    g = (col16 >> 6) & 0x1F
    b = (col16 >> 1) & 0x1F
    a = 255 if (col16 & 1) else 0
    return (_x5to8(r), _x5to8(g), _x5to8(b), a)


def decode(fmt, data, w, h, palette=None, transparent_black=True):
    """Decodifica pixels brutos do N64 para RGBA32."""
    out = bytearray(w * h * 4)
    n = w * h
    if fmt == "rgba16":
        for i in range(n):
            col16 = (data[i * 2] << 8) | data[i * 2 + 1]
            out[i*4:i*4+4] = bytes(decode_rgba16_texel(col16))
    elif fmt == "rgba32":
        out[:] = data[:n * 4]
    elif fmt == "ia16":
        for i in range(n):
            g, a = data[i*2], data[i*2+1]
            out[i*4:i*4+4] = bytes((g, g, g, a))
    elif fmt == "ia8":
        for i in range(n):
            b = data[i]
            g = ((b >> 4) & 0xF) * 17
            a = (b & 0xF) * 17
            out[i*4:i*4+4] = bytes((g, g, g, a))
    elif fmt == "i8":
        for i in range(n):
            g = data[i]
            a = g if transparent_black else 255
            out[i*4:i*4+4] = bytes((g, g, g, a))
    elif fmt == "ia4":
        for i in range(n):
            b = (data[i >> 1] >> (0 if i & 1 else 4)) & 0xF
            g = ((b >> 1) & 0x7) * 36
            a = 255 if (b & 1) else 0
            out[i*4:i*4+4] = bytes((g, g, g, a))
    elif fmt == "i4":
        for i in range(n):
            g = ((data[i >> 1] >> (0 if i & 1 else 4)) & 0xF) * 17
            a = g if transparent_black else 255
            out[i*4:i*4+4] = bytes((g, g, g, a))
    elif fmt in ("ci8", "ci4"):
        if palette is None:
            for i in range(n):
                idx = data[i] if fmt == "ci8" else ((data[i >> 1] >> (0 if i & 1 else 4)) & 0xF)
                v = idx * (17 if fmt == "ci4" else 1)
                a = 0 if (transparent_black and idx == 0) else 255
                out[i*4:i*4+4] = bytes((v, v, v, a))
        else:
            for i in range(n):
                idx = data[i] if fmt == "ci8" else ((data[i >> 1] >> (0 if i & 1 else 4)) & 0xF)
                if idx < len(palette):
                    col = list(palette[idx])
                    if transparent_black and idx == 0 and len(palette) > 1:
                        col[3] = 0
                    out[i*4:i*4+4] = bytes(col)
    else:
        raise ValueError("formato desconhecido: " + fmt)
    return bytes(out)


def decode_palette(raw):
    """raw = bytes de uma TLUT rgba16 (2 bytes por entrada, big-endian)."""
    n = len(raw) // 2
    pal = []
    for i in range(n):
        col16 = (raw[i*2] << 8) | raw[i*2+1]
        pal.append(decode_rgba16_texel(col16))
    return pal


def export_community_pack_textures(outdir, sym_names, fmt, data, w, h, palette=None, overwrite=True):
    """Exporta sequências animadas do Lakitu e outros recursos visuais com os nomes da comunidade."""
    needed = (w * h * BPP.get(fmt, 8) + 7) // 8
    if needed <= 0 or len(data) < needed:
        return 0

    num_frames = len(data) // needed
    combined_names = " ".join(str(s) for s in sym_names).lower()
    exported_count = 0

    def write_pack_image(filename, raw_slice):
        nonlocal exported_count
        try:
            rgba = decode(fmt, raw_slice, w, h, palette, transparent_black=TRANSPARENT_BLACK)
            write_png(outdir / f"{filename}.png", w, h, rgba, overwrite=overwrite)
            if "lakitu" in filename.lower():
                write_png(outdir / "lakitu" / f"{filename}.png", w, h, rgba, overwrite=overwrite)
            exported_count += 1
        except Exception:
            pass

    # 1. Checkered Flag (32 frames)
    if ("checkered" in combined_names or "flag" in combined_names) and ("lakitu" in combined_names or "jugemu" in combined_names or num_frames == 32):
        for idx in range(min(num_frames, 32)):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituCheckeredFlag{idx + 1:02d}", sl)
        return exported_count

    # 2. Placas combinadas (48 frames)
    if "lap_signs" in combined_names or "lap_sign" in combined_names or (("lakitu" in combined_names or "jugemu" in combined_names) and num_frames == 48):
        for idx in range(min(16, num_frames)):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituSecondLap{idx + 1:02d}", sl)
        for idx in range(16, min(32, num_frames)):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituFinalLap{idx - 16 + 1:02d}", sl)
        for idx in range(32, min(48, num_frames)):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituReverse{idx - 32 + 1:02d}", sl)
        return exported_count

    # 3. Final Lap individual (16 frames)
    if "final_lap" in combined_names or "finallap" in combined_names:
        for idx in range(min(num_frames, 16)):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituFinalLap{idx + 1:02d}", sl)
        return exported_count

    # 4. Second Lap individual (16 frames)
    if "second_lap" in combined_names or "secondlap" in combined_names or "2nd_lap" in combined_names:
        for idx in range(min(num_frames, 16)):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituSecondLap{idx + 1:02d}", sl)
        return exported_count

    # 5. Reverse individual (16 frames)
    if "reverse" in combined_names and ("lakitu" in combined_names or "sign" in combined_names or num_frames == 16):
        for idx in range(min(num_frames, 16)):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituReverse{idx + 1:02d}", sl)
        return exported_count

    # 6. Fishing Hook (4 frames)
    if "fishing" in combined_names or "hook" in combined_names or ("fish" in combined_names and ("lakitu" in combined_names or num_frames == 4)):
        for idx in range(min(num_frames, 4)):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituFishing{idx + 1}", sl)
        return exported_count

    # 7. Semáforo combinado (24 frames)
    if ("traffic_light" in combined_names or "traffic" in combined_names or "signal" in combined_names) and num_frames == 24:
        for idx in range(8):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituNoLights{idx + 1}", sl)
        for idx in range(8, 24):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituRedLights{idx - 8 + 1:02d}", sl)
        return exported_count

    # 8. No Lights individual (8 frames)
    if "no_light" in combined_names or "nolight" in combined_names or (("traffic" in combined_names or "signal" in combined_names) and num_frames == 8):
        for idx in range(min(num_frames, 8)):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituNoLights{idx + 1}", sl)
        return exported_count

    # 9. Red Lights individual (16 frames)
    if "red_light" in combined_names or "redlight" in combined_names or (("traffic" in combined_names or "signal" in combined_names) and num_frames == 16):
        for idx in range(min(num_frames, 16)):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"gTextureLakituRedLights{idx + 1:02d}", sl)
        return exported_count

    # 10. Sombra do kart
    if "kart_shadow" in combined_names or ("shadow" in combined_names and "kart" in combined_names):
        write_pack_image("kart_shadow", data[:needed])
        return exported_count

    # 11. Logotipo Mario Kart 64
    if "logo" in combined_names and ("mario" in combined_names or "kart" in combined_names or "title" in combined_names):
        write_pack_image("logo_mario_kart_64", data[:needed])
        return exported_count

    # 12. Qualquer outra textura do Lakitu multi-frame
    if ("lakitu" in combined_names or "jugemu" in combined_names) and num_frames > 1:
        base_name = sym_names[0] if sym_names else "gTextureLakitu"
        for idx in range(num_frames):
            sl = data[idx * needed : (idx + 1) * needed]
            write_pack_image(f"{base_name}{idx + 1:02d}", sl)

    return exported_count


# ---------------------------------------------------------------- metadata do proprio codigo-fonte

SOURCE_TEX_RE = re.compile(r'(?P<loader>(?:gs|g)DPLoadTextureBlock(?:_4b)?)\s*\([^;\n]*?(?P<symbol>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*G_IM_FMT_(?P<fmt>RGBA|IA|I|CI)(?:\s*,\s*G_IM_SIZ_(?P<size>4b|8b|16b|32b))?\s*,\s*(?P<w>\d+)\s*,\s*(?P<h>\d+)\s*,', re.I)

SOURCE_HELPER_TEX_RES = (
    (re.compile(r'\bload_texture_block_i8_nomirror\s*\(\s*(?P<symbol>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*(?P<w>0x[0-9A-Fa-f]+|\d+)\s*,\s*(?P<h>0x[0-9A-Fa-f]+|\d+)\s*\)', re.I), 'i8'),
    (re.compile(r'\bload_texture_block_i4_nomirror\s*\(\s*(?P<symbol>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*(?P<w>0x[0-9A-Fa-f]+|\d+)\s*,\s*(?P<h>0x[0-9A-Fa-f]+|\d+)\s*\)', re.I), 'i4'),
    (re.compile(r'\bfunc_80044F34\s*\(\s*(?P<symbol>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*(?P<w>0x[0-9A-Fa-f]+|\d+)\s*,\s*(?P<h>0x[0-9A-Fa-f]+|\d+)\s*\)', re.I), 'i4'),
)
SOURCE_FMT_RE = re.compile(r'\.(rgba32|rgba16|ia16|ia8|ia4|i8|i4|ci8|ci4)\.inc\.c$', re.I)

def source_format(fmt_name, size_name=None):
    f,z=(fmt_name or '').lower(),(size_name or '').lower()
    if f=='rgba': return {'32b':'rgba32','16b':'rgba16'}.get(z)
    if f=='ia': return {'16b':'ia16','8b':'ia8','4b':'ia4'}.get(z)
    if f=='i': return {'8b':'i8','4b':'i4'}.get(z)
    if f=='ci': return {'8b':'ci8','4b':'ci4'}.get(z)
    return None


def load_source_texture_metadata(root):
    """Lê metadata de uso de textura em todos os C/H do projeto."""
    found = defaultdict(set)
    files = []
    for base in (root / 'assets', root / 'src', root / 'include'):
        if not base.is_dir():
            continue
        for path in base.rglob('*'):
            if path.suffix.lower() not in ('.c', '.h', '.inc'):
                continue
            try:
                files.append((path, path.read_text(encoding='utf-8', errors='ignore')))
            except OSError:
                pass

    aliases = defaultdict(set)
    dma_re = re.compile(
        r'\b(?P<var>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*\(?\s*'
        r'(?:void\s*\*\s*\)?\s*)?dma_textures\s*\(\s*'
        r'(?P<symbol>[A-Za-z_][A-Za-z0-9_]*)\s*,', re.I)
    for _path, text in files:
        for m in dma_re.finditer(text):
            aliases[m.group('var')].add(m.group('symbol'))

    for _path, text in files:
        for m in SOURCE_TEX_RE.finditer(text):
            fmt = source_format(m.group('fmt'), m.group('size') or
                                ('4b' if m.group('loader').lower().endswith('_4b') else None))
            if fmt:
                sym = m.group('symbol')
                if sym in aliases:
                    for original in aliases[sym]:
                        found[original].add((int(m.group('w')), int(m.group('h')), fmt))
                else:
                    found[sym].add((int(m.group('w')), int(m.group('h')), fmt))

        for helper_re, helper_fmt in SOURCE_HELPER_TEX_RES:
            for m in helper_re.finditer(text):
                sym = m.group('symbol')
                if sym in aliases:
                    for original in aliases[sym]:
                        found[original].add((int(m.group('w'), 0), int(m.group('h'), 0), helper_fmt))
                else:
                    found[sym].add((int(m.group('w'), 0), int(m.group('h'), 0), helper_fmt))

        init_re = re.compile(
            r'\binit_texture_object\s*\(\s*[^,]+,\s*(?P<tlut>[^,]+),\s*'
            r'(?:\(\s*[A-Za-z_][A-Za-z0-9_]*\s*\*?\s*\)\s*)?'
            r'(?P<tex>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*'
            r'(?P<w>0x[0-9A-Fa-f]+|\d+)\s*,\s*'
            r'(?P<h>0x[0-9A-Fa-f]+|\d+)\s*\)', re.I)
        for m in init_re.finditer(text):
            tex = m.group('tex')
            w, h = int(m.group('w'), 0), int(m.group('h'), 0)
            targets = aliases.get(tex, {tex})
            for original in targets:
                found[original].add((w, h, 'ci8'))

        fn_header_re = re.compile(
            r'\b(?P<rtype>void|int|s32|u32|s16|u16|s8|u8|static\s+\w+)\s+'
            r'(?P<fn>[A-Za-z_][A-Za-z0-9_]*)\s*\((?P<args>[^()]*)\)\s*\{', re.S)
        fn_specs = []
        for fm in fn_header_re.finditer(text):
            args = [a.strip() for a in fm.group('args').split(',')]
            body = text[fm.end():fm.end()+20000]
            for pos, argdecl in enumerate(args):
                names = re.findall(r'[A-Za-z_][A-Za-z0-9_]*', argdecl)
                if not names:
                    continue
                arg = names[-1]
                for lm in SOURCE_TEX_RE.finditer(body):
                    if lm.group('symbol') != arg:
                        continue
                    fmt = source_format(lm.group('fmt'), lm.group('size') or
                                        ('4b' if lm.group('loader').lower().endswith('_4b') else None))
                    if fmt:
                        fn_specs.append((fm.group('fn'), pos, int(lm.group('w')), int(lm.group('h')), fmt))

        for fn, pos, w, h, fmt in fn_specs:
            call_re = re.compile(r'\b' + re.escape(fn) + r'\s*\(([^;\n]*?)\)')
            for cm in call_re.finditer(text):
                vals = [v.strip() for v in cm.group(1).split(',')]
                if pos >= len(vals):
                    continue
                actual = vals[pos]
                if actual in aliases:
                    for original in aliases[actual]:
                        found[original].add((w, h, fmt))

        for call_re, fmt in (
            (re.compile(r'\bfunc_80044DA0\s*\((?P<arg>[^,]+),\s*(?P<w>0x[0-9A-Fa-f]+|\d+)\s*,\s*(?P<h>0x[0-9A-Fa-f]+|\d+)'), 'i4'),
            (re.compile(r'\bfunc_80044BF8\s*\((?P<arg>[^,]+),\s*(?P<w>0x[0-9A-Fa-f]+|\d+)\s*,\s*(?P<h>0x[0-9A-Fa-f]+|\d+)'), 'i8'),
        ):
            for cm in call_re.finditer(text):
                arg_expr = cm.group('arg')
                w, h = int(cm.group('w'), 0), int(cm.group('h'), 0)
                for alias_var, originals in aliases.items():
                    if re.search(r'\b' + re.escape(alias_var) + r'\b', arg_expr):
                        for original in originals:
                            found[original].add((w, h, fmt))

    result, conflicts = {}, {}
    for sym, vals in found.items():
        if len(vals) == 1:
            w, h, fmt = next(iter(vals))
            result[sym] = {'width': w, 'height': h, 'type': fmt}
        else:
            conflicts[sym] = sorted(vals)
    return result, conflicts

def infer_from_filename_and_size(rel,size):
    m=SOURCE_FMT_RE.search(rel)
    if not m: return None
    fmt=m.group(1).lower(); bits=BPP.get(fmt)
    if not bits or (size*8)%bits: return None
    npix=size*8//bits; candidates=[]; w=1
    while w<=npix:
        if npix%w==0:
            h=npix//w
            if (w&(w-1))==0 and (h&(h-1))==0 and w<=1024 and h<=1024: candidates.append((w,h))
        w<<=1
    return (*candidates[0],fmt) if len(candidates)==1 else None

def detect_rom(root,explicit=None):
    if explicit:
        path=Path(explicit).resolve()
        if not path.is_file(): sys.exit('ROM nao encontrada: '+str(path))
        return path,'explicit'
    known_crc={0x434389C1:'us',0x3B0D98C1:'br'}
    known_sha1={'c2baf5b4a5355fff2dac08e971a62834ef70268c':'br'}
    import hashlib
    candidates=[]
    for path in sorted(root.glob('baserom.*.z64'))+sorted(root.glob('*.z64')):
        if path.is_file() and path not in candidates: candidates.append(path)
    for path in candidates:
        try: data=path.read_bytes()
        except OSError: continue
        crc=zlib.crc32(data)&0xffffffff
        if crc in known_crc: return path.resolve(),known_crc[crc]
        sha1=hashlib.sha1(data).hexdigest()
        if sha1 in known_sha1: return path.resolve(),known_sha1[sha1]
    for name in ('baserom.br.z64','baserom.us.z64'):
        path=root/name
        if path.is_file(): return path.resolve(),('br' if name=='baserom.br.z64' else 'us')
    return None,None

# ---------------------------------------------------------------- dimensoes (fallback quando nao ha YAML)

BPP = {"rgba32": 32, "rgba16": 16, "ia16": 16, "ia8": 8, "i8": 8,
       "ci8": 8, "ia4": 4, "i4": 4, "ci4": 4}


def guess_dims(npix):
    cands = []
    w = 1
    while w <= npix:
        if npix % w == 0:
            h = npix // w
            if (h & (h - 1)) == 0:
                cands.append((w, h))
        w <<= 1
    if not cands:
        return None
    cands.sort(key=lambda p: (abs(p[0] / p[1] - 1), -p[0]))
    return cands[0]


FMT_RE = re.compile(r"\.(rgba32|rgba16|ia16|ia8|ia4|i8|i4|ci8|ci4)\.inc\.c$", re.I)
EXCLUDE_RE = re.compile(r"staff_ghost", re.I)


def symbol_of(rel):
    """'assets/.../common_texture_traffic_light_01.ci8.inc.c' -> 'common_texture_traffic_light_01'"""
    base = Path(rel).name
    return base.split(".", 1)[0]


# ---------------------------------------------------------------- sprites dos pilotos/karts

def int_value(value):
    """Aceita tanto offsets JSON em hexadecimal quanto inteiros."""
    return int(value, 0) if isinstance(value, str) else int(value)


def extract_kart_sprites(root, rom, outdir, cache, overwrite=True):
    """Exporta os 321 quadros CI8 de cada piloto+kart."""
    kart_dir = root / "assets" / "karts"
    manifest = []
    count = 0
    if not kart_dir.is_dir():
        print("Karts : assets\\karts nao encontrado; ignorando sprites de piloto.")
        return count

    for json_path in sorted(kart_dir.glob("*_kart.json")):
        entries = json.loads(json_path.read_text(encoding="utf-8"))
        for symbol, info in entries.items():
            if not symbol.endswith("_frame") and "_frame" not in symbol:
                continue
            if info.get("type", "").lower() != "ci8" or not info.get("tlut"):
                continue

            w, h = int(info["width"]), int(info["height"])
            expected = w * h
            compressed = source_bytes(rom, int_value(info["rom_offset"]), cache, expected)
            if len(compressed) < expected:
                print(f"  ! quadro curto: {symbol} ({len(compressed)} < {expected})")
                continue
            pixels = compressed[:expected]

            palette = []
            for palette_symbol in info["tlut"]:
                palette_info = entries.get(palette_symbol)
                if palette_info is None:
                    raise KeyError(f"TLUT ausente para {symbol}: {palette_symbol}")
                pw, ph = int(palette_info["width"]), int(palette_info["height"])
                raw_palette = source_bytes(
                    rom, int_value(palette_info["rom_offset"]), cache, pw * ph * 2)
                palette.extend(decode_palette(raw_palette))
            if len(palette) < 256:
                raise ValueError(f"TLUT incompleta para {symbol}: {len(palette)} cores")

            rgba = decode("ci8", pixels, w, h, palette, transparent_black=TRANSPARENT_BLACK)
            relative_output = Path(info["output_dir"]) / f"{symbol}.png"
            write_png(outdir / "karts" / relative_output, w, h, rgba, overwrite=overwrite)
            manifest.append({
                "png": str(Path("karts") / relative_output).replace("\\", "/"),
                "symbol": symbol,
                "rom_offset": f"0x{int_value(info['rom_offset']):X}",
                "width": w,
                "height": h,
                "format": "ci8",
                "decoded_hash_fnv1a32": f"{fnv1a32(pixels):08x}",
                "tmem_halves": tmem_halves_for(pixels, w, h, is_kart=True),
                "tlut": info["tlut"],
            })
            count += 1

    (outdir / "kart_sprite_manifest.json").write_text(
        json.dumps(manifest, indent=1), encoding="utf-8")
    return count


# ---------------------------------------------------------------- sprites do Lakitu (assets/lakitu/*.json)

def find_lakitu_sources(root):
    """Localiza o diretório e arquivos JSON do Lakitu no projeto.
    Suporta assets/lakitu, mk64_master/assets/lakitu e subdiretórios."""
    candidates = [
        root / "assets" / "lakitu",
        root / "mk64_master" / "assets" / "lakitu",
        root / "mk64-master" / "assets" / "lakitu",
    ]
    for c in candidates:
        if c.is_dir():
            jsons = sorted(c.glob("*.json"))
            if jsons:
                return c, jsons

    # Busca alternativa por qualquer diretório lakitu dentro de assets
    assets_dir = root / "assets"
    if assets_dir.is_dir():
        for d in assets_dir.rglob("lakitu"):
            if d.is_dir():
                jsons = sorted(d.glob("*.json"))
                if jsons:
                    return d, jsons

    return None, []


def extract_lakitu_sprites(root, rom, outdir, cache, asset_json_symbols=None, overwrite=True):
    """
    Extrai TODOS os quadros do Lakitu mapeados em assets/lakitu/*.json
    (semáforo de largada, contramão, volta final, pesca/resgate, bandeirada, etc).

    Gera exatamente a estrutura de lakitu_sprite_manifest.json esperada com:
      - "png": "lakitu/bluelight/gTextureLakituBlueLight4.png"
      - "symbol": "gTextureLakituBlueLight4"
      - "rom_offset": "0x6BB400"
      - "width": 56, "height": 72, "format": "ci8"
      - "decoded_hash_fnv1a32": "05190c5d"
      - "tmem_halves": [ { "y0": 0, "y1": 36, "hash": "..." }, { "y0": 35, "y1": 72, "hash": "..." } ]
    """
    lakitu_dir, jsons = find_lakitu_sources(root)
    if not lakitu_dir or not jsons:
        print("Lakitu: assets/lakitu nao encontrado ou sem arquivos .json; ignorando.")
        return 0, 0, []

    # Carrega todas as entradas de todos os JSONs do Lakitu para resolução cruzada de TLUTs
    all_lakitu_entries = {}
    json_data_by_file = {}
    for jp in jsons:
        try:
            data = json.loads(jp.read_text(encoding="utf-8"))
            if isinstance(data, dict):
                json_data_by_file[jp] = data
                all_lakitu_entries.update(data)
        except Exception as exc:
            print(f"  ! erro ao ler {jp.name}: {exc}")

    manifest = []
    pulados = []
    por_categoria = defaultdict(int)

    def _get_entry(key):
        return (all_lakitu_entries.get(key) or
                (asset_json_symbols.get(key) if asset_json_symbols else None))

    for jp, entries in json_data_by_file.items():
        for symbol, info in entries.items():
            if not isinstance(info, dict):
                continue

            fmt = str(info.get("type", "")).lower()
            if fmt not in BPP:
                continue

            # Paletas são entradas auxiliares, não quadros gráficos de sprite
            if symbol.startswith("common_tlut") or "tlut" in symbol.lower():
                continue

            tlut_ref = info.get("tlut") or info.get("tlut_symbol")
            if fmt.startswith("ci") and not tlut_ref:
                pulados.append((symbol, "sem TLUT declarada"))
                continue

            try:
                w, h = int(info["width"]), int(info["height"])
            except (KeyError, TypeError, ValueError):
                pulados.append((symbol, "largura/altura ausente ou inválida"))
                continue

            needed = (w * h * BPP[fmt] + 7) // 8
            boff = int_value(info.get("block_offset", 0))
            raw = source_bytes(rom, int_value(info["rom_offset"]), cache, boff + needed)
            pixels = raw[boff:boff + needed]
            if len(pixels) < needed:
                pulados.append((symbol, f"dados curtos ({len(pixels)} < {needed})"))
                continue

            palette = None
            if fmt.startswith("ci"):
                refs = [tlut_ref] if isinstance(tlut_ref, str) else list(tlut_ref)
                palette = []
                falhou = None
                for pal_symbol in refs:
                    pal = _get_entry(pal_symbol)
                    if pal is None:
                        falhou = f"TLUT ausente: {pal_symbol}"
                        break
                    try:
                        pw, ph = int(pal["width"]), int(pal["height"])
                        pboff = int_value(pal.get("block_offset", 0))
                        need_pal = pw * ph * 2
                        praw = source_bytes(rom, int_value(pal["rom_offset"]), cache, pboff + need_pal)
                        palette.extend(decode_palette(praw[pboff:pboff + need_pal]))
                    except Exception as p_exc:
                        falhou = f"erro ao decodificar TLUT {pal_symbol}: {p_exc}"
                        break
                if falhou:
                    pulados.append((symbol, falhou))
                    continue

            try:
                rgba = decode(fmt, pixels, w, h, palette, transparent_black=TRANSPARENT_BLACK)
            except Exception as exc:
                pulados.append((symbol, str(exc)))
                continue

            categoria = info.get("output_dir") or jp.stem
            rel = Path("lakitu") / categoria / f"{symbol}.png"
            destino = outdir / rel

            write_png(destino, w, h, rgba, overwrite=overwrite)
            por_categoria[categoria] += 1

            # Entrada compatível exatamente com a especificação do port e a foto do bloco de notas
            manifest.append({
                "png": str(rel).replace("\\", "/"),
                "symbol": symbol,
                "rom_offset": f"0x{int_value(info['rom_offset']):X}",
                "width": w,
                "height": h,
                "format": fmt,
                "decoded_hash_fnv1a32": f"{fnv1a32(pixels):08x}",
                "tmem_halves": tmem_halves_for(pixels, w, h) if fmt == "ci8" else [],
            })

    if manifest:
        saida = outdir / "lakitu_sprite_manifest.json"
        saida.parent.mkdir(parents=True, exist_ok=True)
        saida.write_text(json.dumps(manifest, indent=1), encoding="utf-8")

    return len(manifest), len(pulados), por_categoria


def procurar_lakitu_fora_dos_jsons(root, manifest):
    """Localiza símbolos com 'lakitu' no nome que existam nos bancos do port
    mas não tenham aparecido nos JSONs."""
    ja = {e["symbol"].lower() for e in manifest}
    achados = set()
    banks = root / "src" / "xbox360" / "generated_banks"
    if banks.is_dir():
        for c in banks.glob("*.c"):
            try:
                txt = c.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            for linha in txt.splitlines():
                if "lakitu" not in linha.lower() or "[]" not in linha:
                    continue
                for tok in linha.replace("[", " ").replace("]", " ").split():
                    if "lakitu" in tok.lower() and tok.lower() not in ja:
                        achados.add((tok, c.name))
    return sorted(achados)


# ---------------------------------------------------------------- canonicalização segura dos aliases

def canonicalize_common_hash_aliases(outdir):
    """Remove apenas HASH__nome.png que seja byte-a-byte igual ao PNG canônico
    de um manifest generated. Recursos sem prova de igualdade permanecem intactos.
    """
    generated_manifest = outdir / "generated_texture_manifest.json"
    if not generated_manifest.is_file():
        return {"removed": [], "kept": []}
    try:
        entries = json.loads(generated_manifest.read_text(encoding="utf-8"))
    except Exception:
        return {"removed": [], "kept": []}

    by_key = defaultdict(list)
    for e in entries:
        png = str(e.get("png", ""))
        h = str(e.get("decoded_hash_fnv1a32", "")).lower()
        if not png or len(h) != 8:
            continue
        by_key[(h, int(e.get("width", 0) or 0), int(e.get("height", 0) or 0))].append(png)

    removed, kept = [], []
    for png in sorted(outdir.glob("[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]__*.png")):
        name = png.name
        h = name[:8].lower()
        candidates = []
        try:
            # write_png() é determinístico: byte-identidade do PNG é uma prova
            # forte de que os dois caminhos representam exatamente os mesmos pixels.
            for rel in by_key.get((h, 0, 0), []):
                candidates.append(outdir / rel)
            # Quando dimensões não estão indexadas no nome, percorremos somente
            # os manifests com o mesmo hash. O conjunto é pequeno.
            if not candidates:
                for (kh, _w, _h), rels in by_key.items():
                    if kh == h:
                        candidates.extend(outdir / r for r in rels)
            matched = next((c for c in candidates if c.is_file() and c.read_bytes() == png.read_bytes()), None)
        except OSError:
            matched = None
        if matched is not None:
            try:
                png.unlink()
                removed.append({"alias": str(png.relative_to(outdir)).replace("\\", "/"),
                                "canonical": str(matched.relative_to(outdir)).replace("\\", "/"),
                                "hash": h})
            except OSError:
                kept.append(str(png.relative_to(outdir)).replace("\\", "/"))
        else:
            kept.append(str(png.relative_to(outdir)).replace("\\", "/"))

    report = {
        "method": "byte-identical PNG + manifest decoded hash",
        "removed_count": len(removed),
        "kept_count": len(kept),
        "removed": removed,
        "kept_hash_aliases": kept,
        "note": "Hash-prefixed files without exact proof remain untouched."
    }
    (outdir / "texture_alias_manifest.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report


# ---------------------------------------------------------------- Fundos HD dos menus (integrado do GERAR_FUNDOS_MENU_HD/TINT_MENU_BACKGROUNDS)

MENU_FUNDOS = ("background_blue_sky", "background_sunset")
MENU_CORES = {
    "modo": (0xFF, 0xAF, 0xAF),
    "personagem": (0xAF, 0xFF, 0xAF),
    "pista": (0xAF, 0xAF, 0xFF),
}
MENU_GREY_ARG = 0x19


def _png_size(path):
    """Retorna (W,H) de um PNG sem exigir Pillow."""
    try:
        with path.open("rb") as f:
            if f.read(8) != b"\x89PNG\r\n\x1a\n":
                return None
            if f.read(4) != b"\x00\x00\x00\r":
                return None
            if f.read(4) != b"IHDR":
                return None
            import struct as _struct
            data = f.read(8)
            if len(data) != 8:
                return None
            return _struct.unpack(">II", data)
    except (OSError, ValueError):
        return None


def _menu_manifest_info(outdir):
    p = outdir / "texture_tkmk00_manifest.json"
    if not p.is_file():
        return {}
    try:
        return {e.get("symbol"): e for e in json.loads(p.read_text(encoding="utf-8"))
                if e.get("symbol")}
    except Exception:
        return {}


def preserve_menu_originals_before_extraction(root, outdir):
    """
    Guarda os fundos originais antes de uma nova extração. Isso permite que
    a segunda execução use a ROM original mesmo depois que o PNG da pasta
    principal tiver sido substituído pela arte HD.
    """
    manifest = _menu_manifest_info(outdir)
    for fundo in MENU_FUNDOS:
        entry = manifest.get(fundo, {})
        rel = entry.get("png") or f"generated/texture_tkmk00/{fundo}.png"
        src = outdir / rel
        if not src.is_file():
            continue
        expected = (int(entry.get("width", 0)), int(entry.get("height", 0)))
        if not expected[0]:
            expected = (320, 240)
        size = _png_size(src)
        if size != expected:
            # Já é uma arte HD/editada; nunca use essa imagem como "original".
            continue
        backup = outdir / "originais" / rel
        if not backup.is_file() and size:
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, backup)



def extract_tkmk00_originals_only(root, rom, outdir, region="us"):
    """Na segunda execução, extrai SOMENTE os TKMK00 para extracted_textures/originais.

    Nunca escreve em extracted_textures/generated, karts, lakitu ou qualquer outra
    pasta que contenha arte editada pelo usuário. Os PNGs daqui são a referência
    original da ROM usada pela etapa de geração dos backgrounds HD.
    """
    dest_root = outdir / "originais" / "generated" / "texture_tkmk00"
    dest_root.mkdir(parents=True, exist_ok=True)
    helper = find_tkmk_helper(root)
    if helper is None:
        print("  ! TKMK00: helper não encontrado/compilável; originais não atualizados.")
        return 0
    entries = load_tkmk_asset_entries(root, region)
    if not entries:
        print("  ! TKMK00: nenhum asset encontrado em assets.json.")
        return 0
    count = 0
    cache = {}
    for entry in entries:
        dst = dest_root / f"{entry['symbol']}.png"
        try:
            source = source_bytes(rom, int_value(entry["rom_offset"]), cache, int_value(entry["size"]))
            # Originais são sempre reconstruídos da ROM, mas somente dentro de
            # extracted_textures/originais; a árvore editada nunca é tocada.
            extract_tkmk00_texture(root, source, helper, dst, entry["alpha_color"], overwrite=True)
            count += 1
        except Exception as exc:
            print(f"  ! TKMK00 original: {entry['symbol']}: {exc}")
    print(f"  TKMK00 originais extraídos: {count} -> {dest_root}")
    return count

def menu_background_relpaths(root, outdir):
    manifest = _menu_manifest_info(outdir)
    result = {}
    for fundo in MENU_FUNDOS:
        e = manifest.get(fundo, {})
        rel = e.get("png")
        if rel:
            result[fundo] = (rel, int(e.get("width", 0)), int(e.get("height", 0)))
    return result


def menu_hd_exists(outdir):
    """True quando pelo menos um fundo já foi substituído por uma arte maior."""
    for fundo, (rel, w, h) in menu_background_relpaths(None, outdir).items():
        p = outdir / rel
        if p.is_file() and w and h:
            size = _png_size(p)
            if size and size != (w, h):
                return True
    return False


def menu_original_backup(outdir, rel, w, h):
    """Localiza somente o original preservado, nunca a arte HD atual."""
    backup = outdir / "originais" / rel
    if backup.is_file() and _png_size(backup) == (w, h):
        return backup

    # Compatibilidade com a base anterior, que usava extracted_originais.
    legacy_root = outdir.parent / "extracted_originais"
    legacy = legacy_root / rel
    if legacy.is_file() and _png_size(legacy) == (w, h):
        # Migra silenciosamente para a nova pasta, evitando depender do
        # GERAR_FUNDOS_MENU_HD.py nas próximas execuções.
        try:
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(legacy, backup)
            return backup
        except OSError:
            return legacy

    # Compatibilidade com uma eventual pasta _originais criada anteriormente.
    legacy_current = outdir / rel
    if legacy_current.is_file() and _png_size(legacy_current) == (w, h):
        return legacy_current
    return None


def _menu_pow2(value, exponent):
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


def _menu_normalize(x):
    e = 0
    while x < 0.5 or x >= 1.0:
        if x < 0.5:
            x *= 2.0
            e -= 1
        else:
            x /= 2.0
            e += 1
    return x, e


def _menu_ln(x):
    if x <= 0.0:
        return 0.0
    _, sp38 = _menu_normalize(x / 1.414213562373095)
    x /= _menu_pow2(1.0, sp38)
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


def _menu_exp(x):
    t10 = int((0.5 if x >= 0.0 else -0.5) + (x / 0.6931471805599453))
    x -= t10 * 0.6931471805599453
    f2 = x * x
    f0 = f2 / 22
    for i in range(4):
        f0 = f2 / ((18 - 4 * i) + f0)
    f2 = 2 + f0
    return _menu_pow2((f2 + x) / (f2 - x), t10)


def _menu_pow(a, b):
    if -2147483647.0 <= b <= 2147483647.0 and b == int(b):
        r = 1.0
        for _ in range(int(b)):
            r *= a
        return r
    if a > 0.0:
        return _menu_exp(_menu_ln(a) * b)
    return 0.0


def _menu_gray_table():
    import numpy as np
    exp_ = (MENU_GREY_ARG * 1.5 / 256.0) + 0.25
    tab = np.array([_menu_pow(i / 32.0, exp_) for i in range(32)], dtype=np.float32)
    nivel = (tab * np.float32(32.0)).astype(np.uint32)
    return np.minimum(nivel, 31), exp_


def _menu_tint_hd(img, cor):
    import numpy as np
    from PIL import Image
    _, exp_ = _menu_gray_table()
    a = np.asarray(img.convert("RGBA")).astype(np.float64)
    r5, g5, b5 = a[..., 0] * 31 / 255, a[..., 1] * 31 / 255, a[..., 2] * 31 / 255
    cinza = (r5 * 0x55 + g5 * 0x4B + b5 * 0x5F) / 256.0
    t = np.minimum(np.power(np.clip(cinza / 32.0, 0, 1), exp_) * 32.0, 31.0)
    out = np.empty_like(a)
    for k in range(3):
        out[..., k] = (t * cor[k] / 256.0) * 255 / 31
    out[..., 3] = a[..., 3]
    return Image.fromarray(np.clip(np.rint(out), 0, 255).astype(np.uint8), "RGBA")


def _menu_png_to_rgba16(path, w, h):
    import numpy as np
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    if im.size != (w, h):
        return None
    a = np.asarray(im).astype(np.uint32)
    v = (np.rint(a[..., 0] * 31 / 255).astype(np.uint32) << 11) | \
        (np.rint(a[..., 1] * 31 / 255).astype(np.uint32) << 6) | \
        (np.rint(a[..., 2] * 31 / 255).astype(np.uint32) << 1) | \
        (a[..., 3] >= 128)
    return v.astype(">u2").tobytes()


def _menu_tint_rgba16(raw, cor):
    import numpy as np
    nivel, _ = _menu_gray_table()
    v = np.frombuffer(raw, dtype=">u2").astype(np.uint32)
    r = ((v & 0xF800) >> 11) * 0x55
    g = ((v & 0x07C0) >> 6) * 0x4B
    b = ((v & 0x003E) >> 1) * 0x5F
    a = v & 1
    t = nivel[(r + g + b) // 256]
    lum = (t * 0x4D + t * 0x96 + t * 0x1D) // 256
    out = (((lum * cor[0]) // 256) << 11) + \
          (((lum * cor[1]) // 256) << 6) + \
          (((lum * cor[2]) // 256) << 1) + a
    return out.astype(">u2").tobytes()


def _menu_fnv1a32(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def generate_hd_menu_backgrounds(root, outdir):
    """
    Equivalente interno do antigo TINT_MENU_BACKGROUNDS.py.
    Usa os PNGs originais preservados em extracted_textures/originais e
    a arte HD atualmente presente em extracted_textures.
    """
    try:
        from PIL import Image
        import numpy as np  # noqa: F401
    except ImportError:
        print("  ! Para gerar os fundos HD, instale Pillow e numpy: py -m pip install pillow numpy")
        return False

    imagens = _menu_manifest_info(outdir)
    if not imagens:
        print("  ! texture_tkmk00_manifest.json não encontrado; fundos não gerados.")
        return False

    layout, fonte = None, ""
    anim_p = root / "manifest_runtime" / "texture_tkmk00_anim_manifest.json"
    if anim_p.is_file():
        try:
            anim = {e["symbol"]: e for e in json.loads(anim_p.read_text(encoding="utf-8"))}
            if len(anim.get("background_blue_sky", {}).get("tiles", [])) > 10:
                layout, fonte = anim["background_blue_sky"]["tiles"], "medida"
        except Exception:
            pass

    if layout is None:
        rt_p = root / "manifest_runtime" / "texture_tkmk00_runtime_manifest.json"
        if rt_p.is_file():
            try:
                for e in json.loads(rt_p.read_text(encoding="utf-8")):
                    if e.get("symbol") == "background_blue_sky" and e.get("tmem_tiles_runtime"):
                        layout = [dict(t, y1=t["y1"] + 1) for t in e["tmem_tiles_runtime"]
                                  ]
                        fonte = "eduardo"
                        break
            except Exception:
                pass

    if layout is None:
        layout = [{"x0": 0, "y0": y, "x1": 320, "y1": y + 3}
                  for y in range(0, 240, 2)]
        fonte = "padrao"

    print(f"  disposição dos blocos do fundo: {len(layout)} blocos "
          f"({'medida com a sua ROM' if fonte == 'medida' else 'manifest do fork' if fonte == 'eduardo' else 'padrão embutido'})")

    saida, resumo = [], []
    for fundo in MENU_FUNDOS:
        e = imagens.get(fundo)
        if not e:
            continue
        W, H, rel = int(e["width"]), int(e["height"]), e["png"]
        orig_path = menu_original_backup(outdir, rel, W, H)
        if not orig_path:
            print(f"  {fundo}: PNG original ({W}x{H}) não encontrado em extracted_textures/originais -- pulado")
            continue

        orig = _menu_png_to_rgba16(orig_path, W, H)
        if not orig:
            print(f"  {fundo}: não foi possível ler o PNG original -- pulado")
            continue

        if fundo == "background_blue_sky":
            ok = tot = 0
            for t in layout:
                off = (t["y0"] * W + t["x0"]) * 2
                L = (t["x1"] - t["x0"]) * (t["y1"] - t["y0"]) * 2
                if "hash" in t and off + L <= len(orig):
                    tot += 1
                    ok += _menu_fnv1a32(orig[off:off + L]) == int(t["hash"], 16)
            if tot:
                print(f"  conferência da fórmula: {ok}/{tot} blocos do fundo original batem")
            if fonte == "medida" and (tot == 0 or ok < tot):
                print("  ! A fórmula não bateu com os hashes medidos; nada será gerado.")
                return False
            if fonte == "eduardo" and tot and ok < tot:
                print("  (aviso: hashes do manifest podem ser de outra ROM/região; disposição mantida.)")

        hd_path = outdir / rel
        hd = Image.open(hd_path).convert("RGBA") if hd_path.is_file() else None
        if hd is None or hd.size == (W, H):
            print(f"  {fundo}: sem arte HD (o PNG ainda está no tamanho original) -- pulado")
            continue

        for menu, cor in MENU_CORES.items():
            tingido = _menu_tint_rgba16(orig, cor)
            tiles = []
            for t in layout:
                off = (t["y0"] * W + t["x0"]) * 2
                w = t["x1"] - t["x0"]
                L = w * (t["y1"] - t["y0"]) * 2
                L_log = L - w * 2
                nt = {k: t[k] for k in ("x0", "y0", "x1", "y1")}
                if off + L <= len(tingido):
                    nt["hash"] = f"{_menu_fnv1a32(tingido[off:off + L]):08x}"
                if L_log > 0 and off + L_log <= len(tingido):
                    nt["logical_hash"] = f"{_menu_fnv1a32(tingido[off:off + L_log]):08x}"
                if "hash" in nt or "logical_hash" in nt:
                    tiles.append(nt)

            nome_png = f"generated/texture_tkmk00/tinted/{fundo}__{menu}.png"
            destino = outdir / nome_png
            destino.parent.mkdir(parents=True, exist_ok=True)
            if destino.exists():
                print(f"  {nome_png}: já existe, mantido (apague para regerar)")
            else:
                _menu_tint_hd(hd, cor).save(destino)
            saida.append({"symbol": f"{fundo}__{menu}", "png": nome_png,
                          "width": W, "height": H, "tiles": tiles})
            resumo.append(f"  {fundo:<22} {menu:<10} {len(tiles)} bloco(s)")

    if not saida:
        print("  ! Nenhum fundo HD foi gerado.")
        return False

    dest = root / "manifest_runtime" / "texture_tkmk00_tinted_manifest.json"
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(saida, indent=1), encoding="utf-8")
    # O PACK procura primeiro aqui e depois em manifest_runtime.
    (outdir / dest.name).write_text(json.dumps(saida, indent=1), encoding="utf-8")
    print("\n".join(resumo))
    print(f"\n  manifest HD dos fundos: {dest}")
    print(f"  PNGs HD tingidos: {outdir / 'generated/texture_tkmk00/tinted'}")
    return True


def copy_runtime_manifests(root, outdir, include_tinted=False):
    """Copia os manifests de runtime para extracted_textures.

    texture_tkmk00_tinted_manifest.json só é liberado depois que a etapa HD
    desta execução o gerar; assim ele não aparece na primeira extração por
    causa de um arquivo antigo deixado em manifest_runtime.
    """
    src = root / "manifest_runtime"
    if not src.is_dir():
        print("  ! pasta manifest_runtime não encontrada.")
        return 0
    count = 0
    for p in src.rglob("*"):
        if not p.is_file():
            continue
        rel = p.relative_to(src)
        if rel.as_posix() == "texture_tkmk00_tinted_manifest.json" and not include_tinted:
            continue
        dst = outdir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst)
        count += 1
    return count


def ask_generate_hd_menu_backgrounds(root, outdir):
    """
    Pergunta somente após a extração. A resposta não altera a extração:
    serve apenas para disparar a etapa HD quando a arte já foi editada.
    """
    paths = menu_background_relpaths(root, outdir)
    hd = []
    for fundo, (rel, w, h) in paths.items():
        p = outdir / rel
        size = _png_size(p) if p.is_file() else None
        if size and w and h and size != (w, h):
            hd.append(f"{fundo} ({size[0]}x{size[1]})")

    print("\n" + "=" * 68)
    print("FUNDOS COLORIDOS DOS MENUS EM HD")
    print("=" * 68)
    if hd:
        print("Detectei arte HD já colocada em extracted_textures:")
        for item in hd:
            print(f"  - {item}")
        pergunta = "Você já colocou as imagens HD e quer gerar os backgrounds coloridos agora? [s/n]: "
    else:
        print("Os fundos ainda estão no tamanho original.")
        print("Quando você substituir background_blue_sky.png e/ou background_sunset.png por arte HD,")
        print("rode o EXTRACT_MK64_TEXTURES.py novamente para esta etapa gerar os fundos coloridos.")
        pergunta = "Você já colocou as imagens HD e quer gerar os backgrounds coloridos agora? [s/n]: "

    resp = input(pergunta).strip().lower()
    if resp not in ("s", "sim", "y", "yes", "ok", "1"):
        print("  Fundos coloridos HD: não gerados nesta execução.")
        return False

    return generate_hd_menu_backgrounds(root, outdir)


# ---------------------------------------------------------------- main

def main():
    global TRANSPARENT_BLACK
    ap = argparse.ArgumentParser(description="Extrator de Texturas de Alta Fidelidade do Mario Kart 64 (com suporte a Lakitu)")
    ap.add_argument("--root")
    ap.add_argument("--rom")
    ap.add_argument("--region", default="auto", help="região dos YAMLs (ex.: us, br). Se não existir, usa us")
    ap.add_argument("--out", default="extracted_textures")
    ap.add_argument("--dims", help="json opcional { 'path/rel.inc.c': [w,h] } para forcar dimensoes")
    ap.add_argument("--no-karts", action="store_true",
                    help="nao exporta os sprites de piloto+kart de assets/karts")
    ap.add_argument("--no-lakitu", action="store_true",
                    help="nao exporta os quadros do Lakitu de assets/lakitu")
    ap.add_argument("--no-generated", action="store_true",
                    help="nao exporta texturas verificadas dos bancos gerados do port 360")
    ap.add_argument("--force", action="store_true",
                    help="sobrescreve PNGs mesmo que ja existam no disco")
    ap.add_argument("--keep-black", action="store_true",
                    help="mantem fundo preto opaco em vez de aplicar transparencia automatica (I8/I4/CI)")
    a = ap.parse_args()

    if a.keep_black:
        TRANSPARENT_BLACK = False

    overwrite = a.force or not SKIP_EXISTING_PNGS

    root = Path(a.root).resolve() if a.root else Path(__file__).resolve().parent
    rom_path, detected_region = detect_rom(root, a.rom)
    if rom_path is None:
        sys.exit("ROM nao encontrada: use --rom .\\baserom.br.z64 ou coloque baserom.br.z64/baserom.us.z64 em mk64-master")
    rom = rom_path.read_bytes()
    if a.region == "auto" and detected_region in ("us", "br"):
        a.region = detected_region

    recipes = json.loads((root / "PUBLIC_ASSET_RECIPES.json").read_text(encoding="utf-8")) if (root / "PUBLIC_ASSET_RECIPES.json").is_file() else []
    overrides = json.loads(Path(a.dims).read_text(encoding="utf-8")) if a.dims else {}
    yaml_symbols = load_all_yaml_symbols(root, a.region)
    asset_json_symbols = load_asset_json_symbols(root)
    source_symbols, source_conflicts = load_source_texture_metadata(root)
    print(f"ROM : {rom_path.name} (região detectada={detected_region or 'desconhecida'})")
    print(f"YAML: {len(yaml_symbols)} simbolos carregados (região={a.region})")
    if a.region not in ("auto", "us") and not (root / "yamls" / a.region).is_dir():
        print(f"  ! yamls\\{a.region} nao existe; usando yamls\\us como fallback de metadata")
    print(f"JSON: {len(asset_json_symbols)} simbolos carregados de assets\\**\\*.json")
    print(f"SRC : {len(source_symbols)} simbolos com dimensao/formato encontrados no codigo")
    if source_conflicts:
        print(f"  ! conflitos no codigo para {len(source_conflicts)} simbolos; esses simbolos nao serao escolhidos automaticamente")

    recipes_by_symbol = {symbol_of(r["path"]): r for r in recipes}

    tlut_by_dir = defaultdict(list)
    for r in recipes:
        rp = str(r["path"]).replace("\\", "/")
        if "tlut" in rp.lower():
            tlut_by_dir[str(Path(rp).parent)].append(r)

    outdir = root / a.out

    # SEGUNDA EXECUÇÃO: extracted_textures já foi construído.
    # Nunca reextraia a árvore inteira sobre as imagens HD do usuário.
    # O marcador confiável é o manifest TKMK00 produzido pela primeira extração.
    continuation_mode = (outdir / "texture_tkmk00_manifest.json").is_file()
    if continuation_mode:
        print("\nModo continuação detectado: extracted_textures já foi extraído.")
        print("Nenhuma textura editada será reextraída ou sobrescrita.")
        # Atualiza somente a cópia protegida dos TKMK00 a partir da ROM.
        extract_tkmk00_originals_only(root, rom, outdir, a.region if a.region != "auto" else "us")
        print("\nManifestos de runtime: sincronizando com extracted_textures...")
        runtime_copied = copy_runtime_manifests(root, outdir)
        print(f"Manifestos runtime sincronizados: {runtime_copied}")
        generated_tinted = ask_generate_hd_menu_backgrounds(root, outdir)
        # Mesmo no modo continuação, os frames dos neons precisam ser
        # regenerados a partir da ROM/manifesto, sem sobrescrever qualquer
        # PNG HD já existente. Isso mantém o fluxo seguro para uma extração
        # que já foi editada pelo usuário.
        rainbow_cache = {}
        rainbow_frames, rainbow_frame_skipped = extract_rainbow_road_neon_frames(
            root, rom, outdir, rainbow_cache, overwrite=False
        )
        rainbow_manifest_path = outdir / "rainbow_road_neon_frames_manifest.json"
        rainbow_manifest_path.write_text(
            json.dumps(rainbow_frames, indent=1), encoding="utf-8"
        )
        (outdir / "rainbow_road_neon_frames_skipped.json").write_text(
            json.dumps(rainbow_frame_skipped, indent=1), encoding="utf-8"
        )
        print(f"Rainbow Road neon: {len(rainbow_frames)} frames visuais verificados -> {outdir / 'generated' / 'asset_json' / 'rainbow_road' / 'rainbow_road' / 'frames'}")

        # O manifesto consolidado é a fonte usada pelo PACK_TEXTURES.
        # Adiciona/atualiza somente os frames Rainbow Road, preservando tudo
        # que já existe na extração e qualquer outra textura HD do usuário.
        consolidated_path = outdir / "generated_texture_manifest.json"
        try:
            consolidated = json.loads(consolidated_path.read_text(encoding="utf-8")) if consolidated_path.is_file() else []
        except Exception:
            consolidated = []
        rainbow_pngs = {item.get("png") for item in rainbow_frames if item.get("png")}
        consolidated = [item for item in consolidated if item.get("png") not in rainbow_pngs]
        consolidated.extend(rainbow_frames)
        consolidated_path.write_text(json.dumps(consolidated, indent=1), encoding="utf-8")

        asset_manifest_path = outdir / "asset_json_texture_manifest.json"
        try:
            asset_manifest = json.loads(asset_manifest_path.read_text(encoding="utf-8")) if asset_manifest_path.is_file() else []
        except Exception:
            asset_manifest = []
        asset_manifest = [item for item in asset_manifest if item.get("png") not in rainbow_pngs]
        asset_manifest.extend(rainbow_frames)
        asset_manifest_path.write_text(json.dumps(asset_manifest, indent=1), encoding="utf-8")

        # Só depois da geração o tinted_manifest passa a ser copiado para
        # extracted_textures. Se não foi gerado, ele não é inventado.
        runtime_copied = copy_runtime_manifests(root, outdir, include_tinted=bool(generated_tinted))
        print(f"Manifestos runtime sincronizados: {runtime_copied}")
        return

    # PRIMEIRA EXECUÇÃO: a árvore ainda não existe como extração completa.
    # Não há nada HD para preservar neste momento.
    cache = {}
    dims_report = {}
    ok = skipped = guessed = from_metadata = from_source = from_format_size = from_override = 0
    skip_list = []

    for r in recipes:
        rel = str(r["path"]).replace("\\", "/")
        if "tlut" in rel.lower():
            continue
        if EXCLUDE_RE.search(rel):
            continue
        m = FMT_RE.search(rel)
        no_suffix = not m
        fmt = m.group(1).lower() if m else "ci8"
        size = int(r["size"])
        off = int(r.get("block_offset", 0))
        data = source_bytes(rom, int(r["rom_offset"]), cache, off + size)[off:off + size]

        sym = symbol_of(rel)
        yinfo = asset_json_symbols.get(sym) or yaml_symbols.get(sym)
        sinfo = source_symbols.get(sym)
        palette = None
        source_kind = None
        resolved = False

        if sinfo:
            w,h,fmt=int(sinfo['width']),int(sinfo['height']),sinfo['type']
            need=(w*h*BPP[fmt]+7)//8 if fmt in BPP else None
            if need is not None and need <= len(data):
                source_kind='source-code'; from_source += 1; resolved=True
            else:
                print(f"  ! codigo-fonte incompatível ({sym}: {w}x{h} {fmt} precisa {need}B, recipe tem {len(data)}B); procurando metadata/formato")
        elif yinfo and 'width' in yinfo and 'height' in yinfo:
            yw,yh=int(yinfo['width']),int(yinfo['height'])
            yfmt=yinfo.get('type',yinfo.get('format',fmt)).lower()
            eff_fmt=yfmt if yfmt in BPP else fmt
            need=(yw*yh*BPP[eff_fmt]+7)//8
            if need <= len(data):
                w,h,fmt=yw,yh,eff_fmt; source_kind='metadata'; from_metadata += 1; resolved=True
            else:
                print(f"  ! metadata incompatível ({sym}: {yw}x{yh} precisa {need}B, recipe tem {len(data)}B); procurando no codigo/formato")

        if not resolved:
            inferred=infer_from_filename_and_size(rel,size)
            if inferred:
                w,h,fmt=inferred; source_kind='format+size'; from_format_size += 1; resolved=True

        if not resolved and rel in overrides:
            w,h=map(int,overrides[rel]); source_kind='manual-override'; from_override += 1; resolved=True

        if not resolved:
            npix=size*8//BPP.get(fmt,8)
            wh=guess_dims(npix)
            if wh is None:
                skip_list.append(f"{rel}  ({npix}px, fmt={fmt})"); skipped += 1; continue
            w,h=wh; guessed += 1; source_kind='guess'
            print(f"  ? dimensao inferida por ultimo recurso: {sym} -> {w}x{h} ({fmt})")

        if fmt in ('ci4','ci8'):
            tlut_sym=None
            if yinfo:
                tlut_sym=yinfo.get('tlut',yinfo.get('tlut_symbol'))
                if isinstance(tlut_sym,list): tlut_sym=None
            if tlut_sym and tlut_sym in recipes_by_symbol:
                tr=recipes_by_symbol[tlut_sym]
                toff=int(tr.get('block_offset',0)); tsize=int(tr['size'])
                traw=source_bytes(rom,int(tr['rom_offset']),cache,toff+tsize)[toff:toff+tsize]
                palette=decode_palette(traw)
            elif no_suffix:
                dir_tluts=tlut_by_dir.get(str(Path(rel).parent),[])
                if len(dir_tluts)==1:
                    tr=dir_tluts[0]
                    toff=int(tr.get('block_offset',0)); tsize=int(tr['size'])
                    traw=source_bytes(rom,int(tr['rom_offset']),cache,toff+tsize)[toff:toff+tsize]
                    try: palette=decode_palette(traw)
                    except Exception: palette=None

        try:
            rgba = decode(fmt, data, w, h, palette, transparent_black=TRANSPARENT_BLACK)
        except Exception as e:
            print("  ! falha:", rel, e)
            skipped += 1
            continue

        h32 = fnv1a32(data)
        out_png_path = outdir / f"{h32:08x}__{Path(rel).stem}.png"
        write_png(out_png_path, w, h, rgba, overwrite=overwrite)

        export_community_pack_textures(
            outdir,
            [sym, Path(rel).stem, rel],
            fmt,
            data,
            w,
            h,
            palette,
            overwrite=overwrite
        )

        recipe_rom_offset = int_value(r["rom_offset"])
        recipe_block_offset = int_value(r.get("block_offset", 0))
        is_mio0 = rom[recipe_rom_offset:recipe_rom_offset + 4] == b"MIO0"

        dims_report[rel] = {
            "w": w,
            "h": h,
            "fmt": fmt,
            "size": size,
            "hash": f"{h32:08x}",
            "source": source_kind,
            "rom_offset": f"0x{recipe_rom_offset:X}",
            "block_offset": recipe_block_offset,
            "compressed": is_mio0,
            "compressed_size": None
        }
        ok += 1

    if dims_report:
        (root / "texture_dims.json").write_text(
            json.dumps(dims_report, indent=1, sort_keys=True), encoding="utf-8")


    repack_recipes = []
    for recipe in recipes:
        item = dict(recipe)
        item["path"] = str(item.get("path", "")).replace("\\\\", "/").replace("\\", "/")
        item["rom_offset"] = int_value(item["rom_offset"])
        item["size"] = int(item["size"])
        item["block_offset"] = int_value(item.get("block_offset", 0))
        repack_recipes.append(item)

    if repack_recipes:
        (root / "texture_repack_recipes.json").write_text(
            json.dumps(repack_recipes, indent=1, sort_keys=True), encoding="utf-8")
    if skip_list:
        (root / "texture_skip_list.txt").write_text("\n".join(skip_list), encoding="utf-8")

    cache.clear()
    kart_count = 0 if a.no_karts else extract_kart_sprites(root, rom, outdir, cache, overwrite=overwrite)
    cache.clear()

    # --- Extração dos sprites do Lakitu (incorporado diretamente do EXTRACT_LAKITU.py) ---
    lakitu_count = 0
    lakitu_skipped = 0
    lakitu_manifest = []
    if not a.no_lakitu:
        lakitu_count, lakitu_skipped, por_cat = extract_lakitu_sprites(
            root, rom, outdir, cache, asset_json_symbols=asset_json_symbols, overwrite=overwrite
        )
        if lakitu_count > 0:
            lakitu_manifest_file = outdir / "lakitu_sprite_manifest.json"
            if lakitu_manifest_file.is_file():
                try:
                    lakitu_manifest = json.loads(lakitu_manifest_file.read_text(encoding="utf-8"))
                except Exception:
                    pass

    cache.clear()
    if a.no_generated:
        generated_count, generated_skipped = 0, 0
        asset_json_extra, asset_json_skipped = [], []
    else:
        generated_count, generated_skipped, asset_json_extra, asset_json_skipped = extract_generated_textures(
            root, rom, outdir, cache, asset_json_symbols, recipes_by_symbol, overwrite=overwrite
        )

    # Primeira execução: agora que o TKMK00 acabou de ser extraído da ROM,
    # faça a cópia protegida que será usada em todas as futuras execuções.
    # Nunca substitua uma cópia já existente.
    tkmk_manifest = outdir / "texture_tkmk00_manifest.json"
    if tkmk_manifest.is_file():
        try:
            tkmk_items = json.loads(tkmk_manifest.read_text(encoding="utf-8"))
        except Exception:
            tkmk_items = []
        for item in tkmk_items:
            rel = item.get("png")
            if not rel:
                continue
            src = outdir / rel
            dst = outdir / "originais" / rel
            if src.is_file() and not dst.is_file():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)

    alias_report = {"removed": [], "kept": []}
    if not a.no_generated:
        alias_report = canonicalize_common_hash_aliases(outdir)
        print(f"Aliases HASH duplicados removidos com prova exata: {len(alias_report.get('removed', []))}")
        print(f"Aliases HASH sem prova exata preservados: {len(alias_report.get('kept_hash_aliases', []))}")

    print(f"\nOK    : {ok} texturas comuns -> {outdir}")
    print(f"Karts : {kart_count} sprites piloto+kart -> {outdir / 'karts'}")
    if not a.no_lakitu:
        print(f"Lakitu: {lakitu_count} quadros do Lakitu -> {outdir / 'lakitu'}")
        if por_cat:
            for cat in sorted(por_cat):
                print(f"        * {cat:<20} {por_cat[cat]:>3} quadros")
        if lakitu_skipped > 0:
            print(f"        ! {lakitu_skipped} quadros do Lakitu ignorados")
        fora = procurar_lakitu_fora_dos_jsons(root, lakitu_manifest)
        if fora:
            print(f"        ! {len(fora)} simbolo(s) com 'lakitu' fora dos JSONs (sem dimensoes declaradas):")
            for tok, arq in fora[:10]:
                print(f"          - {tok} ({arq})")

    print(f"Bancos: {generated_count} texturas verificadas -> {outdir / 'generated'}")
    print(f"Assets JSON extras: {len(asset_json_extra)} texturas novas verificadas -> {outdir / 'generated' / 'asset_json'}")
    # Os frames foram gerados dentro de extract_generated_textures(); o main
    # apenas lê o manifesto produzido para exibir o resumo.
    rainbow_manifest_path = outdir / "rainbow_road_neon_frames_manifest.json"
    rainbow_skipped_path = outdir / "rainbow_road_neon_frames_skipped.json"
    try:
        rainbow_frames_report = json.loads(rainbow_manifest_path.read_text(encoding="utf-8")) if rainbow_manifest_path.is_file() else []
    except Exception:
        rainbow_frames_report = []
    try:
        rainbow_frame_skipped_report = json.loads(rainbow_skipped_path.read_text(encoding="utf-8")) if rainbow_skipped_path.is_file() else []
    except Exception:
        rainbow_frame_skipped_report = []
    print(f"Rainbow Road neon: {len(rainbow_frames_report)} frames visuais extraídos -> {outdir / 'generated' / 'asset_json' / 'rainbow_road' / 'rainbow_road' / 'frames'}")
    if rainbow_frame_skipped_report:
        print(f"  ! Frames neon ignorados: {len(rainbow_frame_skipped_report)} (motivos em rainbow_road_neon_frames_skipped.json)")
    if asset_json_skipped:
        print(f"  ! Assets JSON ignorados: {len(asset_json_skipped)} (motivos em asset_json_texture_skipped.json)")
    runtime_manifest_path = outdir / "runtime_derived_manifest.json"
    if runtime_manifest_path.is_file():
        try:
            runtime_items = json.loads(runtime_manifest_path.read_text(encoding="utf-8"))
            red_items = [x for x in runtime_items if x.get("derived_kind") == "red_shell"]
            print(f"Red Shell: {len(red_items)} variantes derivadas -> {outdir / 'generated'}")
        except Exception:
            pass
    print(f"  nao exportados: {generated_skipped} (motivos em generated_texture_skipped.json)")
    print(f"  do codigo-fonte:              {from_source}")
    print(f"  de metadata validada:         {from_metadata}")
    print(f"  de formato+tamanho:           {from_format_size}")
    print(f"  de override manual:           {from_override}")
    print(f"  adivinhadas (ultimo recurso): {guessed}")
    print(f"SKIP  : {skipped} (lista completa em texture_skip_list.txt)")
    if dims_report:
        print("Dims gravadas em texture_dims.json")
    if repack_recipes:
        print("Recipes gravadas em texture_repack_recipes.json")

    # Na primeira execução, sincronize somente os manifests que já existem.
    # O texture_tkmk00_tinted_manifest ainda não existe até a etapa HD.
    print("\nManifestos de runtime: sincronizando com extracted_textures...")
    runtime_copied = copy_runtime_manifests(root, outdir)
    print(f"Manifestos runtime copiados: {runtime_copied}")

    generated_tinted = ask_generate_hd_menu_backgrounds(root, outdir)

    # Se o usuário acabou de gerar os fundos HD, o tinted_manifest foi criado
    # agora em manifest_runtime e somente então deve aparecer em extracted_textures.
    runtime_copied = copy_runtime_manifests(root, outdir, include_tinted=bool(generated_tinted))
    print(f"Manifestos runtime sincronizados: {runtime_copied}")


if __name__ == "__main__":
    main()
