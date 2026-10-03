# MKart360 — Texturas HD

## 1. Divisão do trabalho

Antes da parte técnica, quero deixar claro o que foi feito por mim e pelo Felipe.

O `PACK_TEXTURES.py` foi feito pelo Felipe. Eu apenas fiz alguns ajustes nele para adaptar algumas coisas ao funcionamento do projeto.

O script de 60 FPS também foi feito pelo Felipe. Eu somente mexi em algumas partes dele quando foi necessário para o projeto.

O arquivo `menu_tiles_geometry.json` foi feito inteiramente pelo Felipe. A geometria do menu é trabalho dele.

Meu trabalho específico foi o extrator, o descompressor e os dois manifests de runtime:

```text
texture_tkmk00_runtime_manifest.json
texture_tkmk00_names_runtime_manifest.json
```

Todo o restante foi trabalho em equipe entre mim e o Felipe, incluindo os outros ajustes, testes, análise, integração e o funcionamento das diferentes partes.

## 3. O que eu alterei
Este arquivo documenta somente a parte de texturas HD do MKart360.

Eu mantive separado o trabalho de ROM traduzida, região e textos. Aqui entram:

- identificação por hash;
- carregamento HD;
- `.tex`;
- `tex.pak`;
- manifests;
- sprites;
- imagens de menu;
- TMEM;
- cache;
- TKMK00;
- organização final para o Xbox 360.

## 3. Identificação das texturas

A textura original é identificada usando FNV-1a de 32 bits.

O fluxo básico é:

```text
textura original
    ↓
FNV-1a 32 bits
    ↓
hash
    ↓
procura da textura HD
```

Um exemplo de hash é:

```text
79fcbc0f
```

O mesmo identificador é usado pelo sistema de preparação e pelo código do Xbox 360.

A textura HD fica fora da ROM. Se não existir uma substituição, o jogo continua usando a textura original.

## 4. `gfx_pc.c`

O principal arquivo do carregamento HD é:

```text
mk64-master/include/xbox360/gfx_pc.c
```

Ele identifica a textura, procura a versão HD, lê os dados, envia para a GPU e mantém informações em cache.

O buffer HD compartilhado é:

```c
static uint8_t x360_hd_buf[2048 * 2048 * 4];
```

## 5. De `.tex` para `tex.pak`

A primeira versão usava arquivos `.tex` separados:

```text
game:\tex\<prefix>\<hash>.tex
```

Depois eu passei a usar:

```powershell
py .\PACK_TEXTURES.py --pak
```

O resultado principal é:

```text
tex.pak
```

O PAK possui índice e dados das texturas. O índice guarda informações como:

```text
hash
offset
tamanho
largura
altura
```

No Xbox 360, `gfx_pc.c` usa:

```c
x360_pak_read()
```

para localizar a textura pelo hash.

O `.tex` antigo pode continuar sendo usado como compatibilidade, mas o `tex.pak` é o caminho principal.

## 6. Manifests

Eu uso manifests para separar as diferentes categorias de textura.

Os principais são:

```text
generated_texture_manifest.json
kart_sprite_manifest.json
lakitu_sprite_manifest.json
asset_sprite_manifest.json
texture_tkmk00_manifest.json
```

Eles registram a relação entre o PNG extraído e os dados necessários para preparar a textura.

Para texturas carregadas em partes, também podem existir:

```text
tmem_halves
tmem_tiles
```

Isso permite reproduzir a forma como o jogo realmente carregou a textura, em vez de simplesmente cortar a imagem em uma grade genérica.

## 7. Imagens grandes de menu

Algumas imagens de menu são carregadas pelo jogo em várias partes de TMEM.

Para os casos em que é possível usar a imagem completa, o código possui:

```c
x360_try_draw_hd_menu_quad()
```

Também uso:

```text
menu_items.c
menu_tiles_geometry.json
```

A geometria de algumas imagens foi registrada manualmente porque representa melhor os carregamentos reais do jogo.

## 8. Cache

O cache evita ler e enviar a mesma textura HD para a GPU repetidamente.

Também existe um cache específico para imagens de menu.

Na base HD que eu validei, a capacidade do cache de menu foi aumentada para:

```c
#define X360_HDMENU_MAX 512
```

Essa base deve ser preservada antes de fazer novas alterações.

O cache geral também pode usar o conteúdo da textura para diferenciar situações em que o mesmo endereço de RAM recebe dados diferentes.

Para texturas CI, a identidade pode considerar os índices e a paleta.

As leituras usadas para esse tipo de hash possuem limites, como:

```text
size > 0
size <= 8192
line_size <= 8192
```

## 9. Deduplicação e colisões

O `PACK_TEXTURES.py` também verifica duplicatas.

Se duas entradas possuem o mesmo hash, dimensões e bytes, elas podem compartilhar os mesmos dados.

Como FNV-1a tem apenas 32 bits, também existe a possibilidade de colisão.

Quando o empacotador encontra:

```text
mesmo hash
+
dados diferentes
```

ele registra a situação para não deixar uma substituição silenciosa.

## 10. Limite de tamanho

O empacotador possui:

```text
--max
```

O limite usado é:

```text
2048
```

Uma textura que ultrapasse esse limite em largura ou altura não é incluída automaticamente.

## 11. TKMK00

TKMK00 é tratado separadamente porque algumas imagens são montadas e carregadas em partes durante o runtime.

O manifest principal é:

```text
texture_tkmk00_manifest.json
```

Também foram criados:

```text
texture_tkmk00_runtime_manifest.json
texture_tkmk00_names_runtime_manifest.json
```

Esses manifests registram dados observados no Xbox 360, como:

```text
width
height
tile_width
tile_height
TMEM
posição
hash
logical_hash
```

O ponto importante é que o hash do bloco realmente lido pelo runtime pode ser diferente do hash lógico da imagem.

### Nomes dos personagens

Um exemplo é uma imagem registrada logicamente como:

```text
64 × 12
```

mas carregada no runtime como:

```text
64 × 13
```

Isso muda o número de bytes usados no hash.

Por isso, o código tenta primeiro o hash normal. Se não encontrar, pode tentar o hash lógico.

Essa segunda tentativa foi feita para não alterar o comportamento das texturas que já funcionavam.

### Textura `OK`

A textura:

```text
texture_ok
```

tem hash lógico:

```text
dff91b13
```

No runtime foram observadas duas partes com:

```text
35d069ff
154196f3
```

Nesse caso, o código pode localizar a textura completa e recortar a região necessária.

Eu não transformei todo TKMK00 em um caminho genérico de imagem completa. Os recursos que dependem das divisões originais de TMEM continuam usando esse caminho.

## 12. Debug

Durante a investigação, usei trace temporário no `gfx_pc.c` para descobrir:

```text
formato
siz
largura
altura
tamanho lido
hash
pitch
posição
tile
resultado da procura HD
```

Isso serviu para descobrir o comportamento real do runtime.

Na versão final, o debug de textura fica desligado:

```c
#define X360_HDTEX_TRACE 0
```

O debug é ferramenta de desenvolvimento, não parte do funcionamento final.

## 13. O que é automático e o que é manual

Automático:

- extração das texturas comuns;
- geração dos manifests comuns;
- empacotamento do `tex.pak`.

Manual:

- descoberta dos dados de runtime do TKMK00;
- `texture_tkmk00_runtime_manifest.json`;
- `texture_tkmk00_names_runtime_manifest.json`;
- `menu_tiles_geometry.json`;
- edição das texturas HD;
- teste final no Xbox 360.

Os manifests manuais ficam porque funcionam como uma ponte entre os dados descobertos no runtime e o processo offline de empacotamento.

## 14. Arquivos principais

```text
mk64-master/include/xbox360/gfx_pc.c
mk64-master/src/menu_items.c
mk64-master/PACK_TEXTURES.py
mk64-master/EXTRACT_MK64_TEXTURES.py
mk64-master/menu_tiles_geometry.json

mk64-master/extracted_textures/generated_texture_manifest.json
mk64-master/extracted_textures/kart_sprite_manifest.json
mk64-master/extracted_textures/lakitu_sprite_manifest.json
mk64-master/extracted_textures/asset_sprite_manifest.json
mk64-master/extracted_textures/texture_tkmk00_manifest.json
mk64-master/extracted_textures/texture_tkmk00_runtime_manifest.json
mk64-master/extracted_textures/texture_tkmk00_names_runtime_manifest.json
```

## 15. Fluxo que eu uso

```text
ROM
↓
EXTRACT_MK64_TEXTURES.py
↓
extracted_textures
↓
manifests
↓
edição dos PNGs
↓
PACK_TEXTURES.py --pak
↓
tex.pak
↓
Xbox 360
↓
gfx_pc.c
↓
hash original
↓
x360_pak_read()
↓
cache
↓
GPU
```

Para continuar o desenvolvimento, eu parto da base funcional já validada e faço uma alteração por vez, gerando o PAK e testando no Xbox 360 antes da próxima mudança.

## 16. Organização final

A ROM não faz parte do repositório.

Os arquivos gerados durante o processo, como `extracted_textures/` e `tex/`, também não precisam ser distribuídos como parte da base inicial.

O que precisa ficar registrado são o código, os manifests manuais necessários e os arquivos de configuração que permitem reproduzir o processo.

## 17. Resultado

O sistema final separa as texturas HD dos dados originais da ROM.

O principal contêiner é:

```text
tex.pak
```

e o carregamento passa por:

```text
hash → PAK → cache → GPU
```

O TKMK00 continua documentado como uma categoria especial, com os dados de runtime necessários para reproduzir os casos que não podem ser tratados somente pela extração offline.
