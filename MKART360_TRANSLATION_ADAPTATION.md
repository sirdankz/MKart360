# MKart360 --- Adaptação para ROM traduzida / localização BR

## Objetivo

Este documento registra como adaptei o projeto MKart360 para funcionar
com a minha própria ROM de **Mario Kart 64 traduzida para português do
Brasil**.

A ideia é deixar o caminho documentado para quem quiser fazer o mesmo
com outra tradução não oficial: em vez de substituir a ROM traduzida por
uma ROM diferente, o projeto deve ser preparado para usar os bytes,
textos e assets derivados da ROM que a pessoa realmente pretende usar.

Durante o processo usei IA para alguns ajustes e para ajudar a localizar
erros que estavam dando bastante trabalho, principalmente na parte de
strings, codificação dos caracteres e montagem dos glyphs. As alterações
foram conferidas com os arquivos e com a ROM usada no projeto.

------------------------------------------------------------------------

## 1. Ponto de partida

O projeto originalmente estava preparado para a ROM USA.

A preparação de assets fazia auditoria da ROM esperada e utilizava os
dados da ROM USA como referência. O carregador da versão Xbox 360 também
estava configurado para procurar:

``` text
game:\baserom.us.z64
```

e validar o CRC32:

``` text
434389C1
```

Para usar uma tradução própria, isso precisava mudar porque a ROM
traduzida não tem os mesmos bytes da ROM USA.

No meu caso, a ROM usada foi:

``` text
Mario Kart 64 [BR].z64
```

------------------------------------------------------------------------

## 2. Identificação da ROM BR

A ROM utilizada foi verificada antes de alterar o projeto.

Dados da ROM:

``` text
Tamanho:  0xC00000
          12 MiB

CRC32:    3B0D98C1

SHA-1:    c2baf5b4a5355fff2dac08e971a62834ef70268c

MD5:      d81760c53417ea97b04f311b02558aae

Formato:  .z64 / big-endian
```

Os primeiros bytes são:

``` text
80 37 12 40 00 00 00 0F 80 00 04 00 00 00 14 46
```

O CRC32 e o SHA-1 servem para identificar exatamente a ROM que foi usada
durante o desenvolvimento.

Isso não significa que qualquer ROM com o mesmo tamanho será compatível.
O CRC identifica a imagem específica; a compatibilidade do código e dos
assets ainda precisa ser testada.

------------------------------------------------------------------------

# 3. Alteração da preparação dos assets

## Antes

O script de preparação aceitava a ROM USA e verificava os hashes
esperados do projeto.

A configuração original trabalhava com:

``` text
baserom.us.z64
```

e com o CRC32:

``` text
434389C1
```

## Depois

A preparação foi adaptada para aceitar também a ROM BR:

``` text
baserom.br.z64
```

com:

``` text
CRC32 = 3B0D98C1
SHA-1 = c2baf5b4a5355fff2dac08e971a62834ef70268c
```

O script passou a reconhecer a variante BR e a gerar novamente os assets
derivados da ROM que está sendo utilizada.

A execução com a minha ROM apresentou:

``` text
MK64 public-source asset preparation
========================================================================
ROM CRC32: 3B0D98C1 [BR variant accepted]
ROM SHA1:  c2baf5b4a5355fff2dac08e971a62834ef70268c [BR variant accepted]
Media fragments to regenerate: 627
Generated-bank objects:         3736

SUCCESS
  regenerated 627 excluded media source fragments
  regenerated 13 generated-bank C files / 3736 objects
  regenerated 20 course linkonly C/header pairs
```

A preparação também manteve a proteção de integridade do projeto. Quando
um arquivo de código autorizado foi alterado, o hash correspondente
precisou ser atualizado na lista de hashes autorizados.

------------------------------------------------------------------------

# 4. Alteração do carregador do Xbox 360

O carregador original do port estava preso à ROM USA.

## Antes

A lógica original era equivalente a:

``` c
#define X360_EXPECTED_ROM_CRC 0x434389C1U

#ifndef X360_ROM_PATH
#define X360_ROM_PATH "game:\\baserom.us.z64"
#endif
```

E a validação recusava qualquer CRC diferente da ROM USA.

## Depois

O carregador passou a aceitar tanto a ROM USA quanto a ROM BR:

``` c
#define X360_EXPECTED_ROM_CRC 0x434389C1U
#define X360_BR_ROM_CRC       0x3B0D98C1U
```

O caminho utilizado para a versão BR ficou:

``` c
#ifndef X360_ROM_PATH
#define X360_ROM_PATH "game:\\baserom.br.z64"
#endif
```

Depois da leitura da ROM, o CRC é calculado e comparado com os valores
aceitos.

A lógica ficou essencialmente:

``` c
unsigned int finalCrc = (crc ^ 0xFFFFFFFFU);

if (finalCrc != X360_EXPECTED_ROM_CRC &&
    finalCrc != X360_BR_ROM_CRC) {
    x360_log("MK64: unsupported or corrupt local ROM\n");
    return 0;
}

romReady = true;

if (finalCrc == X360_BR_ROM_CRC)
    x360_log("MK64: local BR ROM validated\n");
else
    x360_log("MK64: local US ROM validated\n");
```

Isso evita que o port simplesmente aceite qualquer arquivo com tamanho
correto.

------------------------------------------------------------------------

# 5. Por que os assets precisam ser regenerados

Uma ROM traduzida pode alterar muito mais do que as strings escritas
diretamente em um arquivo `.c`.

Dependendo da tradução, podem existir alterações em:

-   textos;
-   tabelas;
-   fontes;
-   caracteres;
-   imagens;
-   dados referenciados pelo jogo;
-   posições ou conteúdos derivados da ROM.

Por isso, depois de colocar a ROM BR no projeto, os assets derivados da
ROM foram regenerados.

O procedimento utilizado foi:

``` powershell
python PUBLIC_PREPARE_MK64_ASSETS.py --rom baserom.br.z64
```

O script então trabalhou sobre a ROM BR, em vez de simplesmente
reutilizar os assets preparados para a ROM USA.

------------------------------------------------------------------------

# 6. O problema específico dos textos do menu

Depois que a ROM BR passou a ser utilizada pelo projeto, apareceu uma
diferença importante.

No emulador, usando a ROM traduzida, o menu de opções aparecia em
português.

No Xbox 360, porém, algumas dessas mensagens continuavam em inglês.

A causa foi encontrada em `src/menu_items.c`.

Havia strings fixas no código, por exemplo:

``` c
char* gTextOptionMenu[] = {
    "RETURN TO GAME SELECT",
    "SOUND MODE",
    "COPY N64 CONTROLLER PAK",
    "ERASE ALL DATA",
};
```

Ou seja, essas mensagens não estavam sendo obtidas automaticamente da
ROM BR durante a execução do port. Elas estavam compiladas diretamente
no código C.

------------------------------------------------------------------------

# 7. Uso da codificação da ROM

Um dos pontos mais importantes foi descobrir que não era para colocar
UTF-8 nas strings.

A ROM BR utiliza uma codificação própria compatível com o sistema de
glyphs do jogo.

Por exemplo, a ROM contém sequências como:

``` text
RETORNAR A4 A1 SELE A4 C3 A4 E3 O DE JOGO
```

e outras combinações para caracteres acentuados.

No código C, essas sequências precisam ser representadas pelos bytes
correspondentes:

``` c
"RETORNAR \xA4\xA1 SELE\xA4\xC3\xA4\xE3O DE JOGO"
```

Portanto, simplesmente escrever:

``` c
"RETORNAR À SELEÇÃO DE JOGO"
```

ou converter tudo para UTF-8 não reproduz a codificação usada pelo jogo.

------------------------------------------------------------------------

# 8. Primeira correção das strings

O bloco de opções e várias mensagens relacionadas a Controller Pak,
saves e ghosts foram substituídos pelas strings correspondentes
encontradas na ROM BR.

A verificação feita durante o processo comparou os bytes das strings
modificadas com as strings encontradas na ROM.

Resultado:

``` text
source nonempty: 106
rom nonempty:    196
matches:         106
```

Isso significa que todas as 106 strings não vazias alteradas naquele
bloco tinham correspondência byte a byte com a ROM BR.

------------------------------------------------------------------------

# 9. Problema causado por `\x`

Depois apareceu um erro de compilação no MSVC:

``` text
error C2022: '42458' : too big for character
```

O problema estava em uma string como:

``` c
"C\xA5\xA5PIA DE DADOS CONCLU\xA4\xA5DA",
```

Em C, um escape hexadecimal `\x` continua consumindo caracteres
hexadecimais seguintes.

Assim:

``` text
\xA5DA
```

não significa:

``` text
\xA5 + "DA"
```

O compilador pode interpretar como um único valor hexadecimal muito
maior que um caractere.

## Correção

A string foi separada:

``` c
"C\xA5\xA5PIA DE DADOS CONCLU\xA4\xA5" "DA",
```

O resultado em bytes continua sendo:

``` text
43 A5 A5 50 49 41 20 44 45 20 44 41 44 4F 53
20 43 4F 4E 43 4C 55 A4 A5 44 41
```

Esse byte sequence também foi localizado na ROM BR.

Depois foi feita uma varredura nos arquivos `.c` para procurar outros
escapes hexadecimais ambíguos.

Resultado:

``` text
remaining ambiguous hex escapes: 0
```

Também foi realizado um teste de sintaxe com Clang:

``` text
clang -std=c11 -Wall -Wextra -Werror -fsyntax-only
```

Resultado:

``` text
C syntax test exit=0
```

------------------------------------------------------------------------

# 10. Problema dos glyphs acentuados

Mesmo depois das strings estarem corretas, o Xbox 360 ainda apresentava
caracteres estranhos.

O problema apareceu principalmente em:

``` text
Ã
É
```

Por exemplo:

``` text
BOTÃO
SELEÇÃO
ESTÉREO
```

A ROM possuía os bytes corretos, mas o renderer do port Xbox 360 não
estava tratando esses caracteres da mesma maneira que a ROM original.

A investigação mostrou que o problema estava no mapeamento dos glyphs.

Não bastava apenas colocar o byte correto na string.

------------------------------------------------------------------------

# 11. Correção dos glyphs

Foram adicionados glyphs específicos para os caracteres necessários e
ajustada a tabela usada pelo renderer.

Durante uma das tentativas, os índices apontados estavam errados. O
resultado foi que os caracteres simplesmente desapareceram:

``` text
BOT O A
SELEÇ O
EST REO
```

Isso mostrou que o problema não era mais a string: o código estava
chamando os índices incorretos da tabela.

A correção final fez os códigos utilizados pelo renderer apontarem para
os índices corretos dos novos glyphs.

Depois disso, o resultado no Xbox 360 passou a ficar correto.

------------------------------------------------------------------------

# 12. Resultado final do menu

Depois das correções, as telas passaram a apresentar corretamente:

``` text
BOTÃO A • VER DADOS    BOTÃO B • SAIR
```

e:

``` text
RETORNAR A SELEÇÃO DE JOGO

MODO DE SOM       ESTÉREO
```

Além das outras mensagens traduzidas que já estavam funcionando.

Essa foi a última etapa necessária para corrigir os caracteres
acentuados do menu que estavam causando problemas.

------------------------------------------------------------------------

# 13. Como adaptar outra tradução não oficial

Este é o procedimento que deve ser repetido para outra ROM traduzida.

## Passo 1 --- Use a ROM da própria tradução

Coloque a ROM traduzida no projeto, por exemplo:

``` text
baserom.br.z64
```

Não é necessário alterar a ROM para que ela fique igual à ROM USA.

O objetivo é fazer o port trabalhar com a ROM que contém a tradução
desejada.

------------------------------------------------------------------------

## Passo 2 --- Calcule os identificadores da ROM

Obtenha:

``` text
Tamanho
CRC32
SHA-1
```

Por exemplo:

``` text
Tamanho: 0xC00000
CRC32:   3B0D98C1
SHA-1:   c2baf5b4a5355fff2dac08e971a62834ef70268c
```

Para uma tradução diferente, os valores provavelmente serão diferentes.

------------------------------------------------------------------------

## Passo 3 --- Adicione a nova ROM ao sistema de preparação

No `PUBLIC_PREPARE_MK64_ASSETS.py`, acrescente a identificação da nova
ROM.

A ideia é ter algo equivalente a:

``` text
ROM USA
ROM BR
ROM da nova tradução
```

Cada uma deve ter seus próprios identificadores.

Não recomendo simplesmente remover a verificação de hash. É melhor
adicionar a nova ROM como uma variante autorizada.

------------------------------------------------------------------------

## Passo 4 --- Ajuste o caminho da ROM do Xbox 360

Se a nova variante usar um arquivo próprio, ajuste:

``` c
#define X360_ROM_PATH "game:\\baserom.br.z64"
```

para o nome utilizado pela nova ROM.

Também acrescente o CRC da nova ROM à validação.

------------------------------------------------------------------------

## Passo 5 --- Regenere os assets

Execute:

``` powershell
python PUBLIC_PREPARE_MK64_ASSETS.py --rom baserom.br.z64
```

Para outra tradução:

``` powershell
python PUBLIC_PREPARE_MK64_ASSETS.py --rom baserom.novatraducao.z64
```

A ideia é sempre gerar os assets usando a ROM real da tradução.

------------------------------------------------------------------------

## Passo 6 --- Procure textos que continuam vindo do código

Se uma mensagem aparece traduzida no emulador, mas aparece em inglês no
Xbox 360, procure primeiro por essa string nos arquivos `.c`.

Por exemplo:

``` c
"RETURN TO GAME SELECT"
```

Se ela estiver diretamente no código, o port pode estar usando essa
versão em vez do texto existente na ROM.

Nesse caso, substitua pela representação em bytes utilizada pela ROM
traduzida.

------------------------------------------------------------------------

## Passo 7 --- Não use UTF-8 sem verificar a fonte

Antes de alterar uma string com caracteres especiais, descubra como a
ROM representa aquele caractere.

Por exemplo:

``` text
Ã
É
Ç
Á
À
```

Cada tradução pode ter uma tabela de caracteres diferente.

A regra utilizada neste projeto foi:

> **copiar os bytes da própria ROM traduzida e reproduzi-los no código
> C.**

Isso é muito mais seguro do que tentar converter a string para UTF-8.

------------------------------------------------------------------------

## Passo 8 --- Verifique os glyphs

Se o byte está correto, mas o caractere aparece:

-   como símbolo;
-   como espaço;
-   deformado;
-   ou simplesmente desaparece;

então o próximo lugar para investigar é o mapeamento dos glyphs.

Neste projeto, `Ã` e `É` exigiram uma correção específica na tabela de
glyphs do port Xbox 360.

------------------------------------------------------------------------

## Passo 9 --- Cuidado com escapes `\x`

Sempre que existir algo como:

``` c
"\xA5DA"
```

verifique se o compilador não está interpretando tudo como um único
escape.

Quando necessário, separe a string:

``` c
"\xA5" "DA"
```

Isso mantém os bytes corretos e evita erros do MSVC como:

``` text
C2022: too big for character
```

------------------------------------------------------------------------

## Passo 10 --- Atualize os hashes de integridade somente para alterações intencionais

O projeto possui verificações de integridade dos arquivos-fonte.

Se uma alteração legítima for feita em um arquivo protegido e o build
acusar algo como:

``` text
working 2-player gold source was changed
```

não é recomendado simplesmente desativar a proteção.

O correto é:

1.  conferir a alteração;
2.  confirmar que ela é intencional;
3.  atualizar o hash autorizado correspondente;
4.  executar novamente a preparação/build.

Foi exatamente o que foi feito com:

``` text
src/xbox360/xbox360_asset_loader.cpp
```

------------------------------------------------------------------------

# 14. Build

Depois de preparar os assets, o projeto pode ser compilado usando:

``` powershell
.\PUBLIC_BUILD_XBOX360.ps1
```

O ambiente utilizado para preparar e verificar os arquivos não possui o
toolchain Xbox 360/MSVC necessário para executar o build final do XEX.

Por isso, o build final deve ser realizado em um ambiente que tenha as
ferramentas necessárias para o projeto Xbox 360.

------------------------------------------------------------------------

# 15. O que não deve ser feito

Durante este processo algumas abordagens foram descartadas.

### Não substituir a ROM traduzida por outra ROM

O objetivo é justamente usar a ROM da tradução que está sendo adaptada.

### Não converter as strings para UTF-8 sem verificar a ROM

A codificação usada pelo jogo não deve ser presumida.

### Não remover todas as verificações de integridade

As verificações existem para evitar builds baseados em arquivos
alterados acidentalmente.

### Não alterar os bytes dos glyphs sem verificar a tabela

Um glyph correto colocado no índice errado pode fazer a letra
desaparecer.

### Não assumir que todo texto do jogo está em `menu_items.c`

Existem textos provenientes de diferentes lugares:

``` text
ROM
assets derivados da ROM
strings compiladas no código
texturas
glyphs/fontes
```

Cada caso precisa ser investigado separadamente.

------------------------------------------------------------------------

# 16. Resumo das alterações realizadas

## ROM

``` text
ROM usada:
Mario Kart 64 [BR].z64

CRC32:
3B0D98C1

SHA-1:
c2baf5b4a5355fff2dac08e971a62834ef70268c
```

## Preparação

``` text
PUBLIC_PREPARE_MK64_ASSETS.py
```

Foi adaptado para reconhecer a ROM BR e regenerar os assets derivados
dela.

## Xbox 360

``` text
src/xbox360/xbox360_asset_loader.cpp
```

Foi adaptado para reconhecer a ROM BR e informar no log que a ROM BR foi
validada.

## Textos

``` text
src/menu_items.c
```

Foram corrigidas as strings do menu de opções e outras mensagens
relacionadas a saves, Controller Pak e ghosts.

## Codificação

Os textos traduzidos foram representados usando os bytes da própria ROM,
em vez de UTF-8.

## Glyphs

Foram adicionados/corrigidos glyphs necessários para os caracteres
acentuados utilizados no menu.

## Integridade

Os hashes de arquivos modificados intencionalmente foram atualizados sem
desativar o mecanismo de proteção.

## Verificações

Foram realizadas:

``` text
comparação byte a byte das strings
verificação dos bytes na ROM
varredura de escapes hexadecimais ambíguos
teste de sintaxe C
teste de integridade do ZIP
```

------------------------------------------------------------------------

# 17. Considerações para futuras traduções

A adaptação de outra tradução deve ser tratada como uma nova variante de
ROM, e não simplesmente como uma troca de idioma.

Uma tradução pode alterar:

-   comprimento das strings;
-   caracteres disponíveis;
-   fonte;
-   glyphs;
-   assets;
-   dados internos da ROM.

Por isso, o primeiro passo deve ser sempre identificar a ROM e depois
verificar quais partes do port dependem dos bytes originais da ROM USA.

A experiência desta adaptação mostrou que o trabalho pode ser dividido
em três partes:

``` text
1. ROM / assets
       ↓
2. strings e codificação
       ↓
3. renderer / glyphs do Xbox 360
```

Se uma tradução funciona no emulador mas não no port Xbox 360, essas
três camadas devem ser verificadas separadamente.

------------------------------------------------------------------------

## Histórico resumido das correções

### v5

-   Adaptação inicial para a ROM BR.
-   Regeneração dos assets.
-   Correção das strings do menu de opções.
-   Verificação byte a byte das strings.

### v6

-   Correção de um `\xA5DA` que causava erro C2022 no MSVC.
-   Verificação de escapes hexadecimais ambíguos.

### v7

-   Correção adicional das strings `DADOS` e do modo de som.
-   Ajustes iniciais para caracteres acentuados.

### v8

-   Tentativa de adicionar glyphs específicos para `Ã` e `É`.
-   Os índices utilizados inicialmente estavam incorretos e os
    caracteres acabavam desaparecendo.

### v9

-   Correção final dos índices dos glyphs.
-   `Ã` e `É` passaram a ser desenhados corretamente.
-   Menu BR funcionando corretamente no Xbox 360.

------------------------------------------------------------------------

## Estado final

A adaptação BR chegou ao ponto em que:

-   a ROM BR é reconhecida pelo projeto;
-   os assets podem ser regenerados a partir da ROM BR;
-   o carregador Xbox 360 aceita a ROM BR;
-   as strings do menu de opções foram corrigidas;
-   os caracteres acentuados necessários foram tratados no renderer;
-   o erro de escape hexadecimal foi corrigido;
-   as verificações de integridade continuam ativas;
-   o menu de opções passou a apresentar a tradução corretamente no Xbox
    360.

Esta documentação foi escrita para registrar o processo de adaptação e
servir como referência para outras traduções não oficiais, sem depender
de uma ROM específica além da identificação e dos ajustes necessários
para cada variante.
