# Estudo: o que falta para o Metal enxergar a GTX 960

Data: 2026-11-06. Base: fonte do NullMoth (`ref/nullmoth`: `kexts/NVRM/accel`, `plugin/`, `translator/`, `docs/HOW-IT-WORKS.md`),
o `GeForceWeb.kext` em `bins/` e o IORegistry ao vivo do hackintosh. Nada foi instalado nem carregado.

## Conclusão

**Provavelmente não é preciso escrever um kext acelerador.** O `NVAccel.kext` do NullMoth **não é IOGPU**: é um
`IOGraphicsAccelerator2` da **IOAcceleratorFamily2**, a mesma família do `nvAccelerator` do GeForceWeb que já está carregado
e funcionando (é ele que dá o OpenGL acelerado). O plugin Metal do NullMoth usa:

1. os **user clients padrão da família** (Apple), através da classe base `MTLIOAccelDevice` do Metal.framework
   (`-[NVMTLDevice initWithAcceleratorPort:]`);
2. o **NVK** para todo o trabalho de GPU (`/Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib`).

O que o `NVAccel` acrescenta de próprio é (a) os user clients `0x4E56xxxx`, que repassam escapes ao RM do NullMoth,
coberto no nosso caso pela **libnvrm387** + `NVDAUser`, e (b) extras opcionais (`NVAccelSurfaceWait`, compartilhamento de
IOSurface com a VRAM). O plugin funciona sem eles: assume NVIDIA quando não acha o `NVAccel` e faz cópia no lugar do
compartilhamento.

Então o caminho mínimo é: **o plugin do NullMoth + o nosso NVK, pendurados no `nvAccelerator` que já existe.**

## Evidências

| Ponto | Onde |
|---|---|
| `class NVAccel : public IOGraphicsAccelerator` (família 2, não IOGPU) | `kexts/NVRM/accel/nvrm-accel.cpp` |
| `newUserClient`: tipo `0x4E56xxxx` vai para o RM; **qualquer outro vai para `IOGraphicsAccelerator2::newUserClient`** | idem, linha ~619 |
| `@interface NVMTLDevice : MTLIOAccelDevice`, criado com `initWithAcceleratorPort:` | `plugin/NVMTLDevice.m` |
| Plugin acha o NVK em `/Library/GPUBundles/nvmtl/` (ou `NVMTL_ICD_LIB`) | `plugin/nvmtl_vk.c` |
| Sem `NVAccel`, o fornecedor cai em `0x10de` | `nvmtl_vk_vendor()` |
| O `nvAccelerator` do GeForceWeb é da família 2 e usa o `setProperties` padrão dela | `nm` do GeForceWeb |
| A personalidade do GeForceWeb tem `MetalPluginName = GeForceMTLDriverWeb`, `MetalPluginClassName = NVMTLDevice`; o WEG remove o primeiro ao vivo por causa do `ngfxgl=1` | `Info.plist` + `ioreg` |
| O plugin compila com **um** `clang -bundle` (Foundation, Metal, IOSurface, MetalFX + headers Vulkan) | `build/build_plugin.sh` |
| O tradutor AIR→SPIR-V é um crate Rust (`cargo build`, `libnvmtl_translate.dylib`) | `build/build_xlate.sh` |

## Plano em etapas (do mais barato e reversível ao mais invasivo)

1. **Compilar** o `NVMTLDriver` (bundle x86_64) e o `libnvmtl_translate.dylib` aqui no Mac. Sem tocar no hackintosh.
2. **Harness de linha de comando** (com `sudo`, por causa do RM): carregar o bundle por `dlopen`, criar o `NVMTLDevice` com
   `initWithAcceleratorPort:` sobre o serviço `nvAccelerator` e rodar um **kernel de compute em MSL** pela API Metal normal.
   Sem mudar o IORegistry, sem instalar nada no sistema. **É o teste decisivo**: mostra se os user clients de família do
   GeForceWeb (2017) servem ao `MTLIOAccelDevice` do Tahoe.
3. **Metal "de verdade" (`MTLCopyAllDevices`)**: pôr `MetalPluginName` no `nvAccelerator`.
   - primeiro tentar pelo user space (`IORegistryEntrySetCFProperty`, como root): só funciona se o `setProperties` da família
     aceitar a chave; a mudança some no reboot;
   - se não aceitar, um **kext mínimo** que só grava essa propriedade depois que o GeForceWeb sobe (dezenas de linhas, sem
     lógica de GPU). Esse sim segue as regras do CLAUDE.md: pendrive com EFI, OK explícito, AuxKC e reboot.
4. **Apps comuns, sem root**: o NVK dentro de cada app precisa do `NVDAUser`, que só aceita uid 0. Saídas: boot-arg
   `nvrm=0x8` (desliga a checagem; mudança de NVRAM, com OK) ou um daemon intermediário.
5. **WindowServer acelerado** (fase 6): fora deste plano. O `ngfxgl=1` fica, como manda o CLAUDE.md.

## Riscos

| Risco | Peso | Como aparece |
|---|---|---|
| Os user clients de família do GeForceWeb (2017) não atendem ao `MTLIOAccelDevice` do Tahoe | **alto**, é a incógnita principal | etapa 2: `super initWithAcceleratorPort:` volta nil ou um user client falha |
| O plugin supõe recursos do Turing+ no NVK (descriptor indexing, bindless, caminhos SASS/NVVM do NullMoth) | médio | `nvmtl_vk_init` recusa, ou falha em pipelines específicos |
| `nvmtl_process_is_allowed()`: o plugin tem lista de processos permitidos | baixo | ver variável de ambiente que libera |
| `setProperties` da família recusa `MetalPluginName` | médio | etapa 3 precisa do kext mínimo |
| Metal em todo processo novo assim que o `MetalPluginName` aparecer, inclusive daemons do sistema, e eles não são root | **alto** na etapa 3 | sem `nvrm=0x8` o plugin recusa o device; precisa testar que a recusa é limpa (o NullMoth tem lógica de "segurar" o nome até armar) |

## Próximo passo

Etapas 1 e 2. Se o harness rodar um kernel MSL na GTX 960, temos **Metal funcionando de ponta a ponta** (ainda por
`dlopen`, como root), e o resto vira integração com o sistema.

## Resultado da etapa 2 (2026-11-06, ~14:56): METAL FUNCIONOU no harness

`sudo ./nvmtltest` no hackintosh (pilha em `~/maxwell_metal/nvmtl`, nada instalado no sistema):

```
[metal] initWithAcceleratorPort: OK (361 ms)
[metal] MTLDevice: "GeForce GTX 960 (NVMTL over GeForce GTX 960 (NVK GM206-A))"
[metal] newLibraryWithSource (MSL): OK (857 ms)
[metal] newComputePipelineState (AIR -> SPIR-V -> NAK SM 5.2): OK (109 ms)
[metal] commit + waitUntilCompleted: status 4 (1.47 ms)
[metal] => OS 1024 VALORES CONFEREM: A GTX 960 RODOU UM KERNEL METAL
```

- **O risco principal caiu:** o `MTLIOAccelDevice` do macOS 26 aceitou o `nvAccelerator` do GeForceWeb (IOAcceleratorFamily2,
  2017). Não precisa de kext acelerador novo.
- Pilha: Metal.framework → `NVMTLDriver` (NullMoth, dev build, caminhos em `~/maxwell_metal/nvmtl`) → AIR → `libLLVM` 21 x86_64
  mínimo (`air-runtime/`) → `libnvmtl_translate` → SPIR-V → NVK (NAK SM 5.2) → libnvrm387 → `NVDAUser` → RM r387 → GPU.
- Fontes e builds: `metal/plugin-src`, `metal/translator-src`, `metal/llvm` (build x86_64 de Core/BitReader/Support),
  `metal/harness/nvmtltest.m`.

Próximo: etapa 3 (`MetalPluginName` no `nvAccelerator`, para `MTLCopyAllDevices` enxergar a GPU) e etapa 4 (sem root).

## Etapa 3+4: kext MaxwellMetal (pronto para instalar, 2026-11-06 ~15:20)

- Fonte: `kext/MaxwellMetal/MaxwellMetal.cpp` (`build.sh`). Pacote de instalação: `kext/deploy/` (copiado para `~/maxwell_metal/kext/deploy` no hackintosh).
- O que faz: (1) ouvinte kauth em `com.nvidia.NVDA` que dá ALLOW na ação 0; (2) `sysctl debug.maxwell_metal` (0/1) grava ou remove
  `MetalPluginName`/`MetalPluginClassName` no `nvAccelerator`. Boot-arg `-mmeoff` deixa o kext inerte.
- Por que o ouvinte basta: o callback do escopo da NVIDIA (`NVDAResmanWeb` +0xbc0) devolve ALLOW para uid 0 e **DEFER** para os
  demais (nunca DENY); no `kauth_authorize_action` do XNU, um ouvinte com ALLOW troca um DEFER do escopo por ALLOW. O
  `nvrm=0x8` só pula essa checagem (+0x42fb). Ainda há `osIsAdministrator` em `rmAllocInternal`/`rmControlInternal` para
  classes/controles privilegiados: se algo do NVK cair nisso sem root, aparece no teste.
- AuxKC: `kmutil create -n aux ... -p <cada kext da colecao atual> -p MaxwellMetal.kext -x -z`. Com `-r /Library/Extensions` o
  kmutil tenta incluir `NVDAResmanTeslaWeb` (não linka: `_VSLGestalt`); sem `-x` ele varre `/S/L/E` e falha em
  `AppleCentauriManager`. O ensaio sem root gerou a colecao com os 10 kexts de hoje (o nosso fica de fora sem root:wheel).
- `strnstr` é `kpi.private`; trocado por busca própria.

## RESULTADO (2026-11-06 ~16:13): Metal do sistema na GTX 960, sem root

Depois do reboot com o MaxwellMetal na AuxKC + `sudo sysctl -w debug.maxwell_metal=1`, o `mtlprobe` rodando como uid 501:

```
[mtl] MTLCopyAllDevices: 1 dispositivo(s)
[mtl]   "GeForce GTX 960 (NVMTL over GeForce GTX 960 (NVK GM206-A))" registryID 0x100000448
[mtl] MTLCreateSystemDefaultDevice: "GeForce GTX 960 (NVMTL over GeForce GTX 960 (NVK GM206-A))"
[mtl] kernel na GTX 960: status 4, d[0..3] = 1 6 11 16, errados 0
```

- O ouvinte kauth funcionou: NVDAUser aberto sem root, e nenhum `osIsAdministrator` do RM barrou o NVK.
- Aviso inofensivo do kernelmanagerd ("invalid codeful kernel extension in the restricted lookup table"): é a assinatura ad-hoc; o kext carregou.
- Shader cache do Mesa falhou porque `~/.cache` ficou com dono root (das execuções antigas com sudo).
- Apps fora de mtlprobe/nvmtltest/nvmtlrender precisam de `/Library/GPUBundles/nvmtl-allow.txt` (um nome de processo por linha,
  `-nome` nega, `*` libera todos) ou `NVMTL_ENABLE=1`.

## Render Metal (2026-11-06 ~16:30)

- `metal/render/nvmtlrender.m`: 9 degraus (clear, triângulo RGBA8/BGRA8, textura, blend, IOSurface BGRA8, MSAA 4x, depth/stencil,
  instanced). 8/9 de cara; depth/stencil falhava com `unsupported pte_kind 0xc3`.
- Causa: o switch de kinds do `nvkmd_nvrm_va.c` é da Turing. **Na r387 o RM ignora `format` e deriva o kind de `type` + `attr`**
  (`tools/zkind_probe.c`, sem root): Z16 0x01, S8Z24 0x11, Z24S8 0x46, ZF32 0x7b, ZF32_X24S8 0xc3 e S8 0x2a saem com
  `type=DEPTH` + BLOCK_LINEAR + DEPTH/Z_TYPE/ZS_PACKING (S8 = DEPTH_8, packing S8; `type=STENCIL` dá 0x1f).
- Correção: switch Maxwell em `nvkmd_nvrm_va.c` (maxwellHost) + libnvrm387 manda todo `format != 0` pelo caminho nativo.
  Resultado: **9/9**, regressão do Vulkan ok (`/tmp/regr-zkind`). Backup do NVK anterior: `libvulkan_nouveau.dylib.pre-zkind`.
- Chrome (GPU = "Google Chrome Helper (Aperitif)"): ganhava o device, mas Xid 13 no canal; a testar de novo com o depth/stencil corrigido.

## Chrome com Metal na GTX 960 (2026-11-06 ~16:55)

Três bugs até o processo de GPU do Chrome ("Google Chrome Helper (Aperitif)") ficar de pé desenhando com Metal:
1. NVK cruzado sem assinatura: Chrome recusa o dlopen ("at least ad-hoc signed"). `codesign -s -`.
2. `cp` por cima de dylib já carregado: SIGKILL "Code Signature Invalid / Invalid Page" (cache de assinatura do kernel por
   inode). Instalar sempre com temporário + codesign + `mv`.
3. MAP_MEMORY_DMA 0x51 em textura: o VA emulado da libnvrm387 voltava ao pool sem desfazer o mapa no RM (na r615 o FREE do
   virtual desfaz sozinho). Agora cada virtual emulado guarda até 4 mapas e o FREE manda UNMAP (`tests/test_xlate.c`, 83 ok).
   Junto: VRAM com página de 4 KiB por padrão (`NVRM387_VRAM_BIGPAGE=1` volta a grande).
Resultado: sem fault, sem 0x51, submits e surface-out correndo no processo de GPU; render 9/9 e regressão Vulkan ok.
O crash do processo principal do Chrome (menu de compartilhar, ShareKit/`applyIconAppearanceConfiguration`) é anterior e não
passa pela GPU.

## Apps com sandbox (2026-11-07)

- Pilha movida para `/Library/GPUBundles/{NVMTLDriver.bundle,nvmtl/}` (plugin recompilado em `metal/plugin-lib`; pacote e
  instalador em `kext/gpubundles`). App com sandbox lê dali; da home não.
- `tools/sbprobe` (app com `com.apple.security.app-sandbox`, ad-hoc): dentro da App Sandbox o NVDAUser é negado (0xe00002e2)
  e todo user client do `nvAccelerator` abre (inclusive `nvSurface`, `nvFermiGLContext`...): a regra das apps é pelo
  serviço acelerador, não pelo nome da classe.
- Kext 0.2.0: `MaxwellMetal : IOAccelerator` (IOGraphicsFamily); `IOServiceOpen(MaxwellMetal, 'MXRM')` cria
  `IOAccelMaxwellRMClient`, que pede no kernel um NVDAUser ao NVDA em nome da task do app e repassa `externalMethod`.
  libnvrm387 tenta a ponte e cai no NVDAUser direto (`NVRM387_NO_BRIDGE=1` força o direto). `-mmeoff`: nada publicado.
- Resultado: ponte abre dentro da sandbox; render 9/9 e Vulkan ok pela ponte; **Find My** (sandbox) com NVK e 140+ submits;
  Chrome com a sandbox de GPU LIGADA carrega NVK + tradutor pela ponte, sem crash.
- Find My exigiu `shaderFloat16`: MapKit faz conta em half. O NAK já alarga ALU de 16 bits para 32 abaixo do SM70, então
  `nvk_physical_device.c` passou a anunciar `shaderFloat16` a partir do SM50.
