# Mudancas deste fork

Base: sirdankz/MKart360 (somente ROM US). Adiciona substituicao de texturas
HD em tempo de execucao, sem alterar a ROM nem os assets compilados.

## Codigo alterado
- `mk64-master/include/xbox360/gfx_pc.c` — gancho HD em `import_texture()`
  (hash FNV-1a do conteudo original), leitura do `tex.pak` com indice em
  tabela hash, fallback para `.tex` avulsos em subpastas (limite FATX),
  cache em RAM, cache de texturas ampliado (512 -> 1024), trace de
  diagnostico desligado (`X360_HDTEX_TRACE 0`).
  `x360_try_draw_hd_menu_quad` esta presente mas inativa (nao e chamada).
- `mk64-master/.gitignore` — ignora texturas extraidas, `tex.pak`, logs e
  `src/xbox360/generated_banks/` (derivados da ROM).

## Ferramentas novas (mk64-master)
- `EXTRACT_MK64_TEXTURES.py`, `EXTRACT_LAKITU.py` — ROM -> PNG + manifests
- `PACK_TEXTURES.py` — PNG -> `tex.pak`
- `HALVE_PNGS.py` — reduz PNGs pela metade
- `SCAN_HALVES.py`, `CROSS_CHECK.py` — diagnostico com o trace
- `SCAN_MENU.py` — mede os blocos das imagens grandes de menu
- `menu_tiles_geometry.json` — geometria medida dos blocos (so coordenadas)
- `TEXTURAS_HD_COMO_USAR.md` — guia de uso

## Bugs Conhecidos
- Por agora, nenhum

