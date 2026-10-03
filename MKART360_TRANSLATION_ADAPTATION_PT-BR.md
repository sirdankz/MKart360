# MKart360 — Adaptação para ROMs Traduzidas

## 1. O que eu estou adaptando

O MKart360 foi feito originalmente em torno da ROM USA:

```text
baserom.us.z64
```

Uma ROM traduzida possui bytes diferentes, então não basta trocar o arquivo da ROM.

Eu preciso:

- identificar a ROM;
- registrar sua identidade;
- permitir que o Xbox 360 a reconheça;
- regenerar os dados derivados da ROM;
- adaptar textos e caracteres quando necessário;
- verificar offsets e metadados;
- testar a ROM real.

## 2. Identificação da ROM

Eu não identifico uma ROM apenas pelo nome do arquivo.

Eu verifico:

```text
tamanho
CRC32
SHA-1
```

No meu exemplo de PT-BR:

```text
Size: 0xC00000
CRC32: 3B0D98C1
SHA-1: c2baf5b4a5355fff2dac08e971a62834ef70268c
MD5: d81760c53417ea97b04f311b02558aae
```

Para calcular o SHA-1:

```powershell
Get-FileHash ".\MyROM.z64" -Algorithm SHA1
```

Para CRC32, uso uma ferramenta ou um script que calcule o valor da ROM inteira.

## 3. Carregador do Xbox 360

O arquivo principal é:

```text
mk64-master/src/xbox360/xbox360_asset_loader.cpp
```

A ROM USA original usa:

```c
#define X360_EXPECTED_ROM_CRC 0x434389C1U
```

Na minha adaptação PT-BR, adicionei:

```c
#define X360_BR_ROM_CRC 0x3B0D98C1U
```

E o caminho passou de:

```text
game:\baserom.us.z64
```

para:

```text
game:\baserom.br.z64
```

A validação continua existindo. Agora ela aceita a ROM USA ou a ROM BR conhecida e rejeita uma ROM desconhecida.

## 4. Preparação dos assets

Também preciso alterar:

```text
mk64-master/PUBLIC_PREPARE_MK64_ASSETS.py
```

Para a ROM BR, registrei:

```python
BR_ROM_CRC32 = 0x3B0D98C1
BR_ROM_SHA1 = "c2baf5b4a5355fff2dac08e971a62834ef70268c"
```

O script identifica a variante pelo CRC32 e SHA-1.

Isso é importante porque os dados extraídos da ROM traduzida não precisam ser iguais aos dados da ROM USA.

Eu não removo as verificações. Eu adiciono uma variante conhecida.

## 5. Regeneração dos dados

Depois de registrar a ROM, uso a própria ROM traduzida para gerar os dados derivados:

```powershell
python PUBLIC_PREPARE_MK64_ASSETS.py --rom "baserom.br.z64"
```

O fluxo é:

```text
ROM traduzida
↓
extração
↓
dados derivados da ROM
↓
arquivos gerados
↓
compilação
```

Não é seguro simplesmente copiar os dados gerados para a ROM USA e esperar que sejam iguais.

## 6. Script de build

Também alterei:

```text
PUBLIC_BUILD_XBOX360.ps1
```

para procurar a ROM correta antes da preparação.

No meu caso:

```text
baserom.br.z64
```

pode ser escolhida antes de:

```text
baserom.us.z64
```

O nome do arquivo é apenas uma convenção. A identidade real continua sendo determinada pelos dados da ROM.

## 7. Textos traduzidos

Alguns textos ficam diretamente em:

```text
mk64-master/src/menu_items.c
```

Por isso, para uma tradução, eu preciso localizar os textos que ainda estão na versão original e substituir pelos textos da tradução.

Não basta editar o texto visualmente. Também preciso verificar como os caracteres são codificados.

## 8. Caracteres especiais e glifos

O jogo não trata esses caracteres simplesmente como UTF-8.

Para cada caractere especial, eu verifico:

```text
caractere
↓
bytes usados pela ROM
↓
glifo correspondente
```

Se o glifo já existir, reutilizo.

Se não existir, posso precisar:

```text
criar o glifo
↓
criar a MenuTexture
↓
adicionar à gGlyphTextureLUT[]
↓
alterar o decodificador
```

Na minha adaptação PT-BR, foram adicionados, por exemplo:

```text
x360_font_Atilde
x360_font_Eacute
```

Eu não copio essas mesmas regras automaticamente para outra tradução. Cada ROM precisa ser analisada.

## 9. Cuidado com `\x` no C

Preciso tomar cuidado com strings como:

```c
"\xA5DA"
```

porque a sequência hexadecimal pode continuar consumindo os caracteres seguintes.

Quando preciso separar os bytes, posso escrever:

```c
"\xA5" "DA"
```

O compilador concatena as duas strings.

## 10. Decodificador de caracteres

Adicionar um glifo não é suficiente.

O decodificador também precisa reconhecer os bytes usados pela tradução e apontar para o glifo correto.

Por isso, quando uma tradução usa novos caracteres, verifico:

```text
gGlyphTextureLUT[]
decodificador
sequências de bytes
MenuTexture
```

## 11. Nomes das pistas

Também verifico:

```text
mk64-master/assets/course_metadata/gCourseNames.inc.c
```

Os nomes das pistas podem precisar de tradução e de sequências especiais de bytes.

Portanto, a adaptação de texto não fica somente em `menu_items.c`.

## 12. Hash de integridade do código

O projeto possui:

```text
mk64-master/PUBLIC_SOURCE_GOLD_HASHES.json
```

Se eu alterar intencionalmente um arquivo que participa dessa verificação, atualizo o SHA-256 correspondente.

Por exemplo, para:

```text
src/xbox360/xbox360_asset_loader.cpp
```

o hash usado na minha adaptação passou de:

```text
1d21071ae526bc287f27097f68d211d082c3d9c21b5ea64d46be207384d478c8
```

para:

```text
70a427097d48d4d18d25e9aee54d79de32c643325803b9aec584efb562e88b0f
```

Para calcular um novo hash:

```powershell
Get-FileHash ".\src\xbox360\xbox360_asset_loader.cpp" -Algorithm SHA256
```

Eu não altero hashes de arquivos que não modifiquei.

## 13. Extração por região

Para ROMs de regiões diferentes, também uso:

```text
mk64-master/EXTRACT_MK64_TEXTURES.py
```

Posso indicar a região:

```powershell
py .\EXTRACT_MK64_TEXTURES.py --rom .\baserom.br.z64 --region br
```

ou:

```powershell
py .\EXTRACT_MK64_TEXTURES.py --rom .\baserom.us.z64 --region us
```

Os metadados regionais ficam em:

```text
mk64-master/yamls/<region>/
```

Por exemplo:

```text
yamls/us/
yamls/br/
```

Se não existir metadata específico para uma região, o projeto pode usar os metadados USA como fallback.

Eu trato esse fallback somente como compatibilidade. Ele não prova que os offsets USA estão corretos para outra ROM.

## 14. Offsets e TKMK00

Uma ROM diferente pode mover dados dentro da ROM.

Por isso, eu verifico:

```text
offsets
estruturas
tamanhos
dados derivados
TKMK00
```

Para TKMK00, os metadados podem ter offsets por região.

A lógica é:

```text
offset da região existe
↓
usa o offset da região

não existe
↓
usa USA como fallback
```

Se o TKMK00 tiver sido movido na ROM de destino, eu preciso corrigir o metadata antes de confiar na extração.

## 15. ROM oficial de outra região

Para uma ROM oficial de outra região, eu sigo praticamente o mesmo processo:

1. identifico tamanho, CRC32 e SHA-1;
2. registro a ROM no loader;
3. registro a ROM no script de preparação;
4. verifico os offsets;
5. ajusto os metadados se necessário;
6. regenero os assets;
7. compilo e testo.

Eu não renomeio uma ROM diferente para `baserom.us.z64` esperando que isso resolva a compatibilidade.

## 16. ROM traduzida ou modificada

Uma ROM traduzida ou modificada deve ser tratada como uma variante própria.

A lógica é:

```text
ROM original
↓
tradução/modificação
↓
nova ROM
↓
novo CRC32/SHA-1
↓
registrar a variante
↓
verificar estrutura
↓
regenerar os dados
```

Um CRC diferente não significa automaticamente que todos os offsets mudaram.

Mas, se os offsets ou estruturas mudaram, os metadados também precisam ser revisados.

## 17. Arquivos principais

```text
PUBLIC_BUILD_XBOX360.ps1

mk64-master/PUBLIC_PREPARE_MK64_ASSETS.py

mk64-master/src/xbox360/xbox360_asset_loader.cpp

mk64-master/PUBLIC_SOURCE_GOLD_HASHES.json

mk64-master/src/menu_items.c

mk64-master/assets/course_metadata/gCourseNames.inc.c

mk64-master/EXTRACT_MK64_TEXTURES.py

mk64-master/yamls/<region>/

mk64-master/PUBLIC_ASSET_RECIPES.json
```

## 18. Teste no Xbox 360

Depois do build, eu verifico pelo menos:

```text
jogo inicia
ROM é reconhecida
menu principal
seleção de personagem
seleção de copa
nomes das pistas
menus traduzidos
caracteres especiais
pause
opções
save
mensagens de erro
```

Os caracteres especiais merecem atenção porque um byte incorreto pode fazer o caractere aparecer errado ou desaparecer.

## 19. Fluxo para uma nova ROM

Quando adapto outra ROM, sigo esta ordem:

```text
1. identificar a ROM
2. registrar CRC32/SHA-1
3. registrar a variante no loader
4. registrar a variante na preparação dos assets
5. verificar offsets e metadados
6. verificar TKMK00
7. regenerar os assets
8. adaptar textos e glifos
9. adaptar nomes das pistas
10. atualizar hashes de integridade
11. compilar
12. testar no Xbox 360
```

Eu faço a análise antes de alterar os arquivos. Isso evita misturar problemas de ROM, tradução, extração e renderização HD.

## 20. Regra principal

A regra que eu mantenho é simples:

```text
ROM conhecida
→ aceita

ROM conhecida + metadados corretos
→ extrai e gera os assets

ROM desconhecida
→ não considero suportada até analisá-la
```

Assim eu consigo adaptar outras ROMs sem remover a proteção original do projeto e sem misturar a adaptação de ROM com o sistema separado de texturas HD.
