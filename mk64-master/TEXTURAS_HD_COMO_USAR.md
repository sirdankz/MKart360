# Texturas HD no MKart360 — guia de uso

Sistema de substituição de texturas em tempo de execução. O jogo calcula um
hash do conteúdo de cada textura original e, se existir uma versão HD com
aquele hash, usa ela no lugar — sem alterar a ROM nem os assets compilados.

Todos os comandos abaixo rodam de dentro de `mk64-master\`.

---

## Instalação (uma vez)

```powershell
pip install pillow
```

Os scripts ficam em `mk64-master\`:

| Arquivo | Função |
|---|---|
| `EXTRACT_MK64_TEXTURES.py` | ROM → PNGs editáveis |
| `PACK_TEXTURES.py` | PNGs editados → arquivo que o jogo lê |
| `HALVE_PNGS.py` | reduz PNGs pela metade (auxiliar) |

---

## Fluxo completo

### 1. Extrair

```powershell
py .\EXTRACT_MK64_TEXTURES.py --rom .\baserom.us.z64
```

Gera `extracted_textures\` com:

- **raiz** — texturas comuns, nomeadas `<hash>__nome.png`
- **`generated\`** — bancos gerados (menus, HUD), nomeadas pelo símbolo
- **`karts\<personagem>\frames\`** — sprites de piloto+kart (321 por personagem)
- **`*_manifest.json`** — metadados que ligam cada PNG ao seu hash

**Os arquivos `*_manifest.json` não podem ser apagados nem movidos.** É
neles que o empacotador descobre o hash de cada PNG que não tem o hash no
próprio nome.

Por padrão o extrator **não sobrescreve** PNGs já existentes, para não
apagar seu trabalho. Rodar de novo é seguro: ele só preenche o que falta e
atualiza os manifests. Para regerar tudo do zero, use `--force` (isso
descarta suas edições).

Outras opções: `--no-karts`, `--no-generated`, `--out PASTA`.

### 2. Editar

Abra os PNGs e faça upscale ou redesenhe. Pode mudar a resolução à vontade —
256×256 no lugar de um 64×64 original funciona.

**Não renomeie os arquivos.** O nome (ou o caminho registrado no manifest) é
o que liga a textura editada à original.

### 3. Empacotar

```powershell
py .\PACK_TEXTURES.py --only karts --pak
```

Gera `tex\tex.pak` — um arquivo único com tudo dentro.

| Opção | Efeito |
|---|---|
| `--pak` | gera um arquivo só (**recomendado**) |
| `--only PREFIXO` | limita a um subconjunto: `--only karts\bowser` |
| `--out PASTA` | pasta de saída (padrão `tex`) |
| `--max N` | ignora imagens maiores que N pixels (padrão 2048) |

Sem `--pak`, gera milhares de `.tex` avulsos em subpastas. Funciona, mas é
pior: mais lento para carregar e mais frágil para transferir. Use só para
depuração.

**Empacote apenas o que você realmente editou.** PNGs não editados geram
arquivos que substituem a textura por uma cópia idêntica — sem ganho visual,
mas ocupando espaço e memória.

### 4. Instalar no console

Copie para a **raiz da pasta do jogo**, ao lado do executável:

```
MK64.xex
baserom.us.z64
tex.pak          <- aqui, NÃO dentro de uma pasta tex\
```

Não precisa recompilar para trocar texturas: o `tex.pak` é lido em tempo de
execução. Recompilar só é necessário ao alterar código C.

---

## Limites de memória

O Xbox 360 tem 512 MB compartilhados entre sistema e vídeo, e o jogo já usa
parte disso. O elenco completo são 2568 sprites (2 arquivos cada):

| Resolução | Total | Observação |
|---|---|---|
| 256×256 | ~640 MB | não cabe |
| 128×128 | ~160 MB | recomendado |
| 96×96 | ~90 MB | folga maior |

Para reduzir PNGs já editados, coloque o `HALVE_PNGS.py` na pasta e rode:

```powershell
py .\HALVE_PNGS.py --recursive              # prévia, não altera nada
py .\HALVE_PNGS.py --apply --recursive      # aplica
```

Use `--backup` para guardar os originais como `*.orig.png`.

---

## Problemas comuns

**As texturas não aparecem.**
Confira se o `tex.pak` está na raiz, ao lado do `MK64.xex`, e não dentro de
`tex\`. Se estiver certo, force uma recompilação completa (`/t:Rebuild`) —
o `gfx_pc.c` é incluído por outro arquivo e a build incremental às vezes não
percebe a mudança.

**Só metade do sprite fica em HD.**
Sprites CI8 são carregados em duas janelas por causa do limite de 4 KB da
TMEM, cada uma com hash próprio. O extrator já trata isso. Se acontecer,
regenere os manifests com o extrator atualizado e reempacote.

**A transferência falha no fim.**
O sistema de arquivos do console (FATX) aceita no máximo 4096 arquivos por
pasta. É exatamente o que `--pak` resolve, gerando um arquivo só.

**Engasgos no jogo.**
Reduza a resolução (128×128) ou limite o número de personagens
substituídos. Uma travadinha quando um oponente novo entra em cena é
esperada: os sprites dele são carregados naquele momento.

---

## Diagnóstico

Quando algo não bate, há um trace no jogo. Em `include\xbox360\gfx_pc.c`,
troque:

```c
#define X360_HDTEX_TRACE 0   →   1
```

Recompile, jogue alguns segundos e pegue `game:\hdtex-trace.log`. Cada linha
traz o hash calculado em tempo de execução e `found=1` ou `found=0`,
mostrando se a textura HD foi encontrada.

Para cruzar esse log com os manifests:

```powershell
py .\CROSS_CHECK.py --log .\hdtex-trace.log --kart bowser
```

Deixe o trace desligado no uso normal: ele grava em disco durante o jogo.

---

## Como funciona

1. O jogo carrega uma textura e calcula um hash FNV-1a do conteúdo original.
2. O `gfx_pc.c` procura esse hash no `tex.pak`.
3. Se achar, envia a versão HD à GPU no lugar da original.
4. Se não achar, segue o caminho normal — nada quebra.

As dimensões originais são preservadas internamente para o mapeamento de
coordenadas, por isso a textura HD pode ter qualquer resolução. Os dados
lidos ficam num cache em RAM para evitar releituras do disco.
