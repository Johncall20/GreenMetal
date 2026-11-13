/* Minimo do NVKMS KAPI que o NVAccel referencia (so a leitura de CRC por sysctl). No porte para o r387 nao ha KAPI:
 * o backend devolve kapi = NULL em nvDisplayPointers e esse caminho fica desligado. */
#pragma once
#include <stdint.h>
typedef uint32_t NvU32;
struct NvKmsKapiDevice;
struct NvKmsKapiCrc32 { int supported; NvU32 value; };
struct NvKmsKapiCrcs { struct NvKmsKapiCrc32 compositorCrc32, rasterGeneratorCrc32, outputCrc32; };
struct NvKmsKapiFunctionsTable {
    int (*getCRC32)(struct NvKmsKapiDevice *device, NvU32 head, struct NvKmsKapiCrcs *crcs);
};
