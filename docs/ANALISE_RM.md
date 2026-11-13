# Fase 3a: análise estática do RM r387 (Web Driver 10.3.3)

Data: 2026-11-06. Binários: `bins/` (copiados do hackintosh, 26.7.2 25G332, só leitura).
Desmontagem: `analise/resman.otool`, `analise/geforce.otool`, `analise/geforce_rmcalls.txt`.
Lado r615/NullMoth: `ref/`, `tools/r615_layout.txt`. Protótipos e structs: [`nvrm_r387.h`](nvrm_r387.h).

## 1. Como o RM r387 é chamado

Todo `NvRm*` exportado pelo `NVDAResmanWeb` é um **wrapper fino**:

1. zera um bloco na pilha;
2. monta a struct NVOS;
3. chama um despachante interno único (`0x627a4`, chamado aqui de `rmEscape`):

```
rmEscape(int deviceInstance, void *tag, NvU32 cmd = 0x7000 | NV_ESC_*, void *params, NvU32 size)
```

- **Números de escape iguais aos da r615** (`nv_escape.h`): 0x2B ALLOC, 0x29 FREE, 0x2A CONTROL, 0x27 ALLOC_MEMORY,
  0x4E/0x4F MAP/UNMAP_MEMORY, 0x57/0x58 MAP/UNMAP_MEMORY_DMA, 0x34 DUP, 0x4A VID_HEAP…
- `tag == 0x600D600D` indica chamada do kernel. Com qualquer outro valor, o despachante chama `current_task()`
  e entra no caminho de usuário.
- Os acessos a memória de usuário passam por `IOMemoryDescriptor::withAddressRange(...task)` + `readBytes`/`writeBytes`
  (helpers `0xa611` e `0xa6cf`). O RM não chama `copyin`/`copyout` direto.

Com o wrapper, a assinatura de cada função e o tamanho exato da struct saem direto. 33 símbolos `NvRm*`
são exportados; o GeForceWeb importa 21 deles (inventário igual ao da fase 2).

## 2. Diferenças r387 × r615 (o que a ponte precisa traduzir)

| Escape | r387 | r615 | Situação |
|---|---|---|---|
| FREE (NVOS00) | 0x10 | 0x10 | igual |
| CONTROL (NVOS54) | 0x20 | 0x20 | igual |
| ALLOC_MEMORY (NVOS02) | 0x30 | 0x30 | igual |
| MAP_MEMORY (NVOS33) | 0x30 | 0x30 | igual |
| UNMAP_MEMORY (NVOS34) | 0x20 | 0x20 | igual |
| DUP_OBJECT (NVOS55) | 0x1C | 0x1C | igual |
| **ALLOC (NVOS21)** | 0x20 | 0x20 | **layout difere:** r387 sem `paramsSize`, `status@0x18` (r615: `paramsSize@0x18`, `status@0x1C`) |
| **MAP_MEMORY_DMA (NVOS46)** | 0x38 | 0x40 | **difere:** `dmaOffset@0x28`, `status@0x30` (r615: 0x30/0x38) |
| **UNMAP_MEMORY_DMA (NVOS47)** | 0x28 | 0x30 | **difere** |
| **VID_HEAP_CONTROL (NVOS32)** | 0xB0 | 0xB8 | **difere** na união `data` (pendente) |

Consequências:

- **ALLOC sem `paramsSize`.** A r387 descobre o tamanho dos parâmetros pela classe. A ponte precisa de uma tabela
  classe → `sizeof(params r387)` para validar e copiar com segurança. É a parte mais trabalhosa e a que mais exige cuidado.
- **Params por classe:** as structs de alocação mudaram muito entre r387 e r615. A mais crítica é a do canal:
  na r615 o `NV_CHANNEL_ALLOC_PARAMS` tem 376 B, com campos acrescentados para GSP e confidential compute.
  O GeForceWeb monta uma struct bem mais curta (pendente: layout exato).
- **Controles:** o NVK do NullMoth usa vários que provavelmente **não existem** na r387. Ver seção 5.

## 3. Achado principal: o RM já tem um caminho de user space (`NVDAUser`)

O `NVDAResmanWeb` registra um user client próprio:

- `NVDA::newUserClient` com tipo `0x80000000`/`0x80000001` cria um `NVDAUser`;
- `NVDAUser::externalMethod` tem 5 seletores: **1 = `rm_api`**, 3 = `ndrv_api`, 4 = `free_os_event` (0 e 2 vazios);
- `rm_api` lê o struct de entrada como `{u32 cmd; u32 size; params[size]}` e chama
  `rmEscape(dev, this, cmd, params, size)`, ou seja, o **mesmo despachante**, no caminho de usuário.
  A saída é a cópia do buffer depois do escape.
- **Acesso:** a checagem é kauth no escopo `com.nvidia.NVDA`, cujo callback só libera **uid 0**.
  O boot-arg `nvrm` com o bit 0x8 desliga essa checagem.

**O que isso muda:** a ponte pode virar uma **biblioteca em user space**, uma troca do `nvrm_xnu.c` do NullMoth
que traduz as structs r615 → r387 e fala com o `NVDAUser`. Com isso não precisa de kext próprio, AuxKC nem reboot.
O teste de vida da fase 3b (root → device → subdevice → CONTROL → canal) vira uma ferramenta de linha de comando rodada com `sudo`.

**Não confirmado:** se os ponteiros embutidos (`pAllocParms`, `params` do CONTROL) são lidos na task do processo
chamador. Os helpers usam um global de task que o despachante ajusta, mas só a análise estática não fecha isso.
O primeiro teste como root resolve.

Limitações desse caminho:

- **Root obrigatório** (ou `nvrm=0x8`, que libera o RM para qualquer usuário: aceitável numa máquina pessoal, mas
  é uma superfície de ataque). Para o Metal em apps comuns, vai ser preciso o boot-arg ou um intermediário.
- O `rm_api` da NVIDIA **não valida** `size` contra o tamanho do buffer. Nossa biblioteca tem que garantir isso,
  ou um erro nosso corrompe memória do kernel.
- **mmap:** o NVK espera `MAP_MEMORY` + `mmap(fd)`. No caminho de usuário, o RM mapeia direto na task
  (`createMappingInTask`) e devolve `pLinearAddress` (inferido; confirmar no teste).

## 4. Sequência do GeForceWeb até um canal funcionando

Nos call sites (símbolos C++ preservados), com handles internos `0xbeefXXXX`:

1. `nvVirtualAddressSpace::CreateClientAndDevice`: `NvRmAllocRoot(&hClient)`
2. `AllocDevice`: `NvRmAlloc(hClient, hClient, hDevice, 0x80 NV01_DEVICE_0, &params)` e depois
   `NvRmAlloc(hClient, hDevice, hSubdevice, 0x2080 NV20_SUBDEVICE_0, &params)`
3. Espaço de endereço: `NvRmAlloc(..., 0x90F1 FERMI_VASPACE_A, &params)`. O GeForceWeb gerencia as tabelas de
   página por conta própria (`mmuWalk*`, `gmmuFmt*`, `PatchPD2Entry`), o que o NVK não faz: ele usa VA gerenciado pelo RM.
4. Memória: `NvRmAllocMemory64` com classe 0x40 (VRAM) e 0x3E (sistema); `NvRmVidHeapControl`;
   `NvRmMapMemoryDma` em `nvMemoryTask::MapMemoryDma`.
5. Notifiers: `NvRmAllocContextDma2` + `NvRmBindContextDma` (`AllocNotifiers`, `InitNotifier`)
6. Canal: `nvGpFifoChannel::Allocate` → `NvRmAlloc(hClient, hDevice, hChannel, <classe do canal>, &params)`
   (classe vem de variável; pendente confirmar 0xB06F)
7. **USERD:** `MapControlRegisters` → `NvRmMapMemory(hClient, hDevice, hChannel, 0, 0x1000, &ctrl)`.
   A área de controle vem de **mapear o próprio objeto canal**, o caminho clássico pré-Volta, sem doorbell.
   O kick fica em `nvGpFifoChannel::WriteGpPut()` (pendente: ler o corpo e confirmar a escrita em `GPPut`, USERD+0x8C).
8. Objetos de engine: `nvHAL::CreateObject` → `NvRmAlloc` sob o canal
9. Espera: `NvRmIdleChannels` (`WaitForIdle`), notifiers e semáforos (`BindSemaphores`, `ServiceSemaphoreEvent`)

## 5. O que o NVK do NullMoth precisa e a r387 provavelmente não tem

O backend `nvkmd/nvrm` do NullMoth está em `ref/nvk_new/`.

| Usa | Status na r387 | Adaptação |
|---|---|---|
| `NV_SEMAPHORE_SURFACE` + `FB_GET_SEMAPHORE_SURFACE_LAYOUT` | não existe (r5xx+) | semáforo comum em memória mapeada + `SEM_EXECUTE` do canal |
| Usermode/doorbell (`VOLTA/TURING_USERMODE_A`, `NVC361_NOTIFY_CHANNEL_PENDING`) | não existe (Volta+) | escrever `GPPut` na USERD mapeada do canal |
| `WORK_SUBMIT_TOKEN` (NVC36F) | não existe | remover |
| USERD alocada pelo cliente (`hUserdMemory`) | provavelmente não | USERD por `MapMemory` do canal |
| Controles `_V2` (`CLASSLIST_V2`, `FB_GET_INFO_V2`, `GR_GET_INFO_V2`, `GET_ID_INFO_V2`) | incerto | versões sem `_V2` se não existirem |
| Preempção (`CTXSW_PREEMPTION_BIND`, `GFXP`/`CILP`) | Pascal+ | pular (o código já tem *fallback*) |
| Classes Maxwell nas tabelas | **já estão** (`MAXWELL_B`, `MAXWELL_COMPUTE_B`) | nada |

O envio por USERD continua com o mesmo layout (`Nvb06FControl`, `GPPut@0x8C`), então a troca do doorbell é pequena.

## 6. Riscos

1. **Ponteiros embutidos no caminho de usuário:** se o RM não os lê na task do chamador, o `NVDAUser` não serve
   e voltamos ao kext ponte (plano original). Mitigação: é o primeiro teste.
2. **Params por classe sem `paramsSize`:** um tamanho errado na tabela faz o RM ler ou escrever fora do buffer. A tabela
   r387 tem que sair dos call sites do GeForceWeb, classe por classe.
3. **Coexistência com o GeForceWeb:** o RM é multi-cliente, mas um canal nosso divide a GPU com o WindowServer/GL.
   Um erro de canal (RC) pode derrubar o GL. Isso ainda não foi testado.
4. **Kernel panic:** um escape malformado no caminho de usuário roda no kernel. O risco é menor que o de um kext
   (nada persiste nem muda o boot), mas existe. `keepsyms=1 debug=0x100` ajuda.
5. **`nvrm=0x8`:** libera o RM para qualquer processo. Só se for necessário, e com o seu OK.

## 7. Pendências

- Layout r387 dos params de: canal (0xB06F), VASPACE (0x90F1), DEVICE (0x80), SUBDEVICE (0x2080), ContextDma2, IdleChannels.
- Corpo de `WriteGpPut` e de `InitializeChannelId`.
- União `data` do NVOS32 r387.
- Quais controles 0x0000/0x0080/0x2080 existem na r387 (tabela de controles do RM).

## 7b. Resultado da fase 3b (2026-11-06): `nmrm_probe` no hackintosh, como root

| Etapa | Resultado |
|---|---|
| `IOServiceOpen(NVDA, 0x80000000)` | `0x0`: `NVDAUser` aberto (6 serviços `NVDA`, usado o primeiro) |
| 1. FREE com cliente inexistente | `kr 0`, status intacto: rejeitado em `CliGetClientUnderLock` antes do RM (o despachante só loga, não grava status) |
| 2. ALLOC do cliente raiz | `status 0`, `hClient 0xc1d00100`: **o RM escreveu na memória do processo** |
| 3. CONTROL `0x201` GPU_GET_ATTACHED_IDS | `status 0`, `gpuIds = { 0x100 }` |
| FREE do cliente | `status 0` |
| 4. `GPU_GET_ID_INFO` (0x202, `szName` NULL) | `status 0`, `deviceInstance 0`, `flags 0x21` |
| 4. ALLOC `NV01_DEVICE_0` (params 0x38, flags 0) e `NV20_SUBDEVICE_0` (`subDeviceId 0`) | `status 0` nos dois |
| 4. `GPU_GET_NAME_STRING` (0x20800110, 0x84) | `"GeForce GTX 960"` |
| 4/5. `GPU_GET_CLASSLIST` (0x800201): contagem, depois lista por ponteiro aninhado | 56 classes, sem escrita além do buffer |

Classes relevantes (etapa 5):

| Classe | Disponível |
|---|---|
| `MAXWELL_CHANNEL_GPFIFO_A 0xB06F` | **sim** (único tipo de canal GPFIFO; `0xA06F` não está) |
| `KEPLER_CHANNEL_GROUP_A 0xA06C` (TSG), `FERMI_CONTEXT_SHARE_A 0x9067` | sim |
| `MAXWELL_B 0xB197`, `MAXWELL_COMPUTE_B 0xB1C0`, `FERMI_TWOD_A 0x902D`, `KEPLER_INLINE_TO_MEMORY_B 0xA140` | sim |
| `MAXWELL_DMA_COPY_A 0xB0B5` | sim, **3 vezes** (3 copy engines) |
| `FERMI_VASPACE_A 0x90F1`, `NV50_MEMORY_VIRTUAL 0x50A0` | sim (VA gerenciado pelo RM disponível) |
| `VOLTA_CHANNEL_GPFIFO_A`, `VOLTA_USERMODE_A`, `NV_SEMAPHORE_SURFACE` | **não** (confirma: sem doorbell, submissão pela USERD) |

Lista completa: `0004 0002 0003 003d 003e 003f 0040 0076 007d 0005 90e8 9074 5080 907f 00f1 844c 902d 50a0 9096
90e1 90e6 90e7 85b6 9067 90f1 a080 a081 a082 0060 503b 0075 a06c a140 b0b5 b0b5 b0b5 d0b7 b6b0 917a 917b 917e 9570
927c 957d 90ec a0e1 a0e0 90e6 b06f 0073 9571 9072 9068 95a1 b197 b1c0`.

O NVK do NullMoth escolhe as classes pela lista (`nvkmd_nvrm_pdev_find_supported_class`), e todas as Maxwell que
ele procura estão presentes.

**Conclusão:** o caminho de usuário do `NVDAUser` funciona e trata os dois tipos de ponteiro embutido
(`pAllocParms` do ALLOC e `params` do CONTROL) na task do chamador. Nenhum panic.
**A ponte pode ser uma biblioteca em user space**; o kext `NMResmanBridge` fica como plano B.
Ressalva: exige root (ou `nvrm=0x8`).

## 7c. Preparação do canal (análise estática, parcial)

- **`FERMI_VASPACE_A`**: o GeForceWeb usa `index 3` (`GPU_DEVICE`, referência ao VA do device) e zera 0x30 bytes.
  Inferido: r387 = r615 sem `pasid` (0x30). Para nós, `index 0` (`GPU_NEW`).
- **Memória de sistema** (`ALLOC_MEMORY`, classe `0x3E`): o GeForceWeb usa `flags 0x2010`
  (não contígua, PCI, write-combine) e `0x1010` (idem, cached).
- **`ALLOC_CONTEXT_DMA2` (NVOS39, 0x38)**: layout **igual à r615** [confirmado no wrapper];
  `NvRmAllocContextDma2(hParent=hClient, hObjectNew, hClass, flags, hMemory, offset, limit)`.
  O GeForceWeb usa classe `0x2` e `flags 0x20000000`.
- **`BIND_CONTEXT_DMA` (0x10)**: `{hClient, hChannel, hCtxDma, status}`, igual à r615 [confirmado].
- **Regra para o probe:** a r387 não recebe `paramsSize`, então passar sempre buffers de params zerados e maiores
  que o esperado.
- **Pendente:** layout r387 dos params do canal `0xB06F`, mapeamento na GPU (MapMemoryDma × `NV50_MEMORY_VIRTUAL`),
  USERD e `WriteGpPut`.

## 7d. Fase 3b concluída (2026-11-06): a GPU executou um pushbuffer nosso

`sudo ./nmrm_probe 10` no hackintosh: **semáforo = `0xCAFEF00D` em 1 ms**, `GPGet = GPPut = 1`, sem erro de canal.
Sequência que funcionou, toda pelo `NVDAUser` em user space, sem kext:

1. `ALLOC` cliente raiz → `NV01_DEVICE_0` (params 0x38, flags 0) → `NV20_SUBDEVICE_0`
2. Memória: `ALLOC_MEMORY` classe `0x3E` com **flags `0x2010` (write-combine)**, sob o device
3. Mapa da CPU: `MAP_MEMORY(hDevice, hMem)` → ponteiro de user space
4. VA: `ALLOC_MEMORY` classe `0x70` (`NV01_MEMORY_VIRTUAL`) com flags `0x2010` (o RM devolve base 0,
   limite `0xffffffffff`) → `ALLOC_CONTEXT_DMA2` (classe 2, flags `0x20000000`, `hMemory` = virtual,
   limite = o devolvido) → `MAP_MEMORY_DMA(hDma = ctxdma, flags 0)`: o **RM escolhe** o endereço (`0x100000000`)
5. Error notifier: `ALLOC_CONTEXT_DMA2` sobre uma página de sistema
6. Canal: `ALLOC 0xB06F` sob o device, params `{hObjectError, hObjectBuffer = ctxdma VA, gpFifoOffset, entries, flags 0}`
   (prefixo igual à r615; buffer de 0x100 bytes zerado) → `BIND_CONTEXT_DMA(canal, ctxdma VA)`
7. USERD: `MAP_MEMORY(hDevice, hChannel, 0, 0x200)`
8. Objeto `MAXWELL_COMPUTE_B` no canal (params NULL)
9. **`NVA06F_CTRL_CMD_GPFIFO_SCHEDULE` (0xa06f0103) com struct de 1 byte (`bEnable = 1`)**: sem ele o host não executa o canal
10. Pushbuffer `SEMAPHOREA..D` (host, subcanal 0, release 4 bytes), entrada GPFIFO, `sfence`, `GPPut` em **USERD+0x8C**

Lições:
- **Memória cached (`0x1010`) + mapa de GPU sem snoop não funciona:** a GPU lê a RAM por fora do cache da CPU e vê o
  GPFIFO zerado (`GPGet` anda, nada executa). Usar WC, ou mapear com `CACHE_SNOOP`.
- A receita do GeForceWeb (virtual `ALLOC_NONE` + offset fixo) exige device com `VASPACE_SHARED_MANAGEMENT`
  (flags `0x908`); com VA gerenciado pelo RM ela dá `0x1F`.
- Ainda não testado: se o `MAXWELL_COMPUTE_B` é necessário quando se usa o schedule (etapa 10 sempre cria o objeto).

## 8. Ponto de decisão

As assinaturas estão claras: 12 `NvRm*` confirmados, e as structs dos escapes principais estão confirmadas byte a byte.
A sequência de criação do canal está identificada, faltando o layout dos params do canal.

**Recomendação para a fase 3b:** antes de qualquer kext, uma ferramenta `nmrm_probe` em user space,
rodada com `sudo`, que:

1. abre o `NVDAUser` (tipo 0x80000000);
2. faz `ALLOC` do cliente raiz;
3. faz um `CONTROL` de informação simples com ponteiro de usuário.

Se der certo, a "ponte" vira uma biblioteca de tradução r615 → r387 e o caminho do kext fica como plano B.
Isso exige `sudo` e o seu OK explícito.
