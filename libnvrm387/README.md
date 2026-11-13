# libnvrm387

Implementa `nvrm_xnu.h` (a interface que o backend `nvkmd/nvrm` do NVK do NullMoth usa) em cima do
RM r387 do NVIDIA Web Driver 10.3.3, pelo user client `NVDAUser` do `NVDAResmanWeb`. Não precisa de kext.
Exige root (ou o boot-arg `nvrm=0x8`).

O NVK continua falando r615 (structs de `open-gpu-kernel-modules` 615.78.08). A lib traduz para a r387.
O layout r387 está em `../nvrm_r387.h`; a receita testada na GPU, em `../docs/ANALISE_RM.md` §7d.

## Build

```
make test   # testes de tradução com transporte falso (neste Mac, sem GPU)
make lib    # build/libnvrm387.a, x86_64
make hw     # build/test_hw, x86_64: integração na GPU (sudo no hackintosh)
```

## O que é traduzido

| r615 (NVK) | r387 |
|---|---|
| `ALLOC` NVOS21/NVOS64 | NVOS21 r387 (sem `paramsSize`, `status@0x18`); params por classe |
| cliente `0x41` | ALLOC classe 0, handle devolvido pelo RM |
| `NV01_DEVICE_0` | só `deviceId`/`hClientShare`; flags e `vaMode` zerados (VA gerenciado pelo RM) |
| `FERMI_VASPACE_A` | **emulado** (registro local; a r387 usa o VA padrão do device) |
| `NV50_MEMORY_VIRTUAL` | **emulado**: faixa do alocador da lib em `[0x2_0000_0000, 0x80_0000_0000)` |
| `MAP_MEMORY_DMA` no virtual | `MAP_MEMORY_DMA` com `DMA_OFFSET_FIXED` num ctxdma global sobre um `NV01_MEMORY_VIRTUAL` (0x70) que cobre o VA |
| `NV01_MEMORY_SYSTEM` (ALLOC) | `ALLOC_MEMORY` 0x3E, write-combine `0x2010` (`NVRM387_SYSMEM_CACHED=1` usa `0x1010`) |
| `NV01_MEMORY_LOCAL_USER` | **provisório:** vira memória de sistema WC, com aviso (`NVRM387_NO_VRAM_FALLBACK=1` recusa) |
| `NV01_CONTEXT_DMA` (ALLOC) | `ALLOC_CONTEXT_DMA2`, com o cliente como pai |
| canal `0xB06F` | prefixo r615 + `hObjectBuffer` = ctxdma do VA, sem `PRIVILEGED`, `BIND_CONTEXT_DMA` automático |
| engines Maxwell | params r615 descartados, vai NULL |
| TSG, context share, eventos | `NV_ERR_NOT_SUPPORTED` (o NVK tem fallback) |
| `MAP_MEMORY` + `mmap(fd)` | o RM já mapeia na task; o ponteiro fica no fd e o `mmap` só o devolve |
| `MAP/UNMAP_MEMORY_DMA` | NVOS46/47 r615 → r387 (offsets diferentes) |
| controles | lista branca; `CLASSLIST_V2` e `GET_ID_INFO_V2` emulados; `GPFIFO_SCHEDULE` vira 1 byte; `A06F BIND` vira no-op; `WORK_SUBMIT_TOKEN` recusado; o resto, `NOT_SUPPORTED` (`NVRM387_CTRL_PASSTHROUGH=1` manda direto) |
| `CARD_INFO`, `REGISTER_FD`, OS events | emulados |

Variáveis de ambiente: `NVRM387_LOG=1` (log de cada escape), mais as listadas acima.

## Validado na GPU (2026-11-06)

`sudo ./test_hw` no hackintosh, todo pelo caminho do NVK (structs r615 → lib → r387):
`CARD_INFO` (gpu_id 0x100, PCI 10de:1401) → cliente → `GET_ID_INFO_V2` → device/subdevice → nome →
`CLASSLIST_V2` (56 classes) → `FERMI_VASPACE_A` → `NV01_MEMORY_SYSTEM` + `mmap` → `NV50_MEMORY_VIRTUAL`
→ `MAP_MEMORY_DMA` **com offset fixo em 0x2_0000_0000** → ctxdma do notifier → canal `0xB06F` →
USERD → `MAXWELL_COMPUTE_B` → `GPFIFO_SCHEDULE` → **semáforo `0xCAFEF00D` em 1 ms**, sem erro de canal.

### Controles de inicialização do NVK (validados na GPU)

| Controle (r615) | Na r387 | Resultado na GTX 960 |
|---|---|---|
| `MC_GET_ARCH_INFO` | direto | chipset `0x126` (GM206) |
| `GPU_GET_NAME_STRING` | r615 68 B → r387 0x84 | "GeForce GTX 960" |
| `GPU_GET_SHORT_NAME_STRING` | direto | "GM206-A" |
| `GR_GET_INFO_V2` (device) | `GR_GET_INFO` `0x20801201` no subdevice, **índice r387 = r615 − 3** (faixa 0x0C..0x20) | SM `0x502`, 64 warps/SM, 1 SM/TPC |
| `GR_GET_GPC_MASK` / `TPC_MASK` | não existem; derivados de cores/128, TPC/GPC e SM/TPC (só Maxwell) | 2 GPCs, 8 TPCs |
| `FB_GET_INFO_V2` | `FB_GET_INFO` `0x20801301` por ponteiro | 2048 MiB, BAR1 256 MiB |
| `TIMER_GET_TIME`, `FIFO_GET_ENGINE_CONTEXT_PROPERTIES` | direto | ok |
| `ENGINE_PARTNERLIST`, `SEMAPHORE_SURFACE_LAYOUT`, preempção, `PERF_BOOST` | não existem / recusados | o NVK tem fallback, menos o layout de semaphore surface |

O RM r387 confere o `paramsSize` de cada controle: tamanho errado volta `0x3A` sem escrever nada.

## O que o NVK ainda precisa mudar (fase 4)

- **USERD:** na r387 ela vem de `MAP_MEMORY` do próprio canal (0x200 bytes, `GPPut` em +0x8C), não de `hUserdMemory`.
- **Submissão:** escrever `GPPut` na USERD; sem doorbell (`VOLTA_USERMODE_A`) e sem work submit token.
- **Sincronização:** sem `NV_SEMAPHORE_SURFACE`; usar semáforo comum (release do host) e polling, já que eventos não funcionam ainda.
- **Agendamento:** chamar `GPFIFO_SCHEDULE` depois de criar o canal e o objeto de engine (o NVK já chama).

## Pendente

- Alocação de VRAM de verdade (`VidHeapControl` / NVOS32 r387).
- Controles que o NVK usa na inicialização (`MC_GET_ARCH_INFO`, `GR_GET_INFO_V2`, `FB_GET_INFO_V2`, máscaras GPC/TPC, `TIMER_GET_TIME`…): conferir cada um na r387 antes de liberar.
- Eventos (`NV01_EVENT_OS_EVENT`) para não depender de polling.
- PTE kind (block linear/compressão) na tradução do `MAP_MEMORY_DMA`.

## Atualização (2026-11-06, 14:41): pipeline gráfico completo

Regressão `run_maxwell_regression.sh`: `test_hw`, device, compute, triângulo LINEAR e **triângulo OPTIMAL** passam.

- **NVOS32 `ALLOC_SIZE` na r387:** dois campos a mais logo depois de `flags`, então **`attr@0x40` e `format` (page kind) `@0x44`**,
  e o resto 8 bytes depois da r615 (`size@0x60`, `offset@0x70`). Confirmado no `AllocPageLevel`/`CreateSurface` do GeForceWeb e pelo
  `vram_probe` (o RM completa o `attr` que recebe).
- **`NV01_MEMORY_LOCAL_USER` = VRAM física** por NVOS32 sem `VIRTUAL`: `attr` do NVK com `LOCATION_VIDMEM` e coerência WC,
  `MEMORY_HANDLE_PROVIDED`. O mapa na CPU vem pela BAR1 (`MAP_MEMORY`). `NVRM387_VRAM_SYSMEM=1` volta ao fallback antigo.
- **Virtual com kind** (`format 0xFE` + `BLOCK_LINEAR`) por NVOS32 `VIRTUAL`, com mapa direto, `PAGE_KIND_VIRTUAL` e VA fixo.
- **VA emulado alinhado em 128 KiB** (página grande do Maxwell); desalinhado, o mapa de VRAM com página grande dá `NO_MEMORY`.
