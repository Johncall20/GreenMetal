# Fase 6: tirar o Non-Metal do OCLP (estudo, 2026-11-07, só leitura)

## O que o OCLP pôs no sistema (OpenCore-Legacy-Patcher.plist)
- Non-Metal IOAccelerator Common: **IOAcceleratorFamily2.kext 10.13.6** (378.26) no lugar do Tahoe; IOAccelerator.framework,
  GPUSupport, OpenCL do 10.13.6.
- Non-Metal Common: tira GeForce/AMD/Intel/IOGPUFamily/IOAcceleratorFamily2 do Tahoe; IOSurface.kext + IOSurface.framework
  10.14.6; CoreDisplay 10.14.4/10.13.6; OpenGL 10.14.3; SkyLight, QuartzCore e iconservicesagent trocados; `useMetal no`.
- Nvidia Web Drivers: os bundles GL/MTL/VA do 387.10.10.10.40.140.

## Dependências que decidem
- `GeForceWeb.kext` (nvAccelerator, onde o nosso Metal se pendura hoje) exige `com.apple.iokit.IOAcceleratorFamily2 = 2.0.0`:
  é a família do High Sierra que o OCLP pôs. Com a do Tahoe ele não deve carregar.
- `NVDAResmanWeb` (RM + framebuffer NVDA = a tela) não depende dela: sobrevive sem o Non-Metal.
- Crashes de ícone/IOSurface (Chrome no Dock, menu compartilhar, `-[_IOSurfaceDebugDescription pixelFormatString]`) batem com
  os frameworks antigos misturados ao Tahoe.

## O que "Metal 100%" exige
1. Pilha gráfica do Tahoe de volta (reverter o Non-Metal), mantendo só os kexts de RM/tela do web driver.
2. Um acelerador nosso para a IOAcceleratorFamily2 do Tahoe no lugar do GeForceWeb: porte do `NVAccel` do NullMoth
   (`kexts/NVRM/accel/nvrm-accel.cpp`, ~3000 linhas, IOGraphicsAccelerator2 + display machine legada), trocando o backend
   (as funções NVRM_GPUVA/VRAM/FLIP/DISPLAY_PTRS e nvNewAccelClient do RM deles) por chamadas ao RM r387.
3. Flip/apresentação na tela pelo framebuffer NVDA do web driver (o NullMoth usa o framebuffer próprio dele): a parte mais
   difícil e sem nada testado ainda.
4. Kext armando o MetalPluginName no boot e o WindowServer liberado no plugin.

## Riscos
- Sem Non-Metal e sem o acelerador novo funcionando, o WindowServer do Tahoe não tem Metal: tela preta / loop. `-mmeoff` não
  salva (o Non-Metal já teria saído). Volta: clone, ou snapshot selado (que tira também o web driver).
- Cópia de superfícies pela CPU (sem IOSurface na VRAM) pode deixar a composição mais lenta que a de hoje até isso ser feito.
