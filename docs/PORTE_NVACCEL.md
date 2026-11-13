# Porte do NVAccel (NullMoth) para o RM r387: plano

Data: 2026-11-07. Base: `ref/nullmoth/kexts/NVRM/accel` e `FASE6_ESTUDO.md`. Nada instalado nesta etapa.

## O que é o NVAccel
- `nvrm-accel.cpp` (~3000 linhas): `NVAccel : IOGraphicsAccelerator2` para a IOAcceleratorFamily2 **do Tahoe**
  (cabeçalhos próprios em `iofam/`, slots que a Apple não exporta mais repassados por `tahoe_fwd.h`), com
  `NVEventMachine`, `NVDisplayMachine : IOAccelLegacyDisplayMachine`, `NVTask`, `NVMemoryMap`.
- Os user clients de Metal são os da família (o plugin já funciona sobre eles: provado com o nvAccelerator).
- Backend: `callPlatformFunction` para o RM deles (`NVRM`) e para o framebuffer deles (`NVRMFB`, que usa o NVKMS/KAPI).

## Backend a reimplementar sobre o r387

| Função do NullMoth | Uso | No r387 |
|---|---|---|
| `nvNewAccelClient` | user client do RM para o NVK | **feito**: ponte do kext 0.2.0 |
| `nvRmHandles` | hClient/hDevice/hSubDevice do acelerador | NvRmAllocRoot/NvRmAlloc no kernel (API exportada pelo NVDAResmanWeb) |
| `nvAllocVram` / `nvFreeVram` | VRAM das superfícies + mapa no kernel (kva, BAR1) | NVOS32 físico + MAP_MEMORY (receita da libnvrm387) |
| `nvGpuVaAlloc/Map/Free` | VA da GPU das superfícies | virtual + MAP_MEMORY_DMA (receita da libnvrm387) |
| `nvScanoutInfo` | onde fica a tela (phys/kva/pitch/tamanho) | framebuffer `NVDA` (IOFramebuffer do web driver) |
| `nvFlipToSurface` | trocar a superfície da tela | ver abaixo |
| `nvDisplayPointers`, utilização | extras (CRC, estatística) | opcional |

## Tela: dois caminhos
1. **Cópia pela GPU para o framebuffer do NVDA (primeiro alvo).** O NVDA continua varrendo o framebuffer dele; a cada
   swap o copy engine (B0B5) copia o quadro composto para ele. Sem programar display. Custo: ~8 MB/quadro em VRAM.
   Falta descobrir: um objeto RM para a memória do framebuffer do NVDA (candidatos: classe de memória de console
   `NV01_MEMORY_FRAMEBUFFER_CONSOLE`, ou memória física local no offset do framebuffer) e o offset/pitch atuais.
2. **Flip de verdade (depois).** Achado nesta etapa: o GeForceWeb faz o flip em `nvDisplayPipe::DoUpdateNdrv`, chamando
   `NVDA::doDriverIO(1, pb, 0x84, 4)` (vtable `0xaf0`) com um bloco NDRV `csCode = 26` cujo parâmetro (0x88 bytes) é
   `{+0x00 = 0x40a, +0x08 = head, +0x0c = máscara, +0x10..+0x2f = 4 qwords da transação}`. Falta decodificar os 4 qwords
   (preenchidos pela família a partir dos recursos do WindowServer) e o tratamento do `0x40a` no código NDRV do RM.

## Etapa A: o que se descobriu (2026-11-07)
- A r387 **não tem** classe de memória para a tela: `NV01_MEMORY_FRAMEBUFFER_CONSOLE` (0x76) e `NV01_MEMORY_LOCAL_PHYSICAL`
  (0xC2) voltam `0x22` (classe inválida) — `tools/fb_probe.c`. A abertura do framebuffer NVDA é a BAR1 inteira (256 MB em
  `0x7F80000000`), que passa pelas tabelas da BAR1: dali não sai o offset da tela na VRAM. Caminho "copiar para o
  framebuffer do NVDA" sem o protocolo de apresentação: descartado.
- **Protocolo de apresentação ("Presentment") do web driver:** NDRV `csCode 26` com subcomandos `0x40x`:
  - `0x40a` flip (`nvDisplayPipe::DoUpdateNdrv`): bloco de 0x88 bytes `{0x40a, head, máscara, 4 qwords da superfície}`;
  - `0x40d` registra a superfície atual (`nvDisplayManager::DupCurrentSurfaceInNDRV`), com uma `NDRV_SURFACE` de 0x80 bytes
    montada por `GetCurrentSurfaceInfo` a partir do recurso de VRAM do GeForceWeb (handles RM + geometria + offset);
  - há ainda `CallNDRVUpdateMode`, `ClearNDRVAllocatedSurface`, `FreeNDRVAllocatedSurface`.
  - O NVDA pergunta ao acelerador pela superfície com `callPlatformFunction("kNVManageNDRVSurfaceInGLK")`
    (`NVDA::nvGetGLKSurfaceInfo`): na troca, o nosso acelerador terá de responder a isso.
- **User space alcança o NDRV:** `NVDAUser::ndrv_api` = seletor 3 do NVDAUser (o mesmo user client que já usamos). Entrada:
  cabeçalho de 0x14 bytes `{+0 comando, +4 csCode (u16), +0xc tipo 0x80 controle / 0x81 status / 0x82 controle indireto
  (formato do flip: {ptr, tam, ptr resultado, 4}), carga em +0x14}`; chama a mesma entrada NDRV de `NVDA::doDriverIO`.
- Falta: o despacho do csCode 26 dentro do NDRV (região sem símbolos do RM, entrada em `0x2b1400`), para saber o significado
  dos campos da `NDRV_SURFACE` e dos 4 qwords do `0x40a`.

### Experimento decisivo (user space, sem kext, com o Non-Metal instalado)
Alocar uma superfície nossa em VRAM (receita conhecida), pintar um padrão pela BAR1, montar a `NDRV_SURFACE`, mandar `0x40d` e
`0x40a` pelo seletor 3. Se a tela mostrar o padrão, o caminho da fase 6 fecha (o acelerador novo faz o mesmo com os quadros do
WindowServer). Risco: tela errada até o próximo quadro do WindowServer, que faz o próprio flip pelo display pipe e restaura.

## Etapas
- **A. Tela pela GPU, em user space, hoje (Non-Metal instalado):** ferramenta que acha a memória do framebuffer do NVDA no
  RM, mapeia na VA da GPU e manda o copy engine escrever um padrão na tela. Prova o caminho 1 sem kext novo.
- **B. Backend no kernel:** os serviços da tabela acima dentro do MaxwellMetal (link com `com.nvidia.web.NVDAResmanWeb`),
  exercitados por um user client de teste. Testável com o Non-Metal instalado.
- **C. Porte do NVAccel:** compilar contra a família do Tahoe com o nosso backend; display machine usando o caminho 1.
  Só roda depois da troca.
- **D. Troca:** voltar a pilha gráfica do Tahoe (sem o Non-Metal), refazer a AuxKC sem o GeForceWeb, kext armando o Metal
  no boot e WindowServer liberado no plugin. Com o clone pronto: sem acelerador funcionando, tela preta.

## Etapa A: despacho do csCode 26 (decompilado pelo autor no Ghidra, `FUN_002c8847`, 2026-11-07)
Entrada `param_2` = `{blk, len, result, resultLen}` (o formato 0x82). Rejeita com `-0x12` se `blk == 0`, `len < 4` ou
`result == 0`. `param_1` = contexto NDRV do head atual; `param_1[0xc73]` = estrutura compartilhada cujo 1º qword é a
lista ligada dos heads (byte `+0x5b0c` = índice do head). O canal de display do RM tem pushbuffer em
`shared+0x1888`, PUT em `*(shared+0x18a0)`, limite em `shared+0x18ac`, kick com `FUN_002a43ca(ctx, shared+0x1868)`.

### 0x40a (flip) — head inválido volta `-0x11` sem efeito
| Offset | Tipo | Uso |
|---|---|---|
| +0x00 | u32 | 0x40a |
| +0x08 | u32 | head (procurado na lista) |
| +0x0c | u32 | máscara passada adiante (`FUN_002b9f61`); 0 + nada mudou = só kick |
| +0x10 | u64 | flags: bit 1 (0x2) e bit 3 (0x8); 0 = retorna 0 |
| +0x18 | u16 | com 0x2: entradas da tabela de gama |
| +0x20 | ptr | com 0x2: tabela de gama RGB16 (ponteiro de kernel!) |
| +0x28 | u32 | com 0x8: 2º arg de `FUN_002bc77b(head, u32)` |
Sem atualização pendente (`shared+0x16e4 == 0`): `FUN_002b9f61(head, {máscara, ...})`.

**Correção (`FUN_002a8a7d` decompilado):** o bit `0x2` **não é a superfície, é a tabela de gama**. +0x18 = número de
entradas, +0x20 = **ponteiro** para `count × {u16 R, u16 G, u16 B}`; a função monta uma tabela planar
`{0x0003000000010000, u16 count, u16 0xc, R[], G[], B[]}` e entrega a `FUN_002a7fce(head, &tabela, 0)`. O ponteiro é
desreferenciado no kernel sem copyin: **de user space, nunca ligar o bit 0x2** (ponteiro de usuário no kernel = panic).
Logo o 0x40a é "atualização de apresentação" (gama, bit 0x8, máscara + commit), e a troca de superfície mesmo passa pelo
**0x40d** (descritor + bytes de aplicar).

### 0x40d (registrar superfície) — exige `len >= 0x88`; **head inválido = ponteiro nulo no kernel (panic)**
| Offset | Tipo | Uso |
|---|---|---|
| +0x08 | u32 | head |
| +0x0c | u8 | ≠0: copia o descritor e chama `FUN_002b44a2(head, *(head+0x5b74), &desc, blk+0x10)` + `FUN_002c82a0` |
| +0x0d | u8 | ≠0: marca `head+0xcb7*8`; se houver superfície (`head[0xb52]`): `FUN_002b47cb`, `FUN_002b4991` |
| +0x0e | u8 | ≠0 junto com +0x80: aplica (`FUN_0029dedc`, `FUN_002c4ed6`, `FUN_002b2d68`, kick) |
| +0x10 | u32 | 4º arg de `FUN_002b44a2` |
| +0x18..+0x77 | 0x60 B | descritor da superfície (`NDRV_SURFACE` efetiva) → `FUN_002b44a2` |
| +0x7c | u32 | gravado em `ctx+0xc36*8` ao aplicar |
| +0x80 | u8 | precisa ≠0 para aplicar |

### Outros
- `0x406`: só leitura (3 bytes de estado em +0x08..+0x0a) — bom candidato a primeira chamada inofensiva.
- `0x411`/`0x413`: liberam/atualizam a superfície alocada pelo NDRV (`FUN_002b5001`); também sem checar head nulo.
- `0x457` (default): leitura de CRC por notifier em memória de sistema (`NvRmAllocMemory64` classe 0x3E +
  `NvRmAllocContextDma2` + `NvRmBindContextDma`) — mostra o padrão de push do canal de display: `{método | 0x40000, dado}`.

### `FUN_002b44a2(head, depth, desc, arg)` — não lê o descritor, só repassa
- `head+0x6398` = estrutura compartilhada (o mesmo `ctx[0xc73]`). `head+0x5b2c/+0x5b30` = largura/altura do modo atual
  (o 0x401 também passa `head+0x5b2c` como "modo"); vão para `+0x3ff8/+0x3ffc` e `+0x5b78/+0x5b7c`.
- `+0x5b80 = FUN_002a3ade(head+0x5b74)` (profundidade → formato, provável); `+0x5b84 = u16 em +0x5b68`.
- Slots de superfície: `FUN_002b4a01(head, 0|1)` devolve o slot; o descritor é consumido em
  **`FUN_002b4aaa(head, slot, desc, arg)`**. Superfície atual em `head+0x5a90` (slot 0), `+0x5a98` (slot 1); objeto da
  superfície: `+0x1c` refcount, `+0x4c` head, `+0x3c` flag, `+0x60`. `desc == 0` libera os slots (`FUN_002b5001`).
- `head+0x3fcc ≠ 0`: superfície compartilhada entre heads (clone/span) via `head+0x4010` → `+0x28`.
- No fim, `FUN_002b4768(head, superfície)`: provável programação dos métodos de superfície no canal de display.
- (`FUN_00006122 + param_1 + 6` no decompilado é só o offset `0x6128`, erro de tipo do Ghidra.)

### `FUN_002b4aaa(head, slot, desc, hClientSrc)` — onde o descritor é lido
`FUN_002b4a01` aloca o slot (0x68 bytes) e define `slot+0x3c`: 0 = **pitch**, 1 = **block linear** (decidido pelo estado
do RM: `head+0x6458 & 0xf0000`, `head+0x6128`, `head+0x657c`). Layout do slot:

| Slot | Origem |
|---|---|
| +0x00 u64 | offset na VRAM (`desc+0x50`; no caso interno, `NVOS32.offset`) |
| +0x08 u64 | VA de kernel (`NvRmMapMemory` da superfície pelo RM) |
| +0x10 u64 | tamanho (`desc+0x08`; interno: `NVOS32.size`) |
| +0x18 u32 | pitch **calculado pelo RM** (não vem do descritor) |
| +0x1c u32 | refcount |
| +0x20/+0x24 | largura/altura do modo (`head+0x5b78/+0x5b7c`) |
| +0x28 u32 | bpp (`head+0x5b80`) |
| +0x2c/+0x30 | hClient/hDevice do display do RM (`shared+0x1878/+0x187c`) |
| +0x34 u32 | handle novo (`FUN_002b53ce`) — destino do `NvRmDupObject` |
| +0x38 u32 | page kind (`desc+0x4c`; interno: `NVOS32.format`) |
| +0x3c u8 | 0 pitch / 1 block linear |
| +0x44 u32 | 2 ou 4 (`head+0x5f48`) |
| +0x48 u32 | índice do slot |

**Descritor (`blk+0x18`, 0x60 bytes) — campos lidos:** `+0x00` hMemory no cliente de origem; `+0x08` tamanho;
`+0x4c` page kind; `+0x50` offset na VRAM. **`blk+0x10` = hClient de origem.** O RM faz
`NvRmDupObject(hClientDisp, hDevDisp, hNovo, blk+0x10, desc+0x00, 0)` e depois `NvRmMapMemory(..., 0, desc+0x08)`.
Só usa o descritor se `(shared+0x16d0 & 0xf) == 2` (slot 0) ou `(shared+0x16d0 & 0xf0) == 0x20` (slot 1); senão
**ignora o descritor** e aloca memória de sistema (classe 0x3E) por conta própria.

Geometria: o RM impõe largura/altura/bpp do modo atual e calcula o pitch: em pitch-linear via `NvRmControl(subdev,
0x20801324, {w, h, bpp, →pitch, →tamanho}, 0x18)`; em block linear `align64(w × Bpp)` + funções do HAL
(`head+0x128/0x130/0x138`), attr `0x1020000` (Maxwell: arquitetura ≥ 0x130 usaria `0x1820000`). **Nossa superfície tem de
bater com isso** (mesmo layout, pitch e kind).

Alocação interna do NDRV (sem descritor) = NVOS32 r387 com owner `'ndrv'`, `type = 8` (primária), flags `2|4 | 0x4100`,
`+0x38 = head` (um dos dois campos extras da r387), attr acima, alinhamento 0x1000 — é a receita para alocar a nossa igual.

`FUN_002b4768` só chama `head+0x120` na primeira vez em block linear com mapa de kernel (provável limpeza); a
programação do display vem no "aplicar" do 0x40d (`FUN_0029dedc`, `FUN_002c4ed6`, `FUN_002b2d68`, kick).

### Como o GeForceWeb monta o 0x40d (`DupCurrentSurfaceInNDRV` + `GetCurrentSurfaceInfo`)
Bloco de 0x88 bytes, resultado de 4 bytes, csCode 0x1a, via `nvDisplayPipe::PresentmentNdrvDoDriverIO`:
- `+0x00 = 0x40d`; `+0x08 = head` (`this[display*4 + 0x1c0]`); `+0x0c = 1` (registrar); `+0x0d = +0x0e = 0`;
- `+0x10 = hClient` dono da memória (`*param_3` do `GetCurrentSurfaceInfo`);
- `+0x18..+0x87 = NDRV_SURFACE` (0x70 bytes): `+0x00 hMemory`, `+0x08 tamanho`, `+0x10..+0x3b` geometria/layout do
  recurso (informativo; o RM não lê), `+0x4c kind`, `+0x50 offset na VRAM`. Logo `blk+0x7c` e `blk+0x80` ficam 0:
  **o GeForceWeb só registra, nunca aplica pelo 0x40d.** Sucesso marca `this[display + 0x630] = 1`.
- Structs em `nvrm_r387.h`: `NDRV_SURFACE_R387`, `NDRV_REGISTER_SURFACE_R387` (asserts de tamanho/offset ok).

**Sonda de head sem efeito colateral:** `0x40a {head = h, flags = 0}` procura o head na lista e volta `-0x11` se não
existir, `0` se existir, sem fazer nada mais. Usar antes de qualquer `0x40d`.

### `NVDAUser::ndrv_api` (decompilado pelo autor) — seguro para user space
O `structureInput` já vem copiado para o kernel pelo IOKit; para o tipo `0x82` o próprio `ndrv_api` monta
`{blk = entrada+0x14, len = tamanho−0x14, &resultado (pilha do kernel), 4}`. Nenhum ponteiro de usuário chega ao NDRV.
Cabeçalho: `+0x00` comando (`0x84` = controle), `+0x04` csCode (u16), `+0x0c` tipo. Saída: cabeçalho copiado,
`+0x08` = retorno do NDRV (ou o resultado de 4 bytes quando o retorno é 0), bloco devolvido em `+0x14`. Payload `2000`
no tipo 0x82 = `kprintf` de depuração.

### `tools/ndrv_present probe` no hackintosh (2026-11-07, sem root, pela ponte do MaxwellMetal)
```
head 0: retorno 0 -> existe;  heads 1..3: retorno -17 (-0x11) -> não existem
[modo] 1920 x 1080;  [rm] 0x20801324 {1920, 1080, 32}: status 0 pitch 7680 tamanho 0x7e9000
```
O `ndrv_api` funciona pela ponte, a sonda de head se comporta como o decompilado previu, e o RM confirma superfície
pitch-linear 1920×1080×32 com pitch 7680 e tamanho exato 0x7e9000. Próximo: `ndrv_present show 0` (mexe na tela).

### `tools/ndrv_present show 0` (2026-11-07, duas vezes, o autor olhando o monitor)
VRAM primária contígua de 16 MB (offset 0x2f2e0000 / 0x335a0000, kind 0), pintada de magenta pela BAR1, e `0x40d`
{registrar + aplicar} com o nosso hClient/hMemory: **retorno 0 nas duas vezes, sem panic, WindowServer vivo, e a tela
não mudou.** O NDRV aceita a superfície, mas o "aplicar" não leva ao scanout (ou é desfeito na hora pelo GeForceWeb).
Hipóteses: (a) `FUN_0029dedc`/`FUN_002c4ed6` só atualizam estado do NDRV; quem programa o head é o canal de display do
GeForceWeb; (b) `(shared+0x16d0 & 0xf) != 2` e o RM ignorou o descritor; (c) o apply depende de outro estado do head.

### O "aplicar" do 0x40d não toca o endereço da superfície
- `FUN_0029dedc(head, idx)`: nada se `(head+0x6458 & 0xf0) == 0x20`; senão empurra 2–3 pares `{método, dado}` no canal de
  display do RM, com dados de `head+0x5c20/+0x5c24` (ou `+0x5c28/+0x5c2c`) e `head+0x6160`, versões conforme a classe do
  canal core em `shared+0x80` (< `0xc37d`, ≥ `0x857d`). Cara de viewport/escala, **não** de offset da superfície.
- `FUN_002c4ed6` e `FUN_002b2d68` são chamadas indiretas pelos ponteiros de função `head+0xa8` e `head+0x88` (HAL).
- **Contador `shared+0x16e4`:** o 0x40c (máquina de estados) o incrementa/decrementa; enquanto ≠ 0 o NDRV não chama
  `FUN_002b47cb`/`FUN_002b4724` no registro nem `FUN_002b9f61`/kick no 0x40a. Leitura: **enquanto o acelerador
  (GeForceWeb) é dono do display, o NDRV fica passivo**; o `DupCurrentSurfaceInNDRV` só avisa ao NDRV qual superfície
  está na tela, para ele assumir com ela quando o acelerador soltar o display. Explica o "nada mudou".
- Consequência para a fase 6: (1) o acelerador novo programa o display ele mesmo, como o GeForceWeb (canal de display
  próprio: descobrir no `nvDisplayPipe`), ou (2) o display fica com o NDRV (contador 0) e cada flip vira um 0x40d —
  provável estado natural depois de tirar o GeForceWeb, testável só na troca.

### Falta (próximo no Ghidra)
B. GeForceWeb: métodos do `nvDisplayPipe` (Symbol Tree → Classes) — como ele escreve o endereço da superfície no head.
C. `FUN_002c82a0` (chamada no registro com `head+0x5b70`): candidata a programar a superfície quando o NDRV é dono.
A. (feito em parte) `FUN_0029dedc`, `FUN_002c4ed6`, `FUN_002b2d68` (o "aplicar" do 0x40d) e quem escreve `shared+0x16d0`.
0. (feito) **`NVDAUser::ndrv_api`**: o RM desreferencia `blk` e `result` direto. Se o `ndrv_api` não fizer copyin do bloco
   indireto (0x82) para memória de kernel, um ponteiro de user space ali = panic (SMAP no Skylake). **Bloqueia o teste.**
1. **GeForceWeb** (feito) `nvDisplayManager::DupCurrentSurfaceInNDRV` + `GetCurrentSurfaceInfo`: como um chamador legítimo
   preenche o bloco (head, +0x0c/+0x0d/+0x0e, +0x10, +0x7c, +0x80 e os 0x60 bytes inteiros).
2. Quem escreve `shared+0x16d0` (condição para o descritor ser usado).
3. `FUN_0029dedc` (aplicar): confirmar que programa offset/pitch/kind do slot no head.
- `FUN_002a8a7d`: o que são o u16 de +0x18 e o u64 de +0x20 do flip.
- Do lado do GeForceWeb (com símbolos): `GetCurrentSurfaceInfo` preenche o mesmo descritor — confirmar os nomes.

## Revisão (2026-11-08): quem manda na tela, e o experimento decisivo
- O `shared+0x16e4` é um contador de **display desligado** (o 0x40c o incrementa ao ir para os estados 2/3/4/0x13 e
  decrementa ao voltar para 0/1/0x11), não de posse pelo acelerador. Com a tela ligada ele é 0.
- O commit do 0x40a (`FUN_002b9f61` com a máscara expandida em bits 1, 3, 5...) tem cara de UPDATE do canal **core**
  com *interlock* dos canais **base**. Leitura mais provável: o GeForceWeb (`nvDisplayPipe`) tem um **canal base** por
  head, escreve nele o endereço da superfície a cada flip e pede ao NDRV (dono do canal core) o UPDATE com interlock.
  Com um canal base ativo, o que o core programa (o nosso 0x40d) fica por baixo e não aparece. Explica o "nada mudou".
- Sem o GeForceWeb ninguém abre canal base, e o NDRV sozinho define a superfície: é o estado da fase 6.

### Experimento: um boot sem o GeForceWeb (`kext/deploy/exp_sem_geforce.sh`)
AuxKC = a atual menos `com.nvidia.web.GeForceWeb` (teste sem root: kmutil ok, 10 kexts). Tela sem aceleração nesse
boot. Depois: `ndrv_present probe` + `show 0`. Volta: `exp_restaura_geforce.sh` (ou Recovery, `RECUPERACAO.txt`).
- Magenta → o caminho da fase 6 é o NDRV: o acelerador novo apresenta cada quadro com 0x40d (registrar + aplicar),
  e o display machine do NVAccel portado vira ~50 linhas. Também prova que RM + NDRV + MaxwellMetal sobem sem o GeForceWeb.
- Nada → o próximo é imitar o `nvDisplayPipe` (canal base próprio).

### Resultado do boot sem o GeForceWeb (2026-11-08)
- RM, HAL Maxwell e MaxwellMetal sobem sem o GeForceWeb; `ndrv_present probe` ok (head 0). A sessão gráfica cai
  depois do login (o WindowServer do Non-Metal precisa do OpenGL do GeForceWeb); a tela de login funciona.
- `0x40d` registrar + aplicar, `0x40a` commit com máscara 0 e 1: todos retorno 0, **tela não muda**. O NDRV não troca
  a superfície de varredura por esse caminho, nem sendo o único dono do display. Caminho NDRV para a tela: descartado.

### A tela do GeForceWeb é um canal base EVO (pelos símbolos)
`nvGeneralDisplayLayer[Base|BaseInternal]` (`init(..., nvEvoCtxDmaManager*, ..., nvEvoType)`): `StartChannel`,
`CreateCtxDmaForBuffer`/`CreateRequiredCtxDmas` (ctxdma ISO por superfície), `InsertDisplaySurfaceUpdate`,
`InsertPresentControlUpdate`, `SetInterLockWithCore`, `InsertDisplayUpdateForFlip`, `UpdateWithFlush`, `TriggerFlip`,
`GetBaseChannelHandle`. Métodos do canal base da GK110/GM20x: `ref/ogdoc/classes/display/cl927c.h`; core GM20x:
`cl957d.h`. Plano da tela da fase 6: o nosso acelerador abre o próprio canal base no head 0 (como o GeForceWeb) e faz
o flip escrevendo offset/tamanho/storage da superfície + UPDATE. Testável em user space no boot sem o GeForceWeb
(canal base livre). Falta do Ghidra (GeForceWeb): `nvGeneralDisplayLayerBaseInternal::StartChannel`,
`nvGeneralDisplayLayer::CreateCtxDmaForBuffer`, `nvGeneralDisplayLayerBase::InsertDisplaySurfaceUpdate`,
`nvGeneralDisplayLayerBase::UpdateWithFlush` e `SetInterLockWithCore`.

## FLIP PELO CANAL BASE: FUNCIONOU (2026-11-08, `tools/evo_base`, sem root, com o GeForceWeb carregado)
Receita (tudo em user space pela ponte do MaxwellMetal, documentação pública `cl927c.h`/`cl5070.h`):
1. cliente → device 0x80 → subdevice 0x2080; **objeto de display 0x9570** (GM200) filho do device, params `{0,0,0,0}`.
2. pushbuffer: VRAM 4 KiB contígua (NVOS32 tipo 0, attr depth32|compr none|4K|contig|WC), mapa BAR1, ctxdma classe 2
   com `flags = HASH_TABLE_DISABLE | ACCESS_READ_ONLY` (pai do NVOS39 = o cliente).
3. **canal base 0x927C**, pai = objeto de display, params `{channelInstance = head 0, hObjectBuffer = ctxdma do pb, ...,
   subDeviceId = 1}` (struct da r615); o RM devolve `pControl` já mapeado no processo; `RM_MAP_MEMORY(subdevice, canal)`
   também mapeia o controle: `PUT` em +0x0, `GET` em +0x4.
4. superfície: VRAM contígua, ctxdma ISO `HASH_TABLE_DISABLE | READ_ONLY | PTE_KIND_PITCH`, `BIND_CONTEXT_DMA` no canal.
5. métodos `{(1<<18)|mthd, dado}`: `SET_PRESENT_CONTROL(0x84)=0`, `SET_CONTEXT_DMAS_ISO(0xC0)=hIso`,
   `SURFACE_SET_OFFSET(0x400)=offset>>8`, `SET_SIZE(0x408)=w|h<<16`, `SET_STORAGE(0x40C)=(pitch>>8)<<8 | PITCH(1<<24)`,
   `SET_PARAMS(0x410)=A8R8G8B8(0xCF)<<8`, `UPDATE(0x80)=0` (sem interlock com o core); escrever PUT; GET alcança.
6. **A tela ficou magenta.** Liberar o canal NÃO desliga a camada base (a tela ficou rosa); desligar =
   reabrir o canal e mandar `SET_CONTEXT_DMAS_ISO(0)=0` + `UPDATE` (`evo_base off`): a tela voltou.
- O canal base do head 0 estava livre com o GeForceWeb carregado: no Non-Metal a tela anda só pelo core do NDRV.
- Fase 6, display: o acelerador novo abre esse canal no start, cada flip = 7 métodos + PUT, e no stop desliga a camada.
  O caminho NDRV (0x40d) fica descartado.

## Plano do porte (2026-11-08, depois do flip pelo canal base)
O NVAccel do NullMoth fala com o backend só por `callPlatformFunction` em dois serviços (`NVRM` e o framebuffer
`NVRMNVDAFramebuffer`/`NVRMDisplay`), com ABI pequena (`ref/nullmoth/kexts/NVRMFB/nvrm_*_abi.h`):
`nvRmHandles`, `nvAllocVram`/`nvFreeVram`/`nvScanoutInfo`, `nvGpuVaAlloc/Map/Free`, `nvFlipToSurface(Pure)`,
`nvDisplayPointers`, `nvNewAccelClient` (já temos), `nvSurfVram`/`nvSurfPageoff`/`nvSurfShareRm` (IOSurface ↔ NVK).
Então o porte **não reescreve o NVAccel**: o MaxwellMetal publica esses dois serviços e responde a ABI sobre o r387
(API `NvRm*` exportada pelo NVDAResmanWeb, como o GeForceWeb), e o flip vira o canal base EVO.

- **B1 (testável hoje, com o Non-Metal):** MaxwellMetal 0.3 com o backend: cliente/device/subdevice no kernel, VRAM
  (NVOS32 + mapa de kernel), VA da GPU, canal base no head 0 + flip, e um user client de teste (`backend_test`: aloca,
  mapeia, flip magenta, desliga a camada). 1 instalação + reboot.
- **B2:** NVAccel compilado contra a `IOAcceleratorFamily2` do Tahoe: os cabeçalhos do NullMoth (`iofam/`, `re/`,
  `tahoe_fwd.h`) e o binário do **KDK 26.7.2** (`/Library/Developer/KDKs/KDK_26.7.2_25G332.kdk`, x86_64 com
  símbolos) para conferir cada símbolo importado. Só compila/linka; não carrega com o Non-Metal.
- **C/D (a troca, com o clone pronto):** reverter o Non-Metal (snapshot original da Apple ainda existe: `com.apple.os.update-…`,
  XID 298) mantendo os kexts do RM/HAL na AuxKC, sem o GeForceWeb; NVAccel + MaxwellMetal na AuxKC; WindowServer
  liberado no plugin. Volta: clone ou a AuxKC/snapshot anteriores.

## B1: backend no kernel (MaxwellMetal 0.3.0, 2026-11-08) — FUNCIONOU
- `kext/MaxwellMetal/MaxwellBackend.cpp`: cliente RM do **kernel** (`NvRmAllocRoot` + device + subdevice, API exportada
  do NVDAResmanWeb; `OSBundleLibraries` ganhou `com.nvidia.web.NVDAResmanWeb 1.2.0`), VRAM com **mapa de kernel**
  (`NvRmVidHeapControl` + `NvRmMapMemory`), VA da GPU próprio (virtual 0x70 + ctxdma + `NvRmMapMemoryDma` fixo) e a
  tela pelo canal base 0x927C (ctxdma ISO por superfície + bind, 7 métodos + PUT, GET conferido).
- Nada roda no boot: o RM só abre quando o cliente de teste `'MXBK'` (root) abre; ao fechar, desliga a camada base e
  libera tudo. Assinaturas de `NvRmAllocContextDma2`/`NvRmBindContextDma` conferidas nos wrappers exportados.
- Teste: `sudo ~/maxwell_metal/tools/backend_test` (VRAM pintada pelo kernel → leitura → VA da GPU → flip → off).

### B1 no hackintosh (2026-11-08 13:30, com o Non-Metal e o GeForceWeb carregados)
```
[rm] cliente do kernel 0xc1d000d0 device 0xbee10001 subdevice 0xbee10002
[vram] offset 0xc120000 tamanho 0x7e9000;  [kva] leitura pelo mapa de kernel confere
[gpuva] VA 0x200000000;  [flip] PUT 0x38 GET 0x38 (consumido) -> tela azul-claro;  [off] PUT 0x48 GET 0x48
RESULTADO: backend ok
```
Cliente RM do kernel, VRAM com mapa de kernel, VA da GPU e flip pelo canal base, tudo de dentro do kext. Próximo: B2.

## B2 (2026-11-08): NVAccel compila contra a família do Tahoe — FEITO
`accel/` (`build.sh`): `nvrm-accel.cpp` do NullMoth + cabeçalhos `iofam/`, `re/`, `tahoe_fwd.h`; KAPI trocado por um
`nvkms-kapi.h` mínimo (só o CRC por sysctl, desligado). Sai `MaxwellAccel.kext` (`local.maxwell.MaxwellAccel`), 0 erros.
`symcheck.sh` no hackintosh: os **869 imports** resolvem contra o KDK 26.7.2 (kernel, IOAcceleratorFamily2 487.4.4,
IOSurface) + IOGraphicsFamily/IOPCIFamily; os 11 "privados" (`IOLockLock`, `sysctl_*`, `IOMalloc`…) são os mesmos que o
MaxwellMetal já usa e carrega.

## B3 (2026-11-08): ligação acelerador ↔ backend — compilado, teste da ABI pronto
- `accel/src/nvrm-accel.cpp`: as 9 chamadas de backend (`nvAllocVram/FreeVram/ScanoutInfo`, `nvRmHandles`,
  `nvFlipToSurface(Pure)`, `nvDisplayPointers`, `nvGpuVa*`, `nvNewAccelClient`, surfshare) vão para o serviço
  **MaxwellMetal** (`nvMxCall`); o framebuffer passa a ser o **NVDA** do web driver (`serviceMatching("NVDA")`,
  `metaCast("NVDA")`). Original em `nvrm-accel.orig.cpp`.
- MaxwellMetal 0.4.0: `callPlatformFunction` responde a ABI: VRAM com mapa de kernel + endereço físico de BAR1
  (`IOMemoryDescriptor::withAddress` + `getPhysicalSegment`, exige contíguo), orçamento de BAR1 160 MB; superfície
  **home** do tamanho do modo (`getPixelInformation` do NVDA) posta na tela pelo canal base; flips para qualquer superfície
  alocada (HOME = volta para a home); VA da GPU com `IORangeAllocator` sobre o virtual 0x70 do cliente do kernel;
  `nvNewAccelClient` = a ponte (NVDAUser em nome da task). Falta: **surfshare** (IOSurface ↔ NVK: `nvSurfShareRm` vira
  `NvRmDupObject` pelo kernel; `nvSurfPageoff`) — responde NVRM_SS_OFF por enquanto.
- Teste sem carregar o acelerador: `sudo ~/maxwell_metal/tools/abi_test` (seletor 7 do cliente de teste percorre a ABI).

### ABI testada no hackintosh (2026-11-08 15:45, MaxwellMetal 0.4.2, com o Non-Metal): `RESULTADO: ABI ok`
```
[0] home: phys 0x7f80a40000 1920x1080 pitch 7680 (tela preta)
[1] nvAllocVram: phys 0x7f81260000 tamanho 0x7f0000 BAR1 usado 0xfe0000
[2] flip (cor) ok   [3] flip HOME ok   [4] VA da GPU 0x1200000000 alloc+map+free ok   [5] free ok
```
Endereço físico = BAR1 (`0x7F80000000` + offset); o `getPhysicalSegment` devolve 4 KiB por vez, a contiguidade é
conferida página a página. Os dois framebuffers NVDA dizem 1×1 em `getPixelInformation` com o Non-Metal: vale o padrão
1920×1080 (achar outra fonte do modo antes da troca). Armadilha de instalação: reiniciar só depois do `PRONTO`.
Próximo: surfshare (IOSurface do WindowServer ↔ NVK do plugin), depois a troca (C/D).

## Surfshare (IOSurface do acelerador ↔ NVK) — MaxwellMetal 0.5.0, compilado (2026-11-08)
Cadeia: NVK `nvkmd_nvrm_import_dma_buf` → escape `NVRM_ESC_SURF_VRAM` (0xE2) → **libnvrm387** (`esc_surfshare`: acha o
device do cliente, escolhe um handle novo, põe os dois em `hSrcClient`/`hSrcMemory` como entrada) → ponte do kernel
(seletor 100 do `IOAccelMaxwellRMClient`) → `NVAccel::callPlatformFunction("nvSurfVram", esc, task, cmd)` → o acelerador
acha o NVVidMemory do IOSurface e pede `nvSurfShareRm` (porte: passa device e handle) → backend faz
`NvRmDupObject(cliente do NVK, device, handle novo, cliente do kernel, VRAM)` → o NVK faz o próprio DUP desse handle e a
libnvrm387 libera o intermediário. `nvSurfPageoff` (cópia para páginas de sistema) segue sem porte.
Teste sem acelerador: passo 6 do `abi_test` (dup para um cliente de usuário + leitura da cor pelo handle novo).
NVK recompilado com a libnvrm387 nova (`kext/gpubundles/nvmtl/libvulkan_nouveau.dylib`), instalado pelo instalador.

### Surfshare testado (2026-11-08 15:59, MaxwellMetal 0.5.0): `RESULTADO: ABI ok`
`[6] dup no cliente 0xc1d000e0 device 0xcafe0080 -> handle 0xcafe5500; leitura pelo handle do usuário: 0xff20e020
(confere: é a mesma VRAM)`. O kernel faz `NvRmDupObject` de uma VRAM do cliente do kernel para dentro de um cliente RM
de usuário, e o usuário mapeia e lê a mesma memória. Todas as peças do backend que o acelerador usa estão testadas
com o sistema atual. Próximo: preparar a troca (C/D).

## Troca (C/D) preparada — `kext/troca/` (2026-11-08), nada executado
Estado do OCLP (`/System/Library/CoreServices/OpenCore-Legacy-Patcher.plist`, 3.0.0-rc.5): Non-Metal tirou
IOAcceleratorFamily2/IOGPUFamily/AMD/Intel/GeForce.kext do sistema e pôs a família 10.13.6, IOSurface 10.14.6,
CoreDisplay/OpenGL/GPUSupport antigos, SkyLight/QuartzCore remendados; gravou `useMetal=NO`/`useIOP=NO` no
CoreDisplay (volume de dados); o web driver pôs os kexts NVDA/GeForce em /Library/Extensions e os bundles GL/MTL no
sistema, e tirou o NVDAStartup.kext da Apple. Snapshot selado original: `com.apple.os.update-ECF5…` (XID 298).
O KDK e o snapshot têm a IOAcceleratorFamily2 487.4.4 original. iGPU desligada (só a GTX 960 no barramento).
- `1_preparar.sh` (seguro): monta o snapshot selado só leitura e liga a AuxKC nova contra a Boot/System KC dele
  (kexts NVDA sem GeForceWeb/Tesla + MaxwellMetal ≥ 0.5 + MaxwellAccel) → `.kc.troca`; guarda `.kc.pre-troca` e tar.
- `2_trocar.sh` (confirmação TROCAR): desliga os LaunchAgents/Daemons do OCLP (auto-patch reaplicaria o Non-Metal),
  apaga `useMetal/useIOP`, lista do plugin com `!WindowServer` etc., `bless --mount / --bootefi --last-sealed-snapshot`,
  publica a AuxKC. Boot-args a acrescentar: `nvaccel=1 nvaccelarm=1`.
- MaxwellAccel 0.2.0: portão `nvaccel=1` (do NullMoth); a espera do `nvrm-claimed` virou "NVDA filho da GPU +
  serviço MaxwellMetal"; temperatura desligada (registro da Maxwell não conferido; `-nvacceltemp`); `MetalPluginName =
  NVMTLDriver` (o caminho que já funciona); **auto-arme** com `nvaccelarm=1` (o WindowServer do Tahoe aborta sem
  dispositivo Metal; o NullMoth só arma por `sysctl debug.nvaccelfb=1`).
- `3_conferir.sh` (sem root, por ssh) e `9_voltar.sh` (recoloca tudo + `OpenCore-Patcher --patch_sys_vol`; ssh com tela
  preta). Último recurso: o clone.

## A troca feita (2026-11-08 16:10) e o primeiro boot do acelerador
- Boot 1 (sem `nvaccel`): sistema = snapshot selado original, IOAcceleratorFamily2 487.4.4, AuxKC nova aceita (NVDA,
  MaxwellMetal, MaxwellAccel carregados), RM/canal base ok. **WindowServer sobe sem Metal: "Software compositor
  activated"** (o Tahoe não exige dispositivo Metal).
- Boot 2 (`nvaccel=1`): MaxwellAccel 0.2.0 esperou o RM e desistiu (o NVDA é neto da GPU: `GFX0 → NVDA,Display-A@0 →
  NVDA`); 0.2.1 corrige. Boot 3: **o acelerador sobe**, o WindowServer cria as superfícies dele na nossa VRAM
  (1920×1080, mapeadas na GPU pelo backend), mas a tela fica preta: o scanout é fechado por padrão.
- `debug.nvaccelfb=3`: superfície home criada e posta na tela pelo canal base (preta). `debug.nvaccel_iop=1` + reiniciar
  o WindowServer: **loop de crash** — `IOPresentment: Unable to get AGDC information (0x10000003) → Pipe creation
  failed`. Também: o GPUWrangler registrava o MaxwellMetal (IOAccelerator em IOResources) como 2ª GPU (`gpu 0x8000
  vid.did=ffff.ffff`).
- Correções: **MaxwellAGDC.kext** (`agdc/`: NVRMAGDC do NullMoth olhando para o NVDA; liga com `sysctl
  debug.nvrmfb_agdc=1`); **MaxwellMetal 0.6.0** = IOService comum, ponte para sandbox num `MaxwellMetalBridge`
  (IOAccelerator) só sem `nvaccel=1`; **libnvrm387** abre o RM primeiro pelo NVAccel (tipo 0x4E560000, via
  `nvNewAccelClient`), depois MaxwellMetalBridge, depois MaxwellMetal. Scripts: `troca/atualizar_accel.sh` (instala
  MaxwellAccel/AGDC + chama o instalador, que agora inclui o AGDC) e `troca/4_ligar_tela.sh` (AGDC → nvaccelfb=3 →
  nvaccel_iop=1 → kickstart do WindowServer).

### Estado em 2026-11-08 16:45 (MaxwellAccel 0.2.2, MaxwellAGDC 0.1.1, MaxwellMetal 0.6.0)
- Com `iop=1`: AGDC responde 0x980 com os 6 framebuffers (`n=6`), os 6 pipes abrem (`GPU: FB: 6 of 6 opened`), mas o
  CoreDisplay não acha display online (`Setting offline display 0x00000000 main` → assert em
  `CoreDisplay_CreateDisplayForCGXDisplayDevice` → SIGABRT, loop). Antes: `IOFBSetDisplayModeAndDepth: Setting mode
  0x0 ... Failed to obtain mode info` e AGDC recusando `0x2001` em cada framebuffer (o NullMoth também não responde
  0x2001; o NVRMFB dele provavelmente dá ao CoreDisplay o que o NVDA não dá).
- Com `iop=0` e o acelerador: o WindowServer vive, compõe em software nas superfícies do acelerador, mas nada chega à
  tela (a cópia/troca pelo contexto 2D está em stub; `nvAccelFlipTo`/`nvAccelCopyToScanout` não têm chamador).
- Caminhos: (a) troca legada: cada flush de superfície copia (CPU, BAR1) para a home na tela; (b) descobrir o que o
  CoreDisplay exige do framebuffer/AGDC (0x2001) para pôr o display online no caminho IOPresentment.

### MaxwellAccel 0.3.0: troca legada com flip (2026-11-08, compilado, não testado)
Sem IOPresentment o WindowServer vive e chama `NVSurface::setSyncCommand` a cada quadro (9163 chamadas em 30 min; no
NullMoth é stub vazio). Os buffers da superfície (`surf+0x11C0`, até 32; `surf+0x11B0` = smode) são VRAM do backend
(pitch 7680), então cada troca faz o **flip do canal base direto para o buffer trocado** (`nvAccelFlipTo`, numa
thread_call; índice = `a1`, senão `a2`, senão 0 — a confirmar pelos logs `legacy swap:`). Sysctls:
`debug.nvaccel_legacy_flip` (1 = liga, padrão), `debug.nvaccel_legacy_swaps`, `debug.nvaccel_legacy_flips`.
Tirada a recusa de flip com vários heads (uma tela só; todas as portas do NVDA usam o canal base do head 0).
Teste: boot com `nvaccel=1`, SEM o `4_ligar_tela.sh` (iop fica 0).

## 2026-11-08 17:10: **"Metal compositor activated"** — o WindowServer compõe no nosso Metal
MaxwellAccel 0.3.2 (`armNow`: o sysctl arma sempre) + `troca/5_metal_windowserver.sh` (iop=0, nvaccelfb=3, kickstart):
o WindowServer carrega o NVMTLDriver (clientes RM pelo acelerador, `newUserClient(0x4e560000)`), o QuartzCore pergunta
`vendorName -> NVIDIA` e o SkyLight loga **"Metal compositor activated."**. Mas:
- `CoreDisplay: Unable to find display pipe for IOFB service / regID 0x10000045e` → sem pipe o quadro não é apresentado e
  o CoreDisplay cai no renderizador por software (`CA::OGL::SW::scanline`) na memória do framebuffer, que não tem mapa
  de CPU → SIGSEGV a cada ~4,7 s; após 120 s o watchdogd dá panic (`userspace watchdog timeout: WindowServer`).
- `nvaccelfb=4` (aperture: mapa de CPU pela BAR1) faz o software desenhar na home, mas lento demais: o WindowServer
  trava (spin) → watchdog.
- Com `iop=1` os pipes existem, mas o display fica offline: o NVDA não dá informação de modo
  (`IOFBGetDisplayModeInformation` falha, `getPixelInformation` = 1×1). É o papel do NVRMFB no NullMoth.
- Próximo: um framebuffer nosso (`MaxwellFB`, IOFramebuffer) com o modo real do monitor (1920×1080@60, EDID), cuja
  varredura é a superfície home/flip do canal base, para o CoreDisplay ter display online com pipe (IOPresentment).

### Metal + IOPresentment (2026-11-08 19:07, MaxwellAGDC 0.2.0, MaxwellAccel 0.3.2)
`6_metal_iop.sh` (AGDC + K5, nvaccelfb=3, iop=1, kickstart): o WindowServer fica **estável** com "Metal compositor
activated", displays enumerados com `isAcc=YES` (o monitor está no NVDA índice 2: `AppleDisplay`, 1920×1080@60,
148,5 MHz, `IOFBDetailedTimings`). Falta: `CoreDisplay: Attempting to get capabilities when none exist` / `No plane
indices` → nenhuma transação (`iop_ok 0`). O K5 nunca é consultado (`k5_asked 0`); o AGDC recebe `0x3`, `0x941`, `0x2`
e `0x2001` (4 bytes, uma vez por framebuffer), todos recusados (o NullMoth também recusa). `IOAccelDisplayPipe::
copyCapabilities` (KDK) só devolve a propriedade `IOAccelDisplayPipeCapabilities` do acelerador, igual à dos drivers
Intel. Suspeita: o `0x2001` é o pedido de capacidades do framebuffer. MaxwellAGDC 0.2.1 loga o tamanho de saída de cada
comando recusado e tem `debug.mxagdc_try` (bits 1/2/4/8 = responder 0x2001/0x941/0x3/0x2 com sucesso zerado).

### De onde vêm as "capabilities" que faltam (2026-11-08 19:20, pelas strings do dyld cache x86_64 do Tahoe)
- AGDC 0.2.1 logou o que o CoreDisplay pede: `0x3` (sai 4 B), `0x941` (48 B ↔ 48 B), `0x2` e `0x2001` (entrada 4 B =
  1, **saída 0 B**: são ajustes, não consultas). Aceitar `0x2001`/`0x2` (`debug.mxagdc_try=9`) não mudou nada.
- `CoreDisplay::LoadCapabilities(IOPresentmentRef, sPipelineDictionary*)` → "capabilities with no devices / device
  with no planes". O framework **IOPresentment** (em `dyld_shared_cache_x86_64h`) converte um dicionário com
  `IOPresentmentCapabilitiesStructureFromDictionary` (`SetDeviceCount`, `SetPlaneCount`, `sourceScalerFromDictionary`,
  `linearizationFromDictionary`, `scalerBWEntryFromDictionary`) e carrega `IOAccelerator.framework`.
- No kernel, `IOAccelDisplayPipeUserClient2::getCapabilitiesData` (KDK) chama o virtual do pipe em **vtable +0x8d0**
  (`copyCapabilities`), serializa (`OSSerialize`) e copia ao usuário. A base devolve a propriedade
  `IOAccelDisplayPipeCapabilities` do acelerador (2 chaves no NullMoth e nos plists Intel). Nenhum kext nem bundle
  contém as chaves do esquema → **o NVDisplayPipe pode sobrescrever `copyCapabilities` e devolver o dicionário inteiro**.
- Chaves do esquema (cstrings do IOPresentment): `PipelineDictionaryVersion`, `DeviceArray`, `EntryDictionary`,
  `EndPointDictionary` {`Flags`, `PixelFormats`, `LooksLikeWidth/Height`, `RefreshTime`, `Min/MaxRefreshTime`,
  `PixelClock`, `Min/MaxPixelClock`, `HorizontalActive/Blanking/SyncOffset/SyncPulseWidth/SyncConfig`,
  `VerticalActive/Blanking/SyncOffset/SyncPulseWidth/SyncConfig`, `VerticalBlankingExtension`, `LinkCurrentMaximumBPC`},
  `InlineBufferDictionary`, `ActivationLatency`, `DeactivationLatency`, `MaxWidth`, `MaxHeight`, `QueueDictionary`
  {`QueueDepth`}, `RotationDictionary`, `PipelineScalerDictionary`, `NonAtomicApplicationPeriod`, `SourceSubRegion`,
  `SourceUpScalingArray`/`SourceDownScalingArray` {`BestDictionary`/`WorstDictionary` {`Width`, `Height`, `FilterType`,
  `CoefficientDictionary` {`HTaps`, `HPhases`, `HTableSize`, `VTaps`, `VPhases`, `VTableSize`}}}, `CursorArray`
  {`CursorMaximumFrames`, `CursorInputSize`, `CursorScalingLimits` {`CursorDown/UpWidth/Height`}}, `PlaneArray`
  {`BlendType`, `ClosedCaptionRegions`, `PlaneScalerDictionary`, `LinearizationArray` {`SegmentDictionary`, `Type`,
  `ConfigurationID`, `MinimumValue`, `MaximumValue`, `MaximumSize`}, `GammaArray`, `FixedFormat`, `CSCArray`}; o
  CoreDisplay ainda lê por plano `SupportedPixelFormats`, `ScalingMinimum/MaximumWidth/Height`,
  `ColorSpaceConversionSupported`.
- **Próximo:** tipos e aninhamento exatos — extrair o IOPresentment do dyld cache (ipsw no MacBook) e ler
  `IOPresentmentCapabilitiesStructureFromDictionary`; então `NVDisplayPipe::copyCapabilities` devolve 1 dispositivo
  (o monitor do NVDA índice 2, 1920×1080@60, 148,5 MHz) com 1 plano BGRA (`'BGRA'`), sem escala nem CSC.

### Causa raiz da tela preta (2026-11-08 19:50, `log stream --level debug` do WindowServer)
RE do IOPresentment (arm64e deste MacBook, carregado por `iopload`/`iopdump` + `llvm-objdump`; o `ipsw` e o LLDB foram
barrados): `IOPresentmentCapabilities::__gatherCapabilities` → `__GatherAGDCLogicalDeviceCapabilities` (AGDC `0x921`,
0xb0 B) e `__GatherAGDCDisplayPipelineCapabilities` (AGDC `0x711`, 0x196c B: plane scaler, pipeline scaler,
linearização, gamma, pixel processing…), só se a entrada do mapa de associação existir e tiver `+0x10 != 0`.
No stream ao vivo:
- `(IOPresentment) Info: Valid ADGC node found for this device.`
- `(IOPresentment) Warning: Unable to acquire capabilities for <fb>:0x1000004e1:0x100000414, return code: 0x2006`
  (para os 6 NVDA; 0x1000004e1 = AGDC, 0x100000414 = acelerador)
- no NVDA do monitor (`0x100000458`): `InstallTiming - legacy… FAILED` para todos os modos (`0xe00002dd`),
  `Failed preflight for 640 x 480 … Driver returned 0xffffffef`, `IOFBSetDisplayModeAndDepth: Setting mode 0x0`,
  `Failed to obtain mode info` → display offline → sem capacidades.
**O NDRV do NVDA (2017) não aceita a instalação de timing do CoreDisplay do Tahoe.** Conclusão: framebuffer próprio
(MaxwellFB, IOFramebuffer) é necessário: 1 modo (timing do NVDA índice 2: 1920×1080@60, 148,5 MHz),
`validateDetailedTiming`/`setDetailedTimings` aceitando, `getTimingInfoForDisplayMode`, atributos de conexão, EDID
(do `AppleDisplay`), vblank por timer, abertura = superfície home; AGDC listando o MaxwellFB; NVAccel ligado a ele.
Log completo no hackintosh: `~/maxwell_metal/iop_stream_2026-11-08.log`. Ferramentas de RE: scratchpad `iopdump.c`,
`annot.py`, `dumpfn.sh` (reconstruir se preciso: dlopen do IOPresentment + dump de bytes + `llvm-objdump`).

## MaxwellFB 0.1.0 (2026-11-08): framebuffer nosso no lugar do NVDA para a tela

Codigo em `fb/` (MaxwellFB.cpp, build.sh). Casa no IOPCIDevice 10de:1401 (GFX0) com IOMatchCategory propria; sobe so com
boot-arg `nvaccel`; `-mxfboff` ou `mxfb=0` desligam.
- espera o servico MaxwellMetal, copia o EDID do AppleDisplay embaixo do NVDA (serial XOR 0x4D, checksum refeito),
  pede `nvScanoutInfo` (superficie home, a que o canal base EVO varre) e publica a abertura como subrange do BAR1;
- um modo (id 1) com o timing do 1o DTD do EDID que bate com o raster (ou CEA 1080p60); validateDetailedTiming aceita
  timing com o mesmo raster e pixel clock a 0,5%, sem escala/rotacao/borda; setDisplayMode desses ids nao faz modeset;
- IOFBTimingRange, IOFBScalerInfo (sem escala), NVRMTiming (13 palavras para o MaxwellAGDC), startup-timing;
- IOFBDependentID 0x4D58464200000001 (controlador proprio, fora do controlador do NVDA), IOFBDependentIndex 0;
- vblank por thread_call no periodo do timing; cursor por software; gamma ignorada.
MaxwellAccel 0.4.0 e MaxwellAGDC 0.3.0 usam o MaxwellFB quando a classe dele existe (kext carregado) e nao ha
mxfb=0/-mxfboff; senao voltam ao NVDA. Instalar: `sudo ~/maxwell_metal/kext/troca/atualizar_accel.sh` e reiniciar.
Tirar: `sudo ~/maxwell_metal/kext/troca/tirar_fb.sh` (por ssh). Conferir: `~/maxwell_metal/kext/troca/7_fb_conferir.sh`.
Expectativa: com o compositor por software o WindowServer ja desenha na abertura (a superficie home na tela); com
6_metal_iop.sh o IOPresentment deve achar capabilities no MaxwellFB.

### MaxwellFB 0.2.0: start sem bloquear a GFX0
1o boot com 0.1.2: GFX0 "busy 90561 ms", nvScanoutInfo NotReady (0x2d8) 60 s, NVDA/AppleDisplay so depois. O IOKit da
start nos drivers de um provedor em serie, e o start do MaxwellFB esperando o backend segurava a GFX0 (RM e NVDA
juntos). Agora `MaxwellFBNub` casa na GFX0, devolve o start na hora e uma thread_call espera a superficie home (ate
240 s) e o EDID (ate 30 s) antes do registerService; o MaxwellFB casa no nub (IOProviderClass MaxwellFBNub) e acha o
IOPCIDevice subindo pelos provedores. Erros de link vistos no caminho: faltava `_kmod_info` (KMOD_EXPLICIT_DECL) e
`thread_call_cancel_wait` e KPI Private. Checagem certa dos imports: KPIs do KDK (System.kext/PlugIns) + IOGraphics/IOPCI do KDK.

### MaxwellFB 0.2.1
Com o 0.2.0 no ar e o WindowServer reiniciado: o WS abriu o MaxwellFB (enableController, vblank, display0/AppleDisplay),
mas o CoreDisplay recusou todo timing em `InstallTimingWithOneWireFormat`: "Declined SDR_RGB__8_BT709 because fbRange_b
... 0xe00002c7". fbRange_b = formato de sinal contra o IOFBTimingRange; EDID CEA-861 (HDMI) so gera formatos BT709 e o
range tinha NativeRGB|sRGB. Correcao: colorimetria + BT601/BT709. E o WS sobe com o registro quieto (boot 0.1.2: WS 1 s
depois do FB soltar a GFX0; boot 0.2.0: WS abriu os NVDA 2 s antes do MaxwellFB): o nub agora fica adjustBusy(+1) ate
registrar (ou 240 s). Travamento na maca no 1o boot do 0.2.0: sem relatorio de panic; o 2o boot subiu normal.

### MaxwellFB 0.2.2 + LaunchDaemon local.maxwell.mxfb-ws
O 0.2.1 (nub adjustBusy) segurou o WindowServer e o backend ficou NotReady 240 s: `NvRmAllocRoot` volta status 0 com
handle 0 ate o RM r387 ser inicializado, e sem o GeForceWeb quem inicializa e o NVDA quando o WindowServer abre o
framebuffer dele. Logo o MaxwellFB so pode nascer depois do WindowServer. 0.2.2 tira o busy; o daemon
(/usr/local/libexec/maxwell/mxfb_ws.sh, RunAtLoad) espera o MaxwellFB e, se o WindowServer nao o abriu, faz
`launchctl kickstart -k system/com.apple.WindowServer` uma vez (log /var/log/mxfb_ws.log). Instalado pelo
atualizar_accel.sh; removido pelo tirar_fb.sh. Pendente: confirmar o BT709 (0.2.1+) no InstallTiming.
Licao de deploy: uma AuxKC publicada se perdeu com reboot ~40 s depois do PRONTO; conferir `kmutil inspect` e `sync && sudo reboot`.

### 2026-11-08 21:24: display ONLINE no MaxwellFB, Metal compositor ativo
Com 0.2.2 + daemon + 6_metal_iop.sh: CoreDisplay "Display 0x1c80f31c ... online enabled, Bounds (0,0)[1920x1080]" e
"Metal compositor activated". InstallTiming SDR_RGB__8_BT709 ok para 0x80001000/0x80001001. Falta o pipe: a familia
registra sozinha todo framebuffer que o WindowServer abre; os 6 NVDA lotaram o limite cfg+0x28 = 6 e o MaxwellFB ficou
de fora ("found_framebuffer ... count went 6 -> 6", "Unable to find display pipe for IOFB ... 0x10000053b"); sem pipe o
IOPresentment nao pede capabilities ao AGDC (k5_asked 0). MaxwellAccel 0.4.1: kNvFamilyMaxFB = 8 so no cfg+0x28.

### 2026-11-08 21:30: por que o CoreDisplay nao acha o pipe do MaxwellFB (RE do CoreDisplay/IOPresentment)
- MaxwellAccel 0.4.1: o MaxwellFB ganhou pipe no kernel (pipe 6 ativo, IOAccelIndex 6, initFramebufferResource sobre a
  superficie home). Mesmo assim o CoreDisplay: "Unable to find display pipe for IOFB ... 0x10000053a".
- CoreDisplay::FindDisplayPipeForService(fb) procura o registry ID do FB em CoreDisplay::gDisplayPipes; quem preenche e
  InitializeDisplayPipes (inline em CGXMappedDisplayStart), que percorre IOPresentmentGetMap(): pares
  {"IOAccelerator","IOFramebuffer"} -> um pipe por par.
- O mapa (ioPresentmentCreateAssociationMap) sai do AGDC: para cada AppleGraphicsDeviceControl, kAGDCVendorInfo +
  ioPresentmentGetNodeMap (pais com "class-code" = caminho PCI) + o IOAccelerator do mesmo caminho PCI, e as entradas de
  framebuffer por kAGDCVendorInfo/kAGDCGPUCapability. Chaves: IOAccelerator, AppleGraphicsDeviceControl, IOFramebuffer,
  IOPresentmentClient, IOAccelIndex.
- Ha dois AGDC na GFX0: NVDC (0x1000004df, criado pelo NVDAResmanWeb via NVDC::withBoardNumber) e o nosso
  NVDADeviceControl. O IOPresentment usou o NVDC: "Unable to acquire capabilities for <NVDA>:0x1000004df:<NVAccel>" para
  os 6 NVDA e "No matching objects found in map" para o MaxwellFB.
- MaxwellAGDC 0.3.1: debug.mxagdc_nvdc=1 (padrao) faz terminate(0) no NVDC da mesma GPU antes de o nosso subir.
- 0.3.1 no hackintosh: "NVDC ... retirado -> 0" (terminate(0) recusado com cliente aberto); IOPresentment seguiu com o
  NVDC. MaxwellAGDC 0.3.2: terminate(kIOServiceRequired).
- 0.3.2 tambem "retirado -> 0": AppleGraphicsDeviceControl::terminate (KDK 0x302a) recusa com o provedor ativo e this+0xb5 bit0 desligado. 0.3.3 chama IOService::terminate(kIOServiceRequired) direto.
- 0.3.3 (IOService::terminate direto no NVDC): o kernel travou na hora (sem panic gravado). Retirar o NVDC: descartado.
- RE do ioPresentmentGenerateDeviceMap: o laco do FindAGDCMatch junta TODOS os AGDC (pula AppleGPUWrangler), mas para
  cada dispositivo PCI o mapa usa o PRIMEIRO AGDC da lista com o mesmo caminho PCI. A lista segue a ordem de attach na
  GFX0; o NVDC nasce na inicializacao do RM (~5 s apos o boot). Os framebuffers de cada AGDC vem da resposta 0x980
  (framebuffer[i], maxFramebuffers em +0x30).
- MaxwellAGDC 0.4.0: sem retirada do NVDC; com boot-arg nvaccel (e sem -mxfboff/mxfb=0/-mxagdcoff) o AGDC sobe sozinho
  no boot, preso na GFX0 do MaxwellFBNub, antes do NVDC; sem framebuffer, o 0x980 responde 0 e o rescan do 0x980 poe o
  MaxwellFB quando ele nasce. tirar_fb.sh tambem desliga isso (sem nub o modo automatico desiste).
- 0.4.0 no hackintosh: o NVDC (22:26:45.9) ainda nasceu antes do nosso AGDC (22:26:46.2), porque o worker esperava o
  MaxwellFBNub *registrado*, e o nub so registra com o backend pronto. 0.4.1: a GPU vem direto de um IOPCIDevice
  NVIDIA de video (existe desde ~22:26:41); slots do 0x980 pelo IOAccelIndex (MaxwellFB no 6, posicao = IOAccelIndex
  da entrada do mapa = pipe do acelerador), slots vazios permitidos, kAgdcMaxFB 8.
- 0.4.1 no hackintosh: nosso AGDC enfim primeiro (antes do NVDC), slot table n=7 [6]=MaxwellFB, iop_empty 6, mas ainda
  "No matching objects found in map" e o WindowServer em loop (SIGSEGV em CA::OGL::SW::scanline: renderizador de
  software sem superficie). Causa: o kext AGDC da Apple (KDK 0x29ef) converte framebuffer[i] -> registry ID so ate o
  primeiro slot NULL (e no maximo 8); com 0..5 vazios o slot 6 chegou como ponteiro cru. 0.4.2: slots contiguos
  (NVDA com IOAccelIndex 0..5 + MaxwellFB 6), sem timing K5 para os NVDA.
- OCLP: o Non-Metal dele (Maxwell/Pascal) e web driver + OpenGL com useMetal=NO/useIOP=NO no CoreDisplay; Metal so na
  Kepler (drivers Kepler da Apple). 8_tela_legado.sh: MaxwellFB no caminho antigo (useIOP=NO, useMetal=NO) = tela por
  CPU na superficie home + Metal para apps.

### 2026-11-10: a camada base nao aparece em nenhum head (caminho antigo e caminho IOP)
- 8_tela_legado.sh (useIOP=NO, useMetal=NO): MaxwellFB online como tela principal, "Software compositor", WS desenha
  direto na abertura (legacy_swaps 0). Monitor aceso e preto, sem cursor.
- abi_test (flip no head 0 pelo backend) e evo_base com EVO_HEAD=1..3: tudo status 0, GET==PUT (consumido), nada na tela.
- disp_probe (NV04_DISPLAY_COMMON 0x73, numeracao ANTIGA da r387: GET_SUPPORTED 0x730120/12 B, GET_CONNECT_STATE
  0x730122/16 B, GET_ACTIVE 0x730126/16 B; tabela 0x7301xx extraida em r387_ctrl_0073.txt): 4 heads, displayMask
  0x3f01, conectado 0x200, head 0 ativo com 0x200. O head esta certo.
- Linux/aberto: cl917d.h (open-gpu) -> metodo do canal core HEAD_SET_BASE_CHANNEL_USAGE_BOUNDS = 0x4D0 + head*0x300,
  bit 0 USABLE, bits 11:8 PIXEL_DEPTH (BPP_32 = 3). Com USABLE=FALSE o head ignora o canal base. nouveau gf119.c: o
  estado do core fica espelhado no BAR0, em uso em 0x640000+metodo (+0x300 por head), montagem em 0x660000+metodo;
  canal base em 0x641080.. (0x6410c0 = ISO, 0x641400.. = imagem).
- r387 tem NV2080 EXEC_REG_OPS (0x20800122, 0x20 B). tools/reg_probe le esses registradores (so leitura).
- reg_probe (EXEC_REG_OPS le os registradores do display, regStatus 0): BASE_CHANNEL_USAGE_BOUNDS head0 = 0x00220301
  (USABLE=1, BPP_32, LUTs 1025) -> a base ESTA liberada. Core: ISO 0x881000e0, 1920x1080, PARAMS 0xcf00, raster
  2200x1125 (o preto aceso e a superficie do core). Canal base head0: ISO em montagem 0xbee10008 (o nosso hIso) mas
  EM USO 0x0, tamanho 0 -> os metodos chegam na montagem e o UPDATE (0x80) nunca os arma. Proximo: registradores de
  excecao (nouveau gf119: 0x6101f0 + canal*12 = stat/dado/codigo; canal 1 = base head0) logo depois de um flip.

### 2026-11-10 01:46: a excecao do flip (PONTO DE RETOMADA)
- reg_probe durante o flip: excecao do canal 1 (base head0) stat 0x10005080 = tipo 5 INVALID_STATE no metodo 0x080
  (UPDATE), codigo 0x0000000d. Montagem do canal base correta (ISO 0xbee10008, SIZE 0x04380780, STORAGE 0x01001e00,
  PARAMS 0xcf00, PRESENT_CONTROL 0); em uso tudo zero. Core: usage bounds 0x00220301 (base liberada).
- Referencia aberta do flip: NVKMS 515.43.04 nvkms-evo2.c FlipBase90 (MIT), copia em
  scratchpad .../bcb2d0b1-.../scratchpad/nvkms515/. Ele escreve, antes do UPDATE: SET_CONTEXT_DMA_NOTIFIER=0,
  SET_NOTIFIER_CONTROL=0, SET_CONTEXT_DMA_SEMAPHORE/CONTROL/ACQUIRE/RELEASE=0, TIMESTAMP_ORIGIN/UPDATE_TIMESTAMP=0,
  SET_PRESENT_CONTROL = MIN_PRESENT_INTERVAL(minPresentInterval) | BEGIN_MODE NON_TEARING | MODE MONO,
  SET_CSC_RED2RED com OWNER (_BASE + matriz, ou _CORE), SURFACE_SET_OFFSET, SET_CONTEXT_DMAS_ISO, SIZE, STORAGE, PARAMS.
  O comentario cita "NV_DISP_BASE_STATE_ERROR_001" (IMMEDIATE + interlock com core): os codigos sao NV_DISP_BASE_STATE_ERROR_nnn.
- O nosso flipTo so manda 0x84=0, 0xC0, 0x400, 0x408, 0x40C, 0x410, 0x80. Suspeitos do codigo 0x0d, em ordem:
  (1) MIN_PRESENT_INTERVAL 0 com NON_TEARING (o NVKMS usa >= 1); (2) SET_CSC_RED2RED sem OWNER definido com o core em
  usage bounds de LUT 1025; (3) notifier/semaforo nunca zerados. Testar no tools/evo_base (sem reboot) antes de mexer
  no backend: acrescentar esses metodos, conferir com reg_probe se 0x6410c0 (ISO em uso) deixa de ser 0.

### MaxwellMetal 0.7.0: flip do canal base pela referencia aberta (compilado, AINDA NAO ENVIADO: hackintosh fora da rede)
- kext/MaxwellMetal/MaxwellBackend.cpp: pushFlip() no lugar dos tres flips curtos (flipTo, cliente de teste, volta para
  a home). Sequencia do NVKMS FlipBase90: notifier/semaforo/timestamps = 0, SET_PRESENT_CONTROL, CSC, LUT, OFFSET, ISO,
  SIZE, STORAGE, PARAMS, UPDATE. debug.mxflip (bits, padrao 0x07): 1 MIN_PRESENT_INTERVAL=1, 2 zera notifier/semaforo,
  4 CSC OWNER_BASE identidade (0x10000), 8 BASE/OUTPUT LUT = USE_CORE_LUT, 0x10 BEGIN_MODE IMMEDIATE, 0x20 CSC OWNER_CORE.
- Depois de cada flip le pelo RM (EXEC_REG_OPS em kernel): debug.mxflip_iso (0x6410c0, ISO em uso), debug.mxflip_exc
  (0x6101fc), debug.mxflip_code (0x610204); log "flip ... ARMOU / nao armou".
- Teste num boot so: sudo sysctl -w debug.mxflip=<bits>; sudo tools/abi_test 0xFFFF00FF 8; sysctl debug.mxflip_iso ...
- Enviar: tar de kext/MaxwellMetal/build/MaxwellMetal.kext para ~/maxwell_metal/kext/deploy/ no hackintosh; instalar com
  kext/troca/atualizar_accel.sh; conferir a AuxKC; sync && sudo reboot.

### 2026-11-10 19:20: a causa do INVALID_STATE e o LAYOUT DE MEMORIA (core block-linear x base pitch)
- 0.7.0 com a sequencia completa do FlipBase90 (mxflip 0x7): mesma excecao (0x10005080, codigo 0xd) -> nao era
  notifier/semaforo/intervalo/CSC.
- NVKMS EvoFlip90: needToReprogramCoreSurface -> quando a base passa a ter superficie, o core e reprogramado na mesma
  atualizacao com a MESMA superficie (HEAD_SET_OFFSET/SIZE/STORAGE/PARAMS/CONTEXT_DMAS_ISO). Core e base andam juntos.
- reg_probe (root; sem root o RM recusa: ctrl 0x1f, regStatus 4): core head0 STORAGE 0x00007804 = BLOCKLINEAR,
  BLOCK_HEIGHT 4 (16 GOBs = 64 linhas), PITCH 0x78 blocos de 64 B = 7680. Nossa base 0x01001e00 = PITCH, 0x1e*256.
  Core BASE_LUT_LO 0x84100000 (ENABLE, INTERPOLATE_1025_UNITY, NEVER_YIELD_TO_BASE), OUTPUT_LUT_LO 0x01000000,
  CONTEXT_DMA_LUT 0x88100102. Tamanho/formato iguais; so o layout difere.
- MaxwellMetal 0.7.1: debug.mxflip_storage (valor direto do SURFACE_SET_STORAGE), bit 0x40 (padrao agora 0x47) copia
  o STORAGE em uso no core, bit 0x80 ctxdma ISO sem KIND_PITCH, +1 MiB por superficie (1088 linhas), e o cliente de
  teste (abi_test) nao derruba mais a home ao fechar (antes: teardownLocked liberava o cliente RM e a home do MaxwellFB).
- Se armar: a tela mostra a home LADRILHADA (o WindowServer por CPU escreve linear). Caminho definitivo: superficies de
  scanout block-linear (nativas do NVK) no caminho Metal/IOP, ou copia linear->block-linear para o caminho por CPU.
- 0.7.1 no hackintosh (19:26): flip com storage 0x7804 igual ao core -> MESMA excecao 0xd. O layout sozinho nao basta.
- NVKMS nvkms-evo2.c (inicializacao do canal, ~linha 178): o canal base recebe uma vez SET_BASE_LUT_LO = USE_CORE_LUT e
  SET_OUTPUT_LUT_LO = USE_CORE_LUT ("mandar em todo flip dispara um erro em flips com tearing; o primeiro flip tem que
  ser sem tearing"). O core do NVDA tem BASE_LUT com NEVER_YIELD_TO_BASE; a nossa base ficava em DISABLE.
  tools/flip_variantes.sh testa mxflip 79, 15, 207, 72, 8, 107 sem reboot e para na primeira que arma.

### 2026-11-11 00:54: PRIMEIRA IMAGEM NA TELA (tela de bloqueio do macOS pelo MaxwellFB + canal base)
- flip_variantes.sh: a 1a variante ARMOU: mxflip 0x4f = sequencia do FlipBase90 + storage do core (0x7804) +
  SET_BASE_LUT_LO/SET_OUTPUT_LUT_LO = USE_CORE_LUT. ISO em uso = hIso da home. O que faltava era o USE_CORE_LUT
  (core com BASE_LUT NEVER_YIELD_TO_BASE). O registrador de excecao continua mostrando a excecao antiga (0xd): nao zera.
- Foto: imagem inteira no quarto de cima da tela (270 linhas), listras diagonais finas, resto branco. O head le a
  superficie como LINEAR com pitch = campo 0x78 * 256 B = 30720 B/linha (4x os 7680 reais): uma linha da tela a cada 4
  da imagem e depois le alem da superficie. Ou seja, o campo PITCH vale em unidades de 256 B mesmo com o bit de layout 0.
- Proximo teste (tools/flip_set.sh <mxflip> <storage>, sem reboot): 79 + storage 0x1e04 (7684); se nao armar,
  0x1e00 (7680) e mxflip 15 com storage 0 (0x01001e00).
- flip_set.sh 79/0x1e04 e 15/0 (0x01001e00): o ISO em uso continuou a home, mas o codigo de excecao passou de 0xd para
  0x12 -> esses flips foram RECUSADOS (a deteccao de "ARMOU" so olhava o ISO, que ja era a home). Regras ate aqui:
  0xd = LUT da base tem que ser USE_CORE_LUT; 0x12 = STORAGE da base diferente do core. Com STORAGE 0x7804 e ctxdma
  KIND_PITCH o head le linear com 0x78*256 = 30720 B por linha.
- MaxwellMetal 0.7.2: ensureHome le o STORAGE do core e aloca a home com esse stride real (30720 B x 1080 = 33 MB, com
  recuo para 7680 se nao couber); nvScanoutInfo devolve esse pitch e o MaxwellFB (0.2.2, sem mudar) o usa como
  bytesPerRow. Padrao debug.mxflip = 0x4f. "ARMOU" agora confere tambem o STORAGE em uso (0x64140c).
  Pendente para o caminho Metal/IOP: as superficies do acelerador/NVK tem pitch 7680 -> precisam do mesmo stride
  (ou de ctxdma/superficie block-linear de verdade).

### 2026-11-11 01:08: AREA DE TRABALHO EM TELA CHEIA na GTX 960 pelo MaxwellFB (sem OCLP, sem GeForceWeb)
- Boot com MaxwellMetal 0.7.2 + MaxwellFB 0.2.2 + MaxwellAGDC 0.4.2 + MaxwellAccel 0.4.1 + daemon mxfb-ws, CoreDisplay
  useIOP=NO/useMetal=NO (8_tela_legado.sh). Log: "home com stride 30720 B (core STORAGE 0x7804; era 7680)",
  "flip home (mxflip 0x4f ... storage 0x7804) ... excecao 0x80 codigo 0x0 -> ARMOU", MaxwellFB "pitch 30720 (33177600 B)".
  Foto: barra de menus, widgets, icones, Dock e cursor em 1920x1080. Composicao por CPU (Software compositor).
- Receita do flip do canal base na r387 com o core do NVDA: sequencia do NVKMS FlipBase90 + SET_BASE_LUT_LO/OUTPUT_LUT_LO
  = USE_CORE_LUT + SURFACE_SET_STORAGE igual ao HEAD_SET_STORAGE do core + superficie linear com stride =
  (campo PITCH do core) * 256 B.
- Papel de parede branco: e um Aerial (video, WallpaperAerialsExtension) e nao ha dispositivo Metal neste boot
  (debug.nvaccelfb 0, MetalPluginName retido) -> a camada de video nao e desenhada.
- Aviso "WindowServer encerrou inesperadamente": vem do kickstart -k do daemon mxfb-ws no boot; nao ha crash novo.
- Proximo: armar o Metal para apps (debug.nvaccelfb=1) e depois o compositor Metal + IOPresentment (6_metal_iop.sh),
  usando o caminho de COPIA do acelerador para a home (debug.nvaccel_iop_flip fica 0: as superficies do NVK tem pitch
  7680 e um flip direto delas sairia espremido).
- 2026-11-11 01:17-05:11: com debug.nvaccelfb=1 o WallpaperAerialsExtension abre o Metal ("NVMTL: vk: UP ... GeForce GTX
  960 (NVK GM206-A)") -> Metal para apps funciona junto com o MaxwellFB. Lentidao: VTDecoderXPCService a 217% de CPU
  (decodificacao do Aerial por software, i3-6100) + WindowServer a 98% (composicao por CPU de cada quadro do video,
  escrevendo 33 MB/quadro na home). Nao e crash: WallpaperAgent estavel. Paliativo: papel de parede estatico.
  Definitivo: compositor Metal + IOPresentment.
- 2026-11-11 08:58: papel de parede branco no modo por CPU. (1) "Tahoe" (NeptuneOneExtension) e Aerials sao video:
  filas de imagem, "enqueued: 10, displayed: 0" -> o compositor por software nao desenha. (2) Imagem estatica
  (tools/setwall, NSWorkspace; DefaultDesktop.heic nas 2 telas): o WallpaperImageExtension abre o Metal do NVK e fica em
  100% de CPU recriando o pipeline VTMTSComputeFunction1x1 (VideoToolbox Metal transfer), "family VRAM import refused
  (-1000072003) - private image + copies". Paliativo: nvmtl-allow-troca.txt nega o Metal aos processos Wallpaper*
  (kext/troca/atualizar_lista.sh). Pendente de verdade: o laco do VTMTS no plugin e a importacao de VRAM recusada.
- O WindowServer ve 2 telas no modo antigo: MaxwellFB em (0,0) e o NVDA@2 fantasma em (1920,0).

### 2026-11-11 09:10: teste do Metal total preparado (kext/troca/10_metal_total.sh, ainda NAO rodado)
- Estado conferido neste boot (so leitura): ordem dos AGDC = NVDADeviceControl (nosso) antes do NVDC; fbmap n=7 com o
  MaxwellFB no slot 6 (id 0x10000054a); MaxwellFB com IOAccelIndex 6; familia com 7 pipes (found_framebuffer 7 -> 7,
  ja conhecido). heads_piped 0 nao bloqueia: pipeFramebuffer() casa o pipe pelo ponteiro do framebuffer (pipe+0x98).
- A copia do performTransaction usa o pitch do backend (30720) na escrita: nada a mudar para o stride novo.
- 10_metal_total.sh: tira useIOP/useMetal, liga agdc_k5/agdc/nvaccelfb=3/nvaccel_iop=1, reinicia o WindowServer, repoe
  as preferencias do caminho por CPU 6 s depois (reboot volta para a tela que funciona), vigia 45 s (pid + iop_ok) e
  volta sozinho se o WS cair 2x ou nenhum quadro passar pelo pipe. Registro em ~/maxwell_metal/logs/metal_total/.

### 2026-11-11 09:10: WindowServer em loop ao reiniciar no caminho por CPU com o Metal ARMADO
- 09:10:29 `10_metal_total.sh volta` (versao 1) rodou em vez do teste: iop=0 + kickstart do WS com debug.nvaccelfb=1
  (armado desde 01:16). O WS novo carrega o NVMTLDriver ("Metal compositor activated."), mas o CoreDisplay esta em
  useIOP=NO: a tela e composta pelo CompositorSW -> CGLayerKitFetch -> CARenderOGLRender e aborta em
  CA::OGL::Context::push_surface (__assert_rtn; log "context(2) transient surface 0x... not found"). 16 quedas, launchd
  passa a espacar os spawns.
- No boot das 01:06 funcionou porque o WS subiu ANTES do arme (MetalPluginName retido). Regra: no caminho por CPU o
  WindowServer nao pode ter dispositivo Metal. O arme nao tem volta sem reboot (sysctl so arma).
- Correcao no script (sem kext): volta() instala a lista do plugin com `-WindowServer` antes do kickstart; o teste
  instala a lista com `!WindowServer`. volta() confere se o pid do WS fica estavel e, se nao, manda reiniciar.

### 2026-11-11 09:14-09:18: o "teste" travou a maquina sem testar nada (watchdog panic)
- 10_metal_total.sh rodou com o WindowServer ja em loop de SIGABRT (secao anterior). Relatorio .spin das 09:16:
  "Launchd throttled processes: WindowServer throttled after SIGABRT"; launchctl kickstart -k parado 67 s numa chamada
  xpc; nenhum WindowServer vivo; threads do kernel (NVAccel, NVDA, IOGraphics) ociosas. 09:17:59 panic "userspace
  watchdog timeout: no successful checkins from WindowServer in 120 seconds" -> reboot sozinho. O caminho Metal + IOP
  NAO chegou a ser exercitado.
- O script tinha apagado useIOP/useMetal e nao chegou a repor: o boot das 09:18 subiu no caminho novo (sem pipe, tela
  preta), desarmado, WS estavel. Os logs do `log stream` redirecionado se perderam (buffer); o log persistido tambem
  nao tem 09:14:57-09:17:59.
- Script v3: recusa rodar com o WS caido/caindo (pid estavel 8 s), kickstart em segundo plano com prazo de 25 s,
  preferencias do caminho por CPU repostas assim que o pid novo aparece (+sync), registro por `script -F` (linha a
  linha), volta() com a lista -WindowServer e aviso de reboot se o WS nao ficar de pe.

### 2026-11-11 09:22: METAL TOTAL SUBIU (WindowServer estavel no Metal + IOPresentment), tela preta
- Boot limpo, 10_metal_total.sh v3: WS pid 1144 estavel, "Metal compositor activated.", plugin "vk: UP ... GTX 960
  (NVK GM206-A)", IOPresentment "Valid ADGC node found", "Successfully created presentment object type: 1024",
  "Capabilities conversion successful" (so o MaxwellFB; os 6 NVDA dao 0x80e). k5_link 9 / k5_caps 9.
- performTransaction chamado: 101 quadros BGRA 1920x1080 copiados para a home nos primeiros ~4 s, todos de uma cor so
  (64 amostras pretas), vindos da memoria de sistema da IOSurface. Depois de ~28 s parado, o WS passa a apresentar as
  superficies 0x1 e 0x4 em formato 'l10r' (0x6c313072, A2R10G10B10) a ~48/s e a copia recusava ("is not BGRA"): 15017
  falhas. Tela preta o tempo todo.
- BUG nosso achado: nvAccelSurfVram zera p->hSrcClient/hSrcMemory (herdado do original, onde sao so saida) ANTES de
  usar os dois como entrada do porte r387 (device do alvo + handle novo da libnvrm387). O backend recebia 0/0 ->
  NVRM_SS_OFF -> o plugin loga "family VRAM import refused (-1000072003)" em TODA superficie e cai em "private image +
  copies". O autoteste de 2026-11-08 nao pegou porque vai direto no backend. Mesmo erro visto no WallpaperImageExtension.
- MaxwellAccel 0.4.2 (compilado e enviado, falta instalar): debug.nvaccel_surfshare (0 = como antes, 1 = passa as
  entradas; debug.nvaccel_surfshare_seen), debug.nvaccel_iop_l10r (padrao 1 = converte l10r -> BGRA na copia; 2 = cru),
  debug.nvaccel_iop_last_fmt/_nz/_varied/_first e nvaccel_iop_l10r_frames. O 10_metal_total.sh aceita SURFSHARE=1 e L10R=n.
- Em aberto: se os 101 quadros pretos eram pretos de verdade (WS sem nada para desenhar) ou se o "surface-out" do
  plugin (copia GPU -> paginas da IOSurface importadas por host pointer) nao chega na memoria de sistema.

### 2026-11-11 09:32: AREA DE TRABALHO COMPOSTA PELO METAL DA GTX 960 (MaxwellAccel 0.4.2)
- 10_metal_total.sh no boot limpo: WS pid 958 estavel, 0 falhas no pipe. 4 quadros BGRA pretos no inicio, 1 quadro BGRA
  com imagem (nz 48/64), e a partir do login (t=36 s) tudo em 'l10r' convertido para BGRA na copia: nz 64/64 var 64/64.
  Autor: "area de trabalho apareceu, consigo usar o wallpaper que nao ia, mas o PC ta lento". debug.nvaccel_surfshare
  ficou 0 (plugin em "private image + copies").
- Os quadros pretos do teste das 09:22 eram pretos de verdade; a causa da tela preta era so a recusa do l10r.
- Lentidao medida (09:34-09:36): ~15 quadros/s no pipe durante animacao; WindowServer 95-135% de CPU, ~80% em espaco
  de usuario numa thread so (0x1f7c, prioridade 37): o plugin compilando variantes do shader do CoreAnimation
  `fixed_frag_lpf_cpf` (685 KB de AIR, especializado por function constants): 23 s, 7 s, 10 s e depois ~5 s CADA
  variante ("fc cache: miss ... stored", "pipeline key: fragment fixed_frag_lpf_cpf ... fc TimgXcu_Iscd"). Fica em
  cache no disco (/var/folders/zz/.../C/WindowServer/nvmtl-mesa + fc cache do plugin).
- Custo fixo por quadro: "surface readback (imported pages)" ~10 ms (GPU -> paginas da IOSurface) + copia no kernel
  com conversao l10r para a home (stime do WS ~25% de um nucleo). VTDecoderXPCService 74% com papel de parede em video.
- Proximo para desempenho: flip sem copia (debug.nvaccel_iop_flip=1) exige (a) superficie do WS na VRAM da familia
  (debug.nvaccel_surfshare=1, ainda nao testado), (b) formato que o canal base 927C aceita (o WS usa l10r =
  A2R10G10B10; o 927C tem A8R8G8B8 e A2B10G10R10: ou fazer o WS ficar em BGRA, ou trocar R/B no plugin), (c) layout
  igual ao do core (STORAGE 0x7804): imagem block-linear nativa do NVK com 16 GOBs, ou linear com stride 30720.

### 2026-11-11 13:53: 4h21 de WindowServer no Metal; onde esta a lentidao; desenho do flip sem copia
- WS pid 958 de pe desde 09:32 (192883 quadros no pipe, 0 falhas). So 1102 quadros l10r (o primeiro minuto): em regime
  o WS apresenta BGRA. Nenhuma compilacao de shader nos ultimos 10 min (cache em disco funcionando).
- Regime com papel de parede em video: 13 quadros/s; VTDecoderXPCService 255% de CPU (decodificacao por software,
  load 8.3 num i3 de 2 nucleos); WS 30% (utime ~24 ms/quadro, stime ~3 ms/quadro: a copia no kernel NAO e o gargalo).
- Leitura do codigo: o plugin (metal/plugin-lib, NVMTLDevice.m nvmtl_surface_image_create) cria a textura de IOSurface
  como "buffer alias" DRM linear com row = IOSurfaceGetBytesPerRow (7680) sobre a VRAM da familia importada por
  nvmtl_vk_surface_vram (NVK stage import -> NVRM_ESC_SURF_VRAM). O flip do canal base exige linear com stride 30720
  (ctxdma KIND_PITCH + STORAGE 0x7804) ou block-linear nativo de 16 GOBs (pitch 0x78 blocos, igual ao core).
- Desenho escolhido (a implementar depois do teste abaixo): "linha larga" so para as superficies de tela do WS
  (WindowServer + RenderTarget + dimensoes do scanout): VRAM da familia de 30720*h, o plugin usa row 30720 e marca a
  IOSurface com uma tag; o acelerador so faz flip de superficie marcada, senao copia. l10r continua pela copia.
- O plugin foi compilado em 2026-11-06 sem script salvo (metal/out, metal/stage/nvmtl); recriar a partir de
  ref/nullmoth/build/build_plugin.sh antes de mexer nele.
- Antes de implementar: provar o pre-requisito (importacao da VRAM da familia) com SURFSHARE=1 e medir.
  10_metal_total.sh ganhou PERFIL=1 (sample do WS antes/depois + quadros/s), FLIP=n e a volta automatica se o ultimo
  quadro vier preto.

## 2026-11-11 15:31-15:50 — teste SURFSHARE=1: dois bloqueios achados, acelerador 0.4.3 e NVK corrigido preparados

Teste `SURFSHARE=1 PERFIL=1 10_metal_total.sh`: WindowServer estavel (pid 4625), quadros chegando, 25 quadros/s na tela de login
(l10r). O autor viu "varios cursores" e atraso grande na tela de login; a causa dos cursores nao foi achada.

1. **A importacao continuou recusada com o kernel dando certo.** O backend fez o dup (`surfshare: VRAM ... : 0x0`), mas o NVK
   faz depois o proprio `RM_DUP_OBJECT` com `hObject = 0` (na r615 o RM escolhe o handle). A r387 devolve
   `NV_ERR_INVALID_OBJECT_HANDLE (0x33)`. Reproduzido fora do WS com `nvmtlrender 6` + `MESA_LOG_FILE`.
   Correcao: `libnvrm387/src/nvrm387.c`, `esc_dup` gera o handle (`auto_handle()`) quando vem 0. NVK relinkado
   (`PATH=~/.rustup/toolchains/stable-aarch64-apple-darwin/bin:nvk/venv/bin:$PATH ninja -C nvk/build-x86`, depois
   `codesign -s - -f`). Teste sem instalar: `DYLD_LIBRARY_PATH=<pasta com o dylib novo>` faz o dlopen do plugin pegar o NVK novo.
   Resultado: `item 7: surface 5 64x64 pitch 256 -> the family's VRAM (vram 1, private 0)`.
2. **Com a importacao funcionando, ler a superficie pela CPU devolve zeros.** O page-off (VRAM -> paginas da IOSurface) usa o
   copy engine pelo backend (`nvSurfPageoff`), que nao foi portado no MaxwellMetal: `surfvram pageoff: ... -> 0x80000002`.
   Teste: `metal/render/surfrt.m` (no hackintosh `~/maxwell_metal/render/surfrt/mtlprobe LARG ALT QUADROS`): desenha numa
   IOSurface e le com IOSurfaceLock. Ligar o compartilhamento com o 0.4.2 quebraria toda leitura por CPU, inclusive a copia de tela.
3. **Efeito colateral do 0.4.2:** cada tentativa de importacao reservava a VRAM da superficie (`IOAccelVidMemory::wire`) antes de
   recusar. As 15:45: `nvaccel_vm_alloc_bytes` 1,23 GB contra 164 MB liberados; um pedido de 8 MB (1920x1080) voltou
   `NVRM_SS_NO_VRAM`. Com SURFSHARE=1 e o NVK velho, cada tentativa ainda deixava um dup pendurado no cliente do NVK.

MaxwellAccel **0.4.3** (compilado, em `kext/troca/`, ainda nao instalado):
- `debug.nvaccel_surfshare=0` recusa antes de reservar VRAM (2 = comportamento do 0.4.2);
- `NVResource::pageoff`: quando o backend recusa o copy engine, copia pela CPU do mapeamento de kernel da VRAM (`fKva`) para as
  paginas da IOSurface; contadores `debug.nvaccel_pageoff_cpu`, `_cpu_us`, `_cpu_kb`.
`atualizar_accel.sh` agora tambem instala o NVK de `kext/gpubundles/nvmtl` quando difere do instalado (backup em `backup/nvmtl.antes`).

Proximo: instalar, reiniciar, rodar `surfrt` com `debug.nvaccel_surfshare=1` (medir o custo do page-off pela CPU em 1920x1080)
e so entao o WindowServer com SURFSHARE=1. O flip sem copia continua dependendo da linha larga (30720) no acelerador e no plugin.

Perfil de CPU do WS (sample, 4 s): na area de trabalho, `swtch_pri` 2783 amostras (alguma thread em espera ativa) e
`nvmtl_vk_image_write` 472; na tela de login, `memmove` 412 (buffer de 33 MB `newBufferWithBytesNoCopy` recopiado a cada
submissao, porque o vkAllocateMemory recusou importar o ponteiro).

## 2026-11-11 (depois do reboot) — 0.4.3 carregado: importacao ok em 1920x1080, page-off pela CPU lento demais para tela

Estado no boot: MaxwellAccel 0.4.3, MaxwellMetal 0.7.2, MaxwellFB 0.2.2, NVK md5 4a8369a3...; `nvaccel_vm_alloc_bytes` 56 MB.
Sem `debug.nvaccelfb=1` o `mtlprobe` nao ve dispositivo Metal (mesmo estando liberado na lista).
Com `debug.nvaccelfb=1 debug.nvaccel_surfshare=1` (`render/surfrt/mtlprobe LARG ALT 4`):
- 320x240 (300 KB): PASSOU; `item 7: surface 9 320x240 pitch 1280 -> the family's VRAM`; lock ~12 ms.
- 1920x1080 (8,1 MB): PASSOU; backend `surfshare: VRAM ... : 0x0`; lock **~540 ms** (`nvaccel_pageoff_cpu_us` 547413 para 8100 KB).
- O page-off pela CPU le a BAR a ~15 MB/s (uma transacao PCIe a cada 8 bytes). Serve como rede de seguranca para leitura
  rara, nao para a tela: WindowServer com SURFSHARE=1 e sem flip ficaria em ~2 quadros/s. Teste de WS com SURFSHARE=1 pulado.
- Conclusao: surfshare so compensa junto com o flip sem copia (linha larga) ou com o page-off pelo copy engine portado.
- Armadilha de teste: o shell remoto e zsh; `$s` com "1920 1080 4" vai como UM argumento (o teste caiu no padrao 256x256).

## 2026-11-11 (noite) — flip sem copia: linha larga implementada (MaxwellAccel 0.4.4, MaxwellMetal 0.7.3, plugin novo); falta instalar e testar

Desenho (o de 13:53), agora em codigo:
- **Acelerador 0.4.4** (`accel/src/nvrm-accel.cpp`, copia anterior em `nvrm-accel.cpp.0.4.3`):
  - publica `NVAccelWide` no servico NVAccel (pitch | largura << 32 | altura << 48) quando o scanout do MaxwellFB tem stride maior
    que 4*largura (30720 em 1920x1080). Publicado no arme (`debug.nvaccelfb`), em initFramebufferResource e em cada quadro copiado.
  - `NVRM_ESC_SURF_VRAM` com o bit `NVRM_SS_PLANE_WIDE` (0x20000000) no plane e size = pitch*altura: superficie BGRA do tamanho do
    scanout, ainda sem VRAM, ganha VRAM de pitch*altura (`NVVidMemory::fWidePitch/fWideBytes`; allocPhysical pede o maior).
    Recusa: `NVRM_SS_TOO_SMALL`. Pedido sem o bit para superficie larga: `NVRM_SS_OFF`.
  - `debug.nvaccel_surfshare=3`: so pedidos de linha larga sao atendidos (o resto fica privado, como com 0).
  - flip (`nvAccelIopFlip`): usa o stride largo; superficie com outro stride nao vai por flip (`nvaccel_iop_flip_geom`).
  - page-off de superficie larga: CPU, linha a linha (`nvaccel_wide_pageoff`). Lento (~540 ms por quadro): so rede de seguranca.
  - `debug.nvaccel_iop=0` leva o painel de volta para a home e solta as superficies do flip (`nvAccelIopHomeAll`).
  - sysctls novos: `nvaccel_wide` (1), `nvaccel_wide_pitch`, `nvaccel_wide_granted`, `nvaccel_wide_refused`, `nvaccel_wide_pageoff`,
    `nvaccel_iop_flip_fmt`, `nvaccel_iop_flip_geom`.
- **MaxwellMetal 0.7.3** (`kext/MaxwellMetal/MaxwellBackend.cpp`, anterior em `.0.7.2`): do 4o flip de superficie em diante o flip e
  rapido (sem o IOSleep(60) nem a leitura de registradores; o STORAGE do core fica em cache). `debug.mxflip_count`,
  `debug.mxflip_stuck` (canal base nao consumiu em 200 ms: o flip volta erro e o acelerador copia).
- **Plugin** (`metal/plugin-lib`, copias `*.2026-11-07`; build salvo em `metal/build_plugin.sh`, saida `metal/out-lib/NVMTLDriver`):
  no WindowServer, textura de IOSurface com RenderTarget e o tamanho do scanout e superficie de tela: BGRA pede a linha larga e
  cria a imagem com row 30720; recusada (ou l10r) fica com imagem privada (nunca VRAM compartilhada com o stride normal).
  `NVMTL_WIDE_TEST=1` faz qualquer processo valer como WindowServer nesse teste. O log de newTextureWithDescriptor mostra o usage.
- Script: `sudo SURFSHARE=3 FLIP=1 PERFIL=1 kext/troca/10_metal_total.sh` (volta sozinho se `mxflip_stuck` > 0 ou se os quadros
  cairem no page-off de linha larga).
- Empacotado e copiado para o hackintosh (nada instalado): `kext/troca/MaxwellAccel.kext` 0.4.4, `kext/deploy/MaxwellMetal.kext`
  0.7.3, `kext/gpubundles/NVMTLDriver.bundle` (uuid 07496283). Anteriores no hackintosh: `.0.4.3`, `.0.7.2`, `.2026-11-07`.

Teste depois de instalar e reiniciar: armar (`sudo sysctl debug.nvaccelfb=1 debug.nvaccel_surfshare=3`), depois por ssh
`NVMTL_WIDE_TEST=1 render/surfrt/mtlprobe 1920 1080 4` (pedido largo, desenho com row 30720 e leitura linha a linha), e so entao o WindowServer.
Nao verificado: se o WindowServer pede RenderTarget nas superficies de tela; se o canal base aceita flips seguidos.

### 2026-11-11 23:40-23:55 — 0.4.4/0.7.3 no ar: linha larga validada fora do WindowServer; 0.4.5/0.7.4 preparados
- Boot limpo com 0.4.4 + 0.7.3. No arme: `linha larga: scanout 1920x1080 stride 30720 -> publicada (NVAccelWide)`.
- `surfshare=3` + `NVMTL_WIDE_TEST=1 mtlprobe 1920 1080 4`: acelerador atendeu (VRAM 34275328 B, stride 30720) mas o backend devolveu
  `NVRM_SS_OFF`: o modo 3 nao repassava hClient/hObject (so o modo 1). Corrigido na **0.4.5**.
- `surfshare=1` + o mesmo teste: PASSOU. `buffer texture: 1920x1080 VkFormat 44 pitch 30720 bound at 0 (DRM linear ... render 0)`,
  `wide row: display surface 12 -> the family's VRAM with a 30720-byte row`, 4 quadros lidos certos pelo page-off linha a linha
  (~485 ms cada; `nvaccel_wide_pageoff` 4). O formato linear nao aceita RenderTarget no NVK ("drop RenderTarget"): o plugin
  desenha em outra imagem e copia pela GPU para a linear (render 2-8 ms contra 1,3 ms da imagem privada).
- Orcamento de BAR1 do MaxwellMetal: 160 MB (BAR1 = 256 MB); cada superficie larga ocupa 34 MB e a home outros 34.
  **0.7.4**: `debug.mx_vram_mapped_kb`, `debug.mx_vram_refused`. Nao mexi no orcamento sem medir.
- `nvaccel_vm_dealloc*` nao conta a liberacao por `NVVidMemory::free()`: `vm_alloc_bytes` so cresce, nao e vazamento provado.
- Sintomas do autor nesse boot: Find My sem Metal (abriu no login antes do arme) e papel de parede branco (processos subiram com
  carga 76; reiniciados por ssh, video "Rendering with success").

## 2026-11-12 00:00 — primeiro teste do flip sem copia na tela (0.4.5 + 0.7.4): funciona, mas so 3 superficies couberam

`sudo SURFSHARE=3 FLIP=1 PERFIL=1 10_metal_total.sh`: WindowServer estavel (pid 956), tela certa (autor). Em 100 s:
- 16 quadros por flip (`iop: ASYNC swap ... pitch 30720 scanned out of its own VRAM`), `mxflip_stuck` 0, `flip_refused` 0, o canal
  base armou (`flip superficie ... storage 0x7804`). O flip em si esta provado.
- ~300 quadros BGRA foram por copia (`nvaccel_iop_flip_unwritten` 301: superficie com imagem privada) + 124 l10r. 21-28 quadros/s.
- Causa: o WindowServer alterna entre 7+ superficies de tela (ids 0x5 0x7 0xa 0xd 0xe 0xf 0x11 0x13 ...), nao 2 ou 3. Cada
  superficie larga = 34 MB de BAR1 (todo nvAllocVram mapeia no kernel); com a home (34 MB) e ~26 MB de outras, so 3 couberam no
  orcamento de 160 MB (`mx_vram_mapped_kb` 152320, `mx_vram_refused` 13, `allocPhysical ... REFUSED (0xe00002bd)`).
- Cada troca copia <-> flip passa pela home com o caminho de diagnostico (IOSleep 60 ms).
- Parada: 0 quadros/s e WindowServer 0% com a tela parada (bom).

Correcao preparada (compilada e copiada, nao instalada): **MaxwellMetal 0.7.5** + **MaxwellAccel 0.4.6**
- `NVRM_VRAM_FLAG_NOMAP` no pedido de VRAM: superficie de linha larga sem mapa permanente na BAR1 (kva NULL). O backend devolve
  como phys o endereco de uma reserva comum mapeada (34 MB, uma so), porque a familia le o campo +0xc8 da vidmem em
  `IOAccelVidMemory::getPhysicalSegment`. Orcamento das sem mapa: 640 MB. `debug.mx_vram_unmapped_kb`, `debug.mx_vram_temp_maps`.
- `nvMapVram` / `nvUnmapVram`: mapa temporario para o page-off de linha larga (`debug.nvaccel_wide_tempmap`).
- `debug.nvaccel_wide_nomap` (1). `nvAccelRestoreParkedForDirty` aceita vidmem larga sem kva.
- flip para a home tambem rapido depois dos 3 primeiros.
Nao verificado: se a familia tolera a vidmem com o phys da reserva; quantas superficies de tela o WindowServer mantem (VRAM = 34 MB cada).
Ideia melhor para depois: imagem block-linear nativa (8 MB por superficie, desenho direto, sem a copia linear na GPU).

## 2026-11-12 00:13-00:34 — segundo teste do flip (0.4.6 + 0.7.5): quase tudo por flip; 8 min depois VRAM cheia, WS aborta, loop, panic

- `sudo SURFSHARE=3 FLIP=1 PERFIL=1 10_metal_total.sh`, WindowServer pid 878 de 00:13:41 a 00:21:44. Tela certa (autor).
- A correcao da BAR1 funcionou: linha larga recusada 0, `sem mapa` 100 MB aos 10 s e 267 MB (8 superficies) aos 40 s e 100 s,
  BAR1 109 MB. 627 flips em 100 s, 0 travados, 0 recusados; `flip superficie #6144 (rapido)` as 00:19:50. A familia aceitou a
  vidmem com o phys da reserva. Com a tela parada: 0 quadros/s.
- Com video rodando: ~32 pedidos de linha larga por segundo (`atendidos 1024` as 00:17:30, `2048` as 00:18:02): o WindowServer
  embrulha de novo a superficie de tela a cada quadro (escape + dup + importacao + imagem), e ha superficies de tela novas (ids ate 0x82).
- 00:20:20: `vk: colour image: VRAM full and no system-memory type takes it -> -13`, `image create FAILED (1920x1080)`,
  `Failed to allocate MTLTexture from IOSurface`. O plugin so segurava 247-288 MB (`free 1326 MB` na conta dele): quem encheu a VRAM
  foi alocacao fora dele. Suspeita principal: as superficies largas do kernel (34 MB cada, contiguas, ate 640 MB sem mapa + 160 MB
  com mapa) e/ou VRAM de superficie que nao volta. NAO medido: os contadores se perderam no reboot.
- 00:21:44: WindowServer aborta em `MetalBacking::AbortWithTextureInfo` <- `MetalIOSurfaceBacking::PrepareForUse` <-
  `CaptureSurfaceMetal::Populate` (captura de janelas pediu textura e veio nil).
- Depois: o launchd sobe o WindowServer com as preferencias de CPU e o Metal ainda armado -> 13 quedas em 3 min
  (`layer_blit_byte_ARGB8_RGB8`, `CA::OGL::SW::scanline`), panic do watchdog as 00:34:39 (120 s sem checkin). A licao de 11-11 de novo.
- Feito: `kext/troca/ws/guarda_ws.sh` (vigia; o script 10 liga depois do RESULTADO): quando o WindowServer do Metal morre, troca a
  lista para `-WindowServer`, grava as preferencias de CPU, desliga flip e IOPresentment e registra os contadores em
  `/var/log/maxwell_guarda_ws.log`. Nao testado.
- A fazer antes de repetir: (1) teste de vazamento por ssh (`NVMTL_WIDE_TEST=1 mtlprobe` varias vezes e ver `mx_vram_unmapped_kb`
  voltar a 0); (2) teto baixo de superficies largas e VRAM nao contigua para elas; (3) o plugin nao deveria refazer a importacao
  a cada quadro (cache por IOSurface para superficie com VRAM da familia).
- Caminho por CPU depois do boot: VTDecoderXPCService 213% + WindowServer 100% por horas (papel de parede em video decodificado
  por software), carga 46 no i3. Papel de parede estatico alivia.

## 2026-11-12 09:30 — depois do travamento: testes por ssh, causa provavel, MaxwellMetal 0.7.6 e NVK novo preparados

Testes por ssh (armado, `surfshare=3`, `NVMTL_WIDE_TEST=1`):
- 6 execucoes do `surfrt/mtlprobe 1920 1080`: `mx_vram_unmapped_kb` volta a 0 em todas; BAR1 94400 KB (60928 + a reserva comum).
  A VRAM de linha larga E devolvida quando a superficie morre.
- `render/surfloop/mtlprobe 1920 1080 1500 1` (fonte `metal/render/surfloop.m`: 1500 texturas novas sobre a MESMA IOSurface, uma
  por volta, desenhando): 15 ms por volta, 5 descritores abertos do inicio ao fim, nenhuma falha, VRAM de volta. A reimportacao
  por quadro, sozinha, nao vaza.
- Releitura do log da queda: so ~2000 a 3000 importacoes ate a falha (nao e contagem); `vkAllocateMemory` devolveu
  OUT_OF_DEVICE_MEMORY de verdade (sem "soft VRAM gate"); segundos antes ha ~30 superficies de tela NOVAS (ids 0x64-0x82,
  `Failed to allocate MTLTexture from IOSurface 0x71 ...`). 30 x 34 MB = 1 GB. Causa provavel: rajada de superficies de tela
  com video/captura, cada uma virando linha larga, ate o teto de 640 MB + 160 MB de BAR1, somado ao resto da VRAM em uso.
  Nao provado (sem contadores do momento).

Preparado (copiado, nao instalado):
- **MaxwellMetal 0.7.6**: teto das superficies sem mapa 352 MB (10 de 1920x1080); pedido sem mapa recusado nao cai mais para a BAR1.
- **libnvrm387** (`src/nvrm387.c`, anterior em `.2026-11-11`), 83 testes ok, NVK relinkado (md5 c3be8155...):
  - handles automaticos com 24 bits de sequencia, faixas 0xD4xxxxxx / 0xD5xxxxxx (eram 16 bits: davam a volta em ~35 min de
    video e o handle novo cairia em cima de objeto vivo);
  - `obj_del` devolve TOMB a FREE no fim das sequencias (sem isso, depois de ~65 mil objetos toda busca que falha varria a tabela).
  Testado no hackintosh sem instalar (`DYLD_LIBRARY_PATH=~/maxwell_metal/nvk_teste`): surfloop 1000 ok, surfrt PASSOU, e o
  page-off por mapa temporario (`nvMapVram`) funcionou (lock ~550 ms).
- `guarda_ws.sh` ja esta no script 10.

## 2026-11-12 10:36 — terceiro teste (0.4.6 + 0.7.6 + NVK c3be8155): todos os quadros por flip; o gargalo agora e CPU do driver

- WindowServer pid 875 no Metal. 2235 flips em 100 s, `flip_unwritten` 0, `flip_refused` 0, `mxflip_stuck` 0, linha larga recusada 0,
  `mx_vram_unmapped_kb` 301248 (9 superficies; teto 10), BAR1 100416 KB. So os 151 quadros l10r da tela de login foram por copia.
- Com a carga do login (load 123): 16 quadros/s, WS 120%. Assentado (load 6): 30 quadros/s = 30 flips/s, WS 46%, com video no Chrome.
- o autor: "tela certa mas ainda nao 100% fluido".
- Perfil do WS (sample 4 s): alem das esperas, `nir_validate_shader`/`validate_*` espalhados (o NVK e build `debugoptimized`: cada
  passo do NIR e validado; e a compilacao de shader que leva 5-23 s), `do_free` da libnvrm387 231 amostras (purge_tree varre as
  65536 entradas a cada FREE), `swtch_pri` 1573 (thread em espera ativa, ainda sem dono).
- Preparado (userland, sem kext): plugin uuid 511ABFD1 com `setenv("NIR_DEBUG","novalidate",0)` antes de carregar o NVK;
  libnvrm387 sem a varredura no FREE de memoria/dup/ctxdma/VA (83 testes ok); NVK md5 db019ea0. surfloop 500: 3,6 -> 2,7 ms por volta.
  Instalar com `sudo kext/gpubundles/install_gpubundles.sh` (sem AuxKC, sem reboot) e repetir o script 10.
- Script 10: encerra o vigia anterior antes de reiniciar o WindowServer (ele dispararia no meio da troca).

## 2026-11-12 10:50-11:05 — pilha leve no ar; investigacao do video por hardware (so leitura)

Pilha leve (plugin 511ABFD1 + NVK db019ea0) instalada com `install_gpubundles.sh`, WindowServer pid 6351 no Metal + flip:
10:55, video no Chrome: 30 quadros/s = 30 flips/s, WS 41%, `flip_unwritten` 0, `mxflip_stuck` 0, sem mapa 301248 KB estavel,
nenhuma recusa. O WindowServer anterior (875) rodou 14 min / 22720 flips sem cair. O script 10 encerrou o vigia antigo e ligou o novo.

Video por hardware:
- **O RM r387 da GTX 960 oferece o decodificador.** Lista de classes (56, `GPU_GET_CLASSLIST`, lida com `NVRM387_LOG=1` e um LOG novo
  na libnvrm387): ... 0xb06f 0xb0b5 0xb197 0xb1c0 **0xb6b0** **0xd0b7**. 0xB6B0 = `NVB6B0_VIDEO_DECODER` (NVDEC da GM206),
  0xD0B7 = `NVD0B7_VIDEO_ENCODER` (NVENC). O `NVDAResmanWeb` tem `RMNvdecUseAltUcodeImage` (o RM carrega o firmware do motor) e o
  HAL GM100 tem as strings NVDEC/MSVLD.
- **O decodificador do NullMoth nao esta no codigo-fonte.** O plugin (`NVMTLDevice.m`, `nvmtl_nvdec_register`) so faz
  `dlopen` de `libnvdec_h264.dylib` / `libnvdec_h264_t2d.dylib` / `libnvdec_hevc.dylib` ao lado do binario e registra a fabrica
  (`NVDecH264_CreateInstance`, `NVDecHEVC_CreateInstance`) no VideoToolbox (`VTRegisterVideoDecoderWithInfo`, rating 1000). Essas
  bibliotecas so vem no pacote binario das releases (`tools/make_nvdec_t2d.py` troca 1 byte do binario para o layout "classic").
  O kext dele publica `class-nvdec` por geracao (Turing+).
- Nao sei: como essas bibliotecas falam com o RM (user client direto ou funcoes do NVK, que passariam pela nossa libnvrm387) e
  se o formato de quadro que montam serve para a classe B6B0 (sao feitas para C4B0 em diante). So inspecionando o binario
  (precisa baixar o pacote de release: pedir ao autor).
- Se nao der para reaproveitar: escrever o nosso (parser de H.264, struct de quadro do NVDEC, canal no motor de video pela
  libnvrm387, fabrica do VideoToolbox). Trabalho grande, de semanas.
- Obs.: `nvk/build-x86` tem o LOG da lista de classes; `nvk/out` e `kext/gpubundles` continuam no db019ea0 (sem o LOG).

### 2026-11-12 11:02 — decodificador do NullMoth inspecionado (pacote v2.9, so leitura estatica)
- Baixado com OK do autor: `ref/nullmoth-release/v2.9/nullmoth-nvidia-2.9.tar.gz` (68,8 MB, SHA-256 b624bf07... confere com o
  SHA256SUMS.txt). Extraidas so as 3 bibliotecas em `ref/nullmoth-release/v2.9/libs/`. Nada executado nem instalado.
- `libnvdec_h264.dylib` / `_t2d` (80 KB, diferem em 1 byte de codigo + assinatura) e `libnvdec_hevc.dylib` (98 KB): x86_64,
  autocontidas. Cada uma traz embutida a propria copia do transporte do NullMoth (`nvrm_xnu_*`, `nvRmApi*`): abre o servico
  IOKit `NVAccel` ou `NVRM` e manda os escapes do RM r615 por `IOConnectCallMethod`. Nao passam pelo NVK nem pela libnvrm387.
- Sequencia de abertura (pelas mensagens): card info -> root client -> id info -> device -> subdevice -> classlist
  ("no NVDEC class in classlist") -> **usermode** -> usermode map -> vaspace -> ctxdma -> channel -> schedule -> **token idx**
  -> decoder object. Ou seja: envio de trabalho por doorbell + work submit token (Volta em diante). Descreve-se como
  "NVDEC H.264 decoder (RTX 5060 NVCFB0)".
- Tem parser proprio de H.264 com limites declarados: so 8 bits 4:2:0, sem entrelacado, sem matrizes de escala, sem FMO.
- A saida e lida pela CPU: a lib trava o CVPixelBuffer e copia luma/croma "destilando" (`nvd264_detile`, modo classic/Blackwell).
- Conclusao: reaproveitar o binario exigiria (1) desviar o IOKit dele para um tradutor r615->r387, (2) emular o objeto usermode e
  o token e redirecionar a USERD, (3) remendar a lista de classes para aceitar 0xB6B0, tudo num binario fechado. Nao recomendo.
  Caminho viavel: decodificador nosso, usando a libnvrm387 (ja fala r387 e ja envia por GP_PUT na USERD), com a sequencia acima
  como roteiro e as structs publicas do NVDEC (nvdec_drv.h do L4T / FFmpeg nvtegra). Da para desenvolver como ferramenta de
  linha de comando por ssh, sem tocar na tela.
- Primeiro passo barato: sonda que cria cliente/device/canal e aloca o objeto 0xB6B0, para ver se o RM r387 sobe o motor
  (firmware) quando pedido de fora do driver da NVIDIA. Ainda nao feito (mexe no RM com a area de trabalho em cima: pedir OK).
