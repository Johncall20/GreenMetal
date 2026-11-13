# Instalando o GreenMetal

🇺🇸 [In English](INSTALL.md)

> [!CAUTION]
> O instalador é só uma frente para scripts de laboratório escritos para uma máquina (GTX 960, macOS 26.7.2). Esta
> página descreve o que eles fazem. Todo passo com `sudo` troca parte do que o seu Mac usa para dar boot. Leia cada script
> antes de rodar. Se você não está pronto para se recuperar de uma tela preta por `ssh`, pare aqui.

## Antes de tudo

1. **Rode a checagem.** Ela só lê e não precisa de root:

   ```bash
   tools/greenmetal-check.sh
   ```

   Resolva primeiro tudo que vier marcado com `[!!]`.
2. **Tenha três caminhos de volta:** um pendrive testado com a sua EFI que funciona, um clone do volume do sistema que
   dê boot, e o Acesso Remoto (`ssh`) ligado, com um segundo computador para usar.
3. **Saiba de onde você parte.** Os scripts assumem o estado de onde o autor partiu: macOS 26 com os patches non-Metal
   do OpenCore Legacy Patcher e o NVIDIA Web Driver 387.10.10.10.40.140 instalado e dirigindo a tela (`NVDAStartupWeb`,
   `NVDAResmanWeb`, `NVDAGM100HalWeb` e `GeForceWeb` na coleção auxiliar de kexts), com `ngfxgl=1` nos boot-args.
   Chegar nesse estado não é assunto desta página.

## O caminho curto: o pacote da release

Cada [release](../../../releases) traz o `GreenMetal-<versão>.tar.gz` com os quatro kexts e a pilha Metal já
compilados, e um comando só, `greenmetal`, na frente dos scripts de instalação. Extraia num lugar que vá ficar (os
dados para a volta são guardados dentro da pasta):

```bash
tar xzf GreenMetal-0.1.0-alpha.tar.gz -C ~
```

```bash
cd ~/GreenMetal-0.1.0-alpha
```

| Passo | Comando | Root | O que faz |
|---|---|---|---|
| 1 | `./greenmetal check` | não | Olha a máquina, só leitura. |
| 2 | `./greenmetal dry-run` | não | Confere se os kexts resolvem as dependências neste sistema. |
| 3 | `sudo ./greenmetal dry-run` | sim | Também monta uma coleção de kexts de teste numa pasta temporária. Não publica nada. |
| 4 | `sudo ./greenmetal install` | sim | Etapa um: Metal para os apps. Depois reinicie e rode `kext/deploy/pos_boot.sh`. |
| 5 | `sudo ./greenmetal switch` | sim | Etapa dois. Acrescente `nvaccel=1 nvaccelarm=1` aos boot-args e reinicie. |
| 6 | `sudo ./greenmetal update` | sim | Depois do primeiro boot na etapa dois: põe o framebuffer e o kext de display. Reinicie. |
| 7 | `sudo ./greenmetal desktop on` | sim | WindowServer em Metal até o próximo boot. `desktop off` volta. |

Caminhos de volta: `sudo ./greenmetal uninstall` (etapa um), `sudo ./greenmetal revert` (etapa dois, funciona por
`ssh`). O `./greenmetal status` mostra onde a máquina está a qualquer momento.

O `greenmetal` recusa rodar numa placa que não seja a GTX 960 sem `--force`, recusa se o `ssh` estiver desligado, e
pede para digitar uma palavra antes de cada passo que muda a máquina. Ele não reimplementa os scripts descritos
abaixo: confere a máquina e roda eles. As mensagens dos scripts estão em português: `PARADA` quer dizer que parou e não
fez mais nada, `PRONTO` que terminou.

**O que foi e o que não foi testado na 0.1.0-alpha.** Na máquina de referência: `check`, `status`, `dry-run` e os
próprios binários do pacote (os kexts são idênticos, byte a byte, aos que estão rodando nela; o NVK e o tradutor do
pacote renderizaram e compilaram shaders na GPU). O `install` e o `switch` rodam os mesmos scripts que colocaram aquela
máquina de pé, mas a sequência inteira não foi repetida a partir do pacote num sistema limpo. Ninguém rodou nada disso
em outra placa.

O resto desta página descreve o mesmo procedimento à mão, a partir do código-fonte.

## O que é instalado

| Peça | Para onde vai |
|---|---|
| `MaxwellMetal.kext`, `MaxwellAccel.kext`, `MaxwellAGDC.kext`, `MaxwellFB.kext` | `/Library/Extensions`, e depois a coleção auxiliar de kexts |
| `NVMTLDriver.bundle` (o plugin Metal, com o runtime de shaders dentro) | `/Library/GPUBundles` |
| `libvulkan_nouveau.dylib` (NVK) e `libnvmtl_translate.dylib` (tradutor de shaders) | `/Library/GPUBundles/nvmtl` |
| `nvmtl-allow.txt` (quais processos ganham o dispositivo Metal) | `/Library/GPUBundles` |

A pasta `/Library/GPUBundles` é usada porque apps com sandbox têm permissão de leitura nela.

## 1. Compilar

Na máquina de build:

- **Kexts:** rode o `build.sh` em `kext/MaxwellMetal`, `accel`, `agdc` e `fb`. Cada um deixa o kext na sua pasta
  `build/`.
- **NVK:** siga o [`nvk/README.md`](../nvk/README.md).
- **Plugin Metal:** `metal/build_plugin.sh` (precisa da árvore do Mesa do passo do NVK, pelos headers do Vulkan).
- **Tradutor de shaders e o runtime dele:** são do NullMoth, sem alteração. Compile o `libnvmtl_translate.dylib` e o
  `air-runtime` (um build do LLVM) a partir do [repositório do NullMoth](https://github.com/nullmoth/nvidia-macos-driver)
  na v2.7.2, seguindo os scripts de build de lá.

Tudo precisa ser x86_64, e todo dylib e bundle precisa pelo menos de assinatura ad-hoc (`codesign -s - -f`). Processos
com sandbox recusam código sem assinatura.

## 2. Arrumar os binários

Os scripts procuram os binários ao lado deles. Deixe assim:

```
kext/deploy/MaxwellMetal.kext
kext/troca/MaxwellAccel.kext
kext/troca/MaxwellAGDC.kext
kext/troca/MaxwellFB.kext                                      (opcional: o framebuffer)
kext/gpubundles/NVMTLDriver.bundle/Contents/Info.plist
kext/gpubundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver
kext/gpubundles/NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib
kext/gpubundles/nvmtl/libvulkan_nouveau.dylib
kext/gpubundles/nvmtl/libnvmtl_translate.dylib
```

Depois copie a árvore para **`~/maxwell_metal`** na máquina de destino. Vários scripts mostram ou usam esse caminho.

## 3. Etapa um: Metal para os apps

Mantém a sua área de trabalho exatamente como está e acrescenta um dispositivo Metal que os apps conseguem usar.

```bash
sudo mkdir -p /Library/GPUBundles
```

```bash
sudo ~/maxwell_metal/kext/deploy/install_maxwellmetal.sh
```

O que o script faz:

- recusa rodar se a assinatura de kext não estiver desligada no SIP, ou se o `NVDAResmanWeb` não estiver na coleção
  atual;
- monta uma coleção auxiliar **nova** num arquivo temporário, com todos os kexts da atual mais o `MaxwellMetal`, e para
  se algum kext existente fosse ficar de fora;
- guarda duas cópias da coleção antiga ao lado da que está em uso: `.maxwell-backup` (de antes da primeira instalação)
  e `.maxwell-prev` (de antes desta rodada);
- instala a pilha de user space em `/Library/GPUBundles`;
- só então publica a coleção nova.

Instale a lista de processos (`sudo ~/maxwell_metal/kext/deploy/allowlist_install.sh`), reinicie e confira:

```bash
~/maxwell_metal/kext/deploy/pos_boot.sh
```

Ele confirma que o kext carregou, arma o plugin Metal (`sudo sysctl -w debug.maxwell_metal=1`, que vale até o próximo
boot) e roda uma sonda pequena de Metal com o seu usuário comum.

**Se a máquina não subir:** dê boot com `-mmeoff` nos boot-args (o kext carrega e não faz nada), ou rode
`sudo ~/maxwell_metal/kext/deploy/uninstall_maxwellmetal.sh` por `ssh`, ou restaure a coleção pelo Recovery como está
em `kext/deploy/RECUPERACAO.txt`.

## 4. Etapa dois: a troca

Este é o passo que pode te deixar sem imagem. Ele tira os patches non-Metal e o acelerador da NVIDIA, e sobe o sistema
com o `MaxwellAccel` no lugar. Só faça depois que a etapa um funcionar.

```bash
sudo ~/maxwell_metal/kext/troca/1_preparar.sh
```

Seguro: não muda nada que o próximo boot usa. Monta o snapshot selado original da Apple só para leitura, monta contra
ele a coleção nova (Web Driver sem o `GeForceWeb`, mais `MaxwellMetal` e `MaxwellAccel`) num arquivo à parte, e guarda
a coleção atual e os kexts da NVIDIA para a volta.

```bash
sudo ~/maxwell_metal/kext/troca/2_trocar.sh
```

Não é seguro: pede para digitar `TROCAR`, e então desliga o auto-patch do OpenCore Legacy Patcher, limpa os ajustes do
CoreDisplay, instala a lista de processos desta configuração, faz o bless do último snapshot selado (o que tira todos
os patches do sistema) e publica a coleção nova.

Antes de reiniciar, acrescente aos boot-args no `config.plist` do OpenCore:

```
nvaccel=1 nvaccelarm=1
```

O `nvaccel=1` deixa o acelerador subir. O `nvaccelarm=1` arma ele no boot. Sem o segundo, você arma à mão com
`sudo sysctl -w debug.nvaccelfb=1`.

Depois do boot, também por `ssh` se a tela estiver preta:

```bash
~/maxwell_metal/kext/troca/3_conferir.sh
```

**A volta** funciona por `ssh` com a tela preta, e reaplica os patches de sistema do patcher:

```bash
sudo ~/maxwell_metal/kext/troca/9_voltar.sh
```

## 5. Depois de cada boot: a área de trabalho em Metal

A máquina sobe com a área de trabalho composta pela CPU. Para passar o WindowServer para Metal com flip sem cópia:

```bash
sudo SURFSHARE=3 FLIP=1 PERFIL=1 ~/maxwell_metal/kext/troca/10_metal_total.sh
```

Ele reinicia o WindowServer, então você cai na tela de login. Mede por uns dois minutos e meio, volta sozinho se os
contadores do flip estiverem errados, e deixa um guarda que devolve o WindowServer ao caminho da CPU se ele morrer.

Para voltar à mão:

```bash
sudo ~/maxwell_metal/kext/troca/10_metal_total.sh volta
```

## 6. Atualizando

- **Mudou algum kext:** arrume os binários de novo e rode `sudo ~/maxwell_metal/kext/troca/atualizar_accel.sh`, depois
  reinicie. Confira a data de `/Library/KernelCollections/AuxiliaryKernelExtensions.kc` antes de reiniciar: se não
  mudou, a coleção não foi refeita.
- **Mudou só o plugin ou o NVK:** `sudo ~/maxwell_metal/kext/gpubundles/install_gpubundles.sh`. Sem reboot. Os apps
  abertos depois disso usam a pilha nova.

## O que relatar

Aconteça o que acontecer, um [relato de placa](../../../issues/new/choose) com a saída do `tools/greenmetal-check.sh`
ajuda.
