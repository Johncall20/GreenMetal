/*
 * nvrm_r387.h - API C classica do RM exportada por NVDAResmanWeb (Web Driver 10.3.3,
 * pacote 387.10.10.10.40.140), deduzida por analise estatica (fase 3a).
 *
 * Legenda:
 *   [confirmado] lido no wrapper exportado do NVDAResmanWeb (offsets/tamanho exatos)
 *   [inferido]   deduzido por analogia com a r615 ou pelo uso no GeForceWeb; confirmar
 *   [pendente]   ainda nao analisado
 *
 * Fonte dos offsets: cada NvRm* exportado zera um bloco na pilha, preenche a struct NVOS e
 * chama o despachante interno rmEscape(dev, tag, 0x7000|ESC, &params, size). O "size"
 * passado e o tamanho exato da struct na r387.
 *
 * Plataforma: x86_64 SysV, LP64. NvP64 = ponteiro de 64 bits alinhado em 8.
 * Comparacao com a r615 (open-gpu-kernel-modules 615.78.08): tools/r615_layout.txt
 */
#ifndef NVRM_R387_H
#define NVRM_R387_H

#include <stdint.h>
#include <stddef.h>

/* Tipos basicos: so quando os headers da r615 (nvtypes.h) nao estao presentes.
 * Campos de ponteiro das structs r387 sao NvU64 (a r615 define NvP64 como void*). */
#ifndef NVTYPES_INCLUDED
typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef uint32_t NvHandle;
typedef uint32_t NvV32;
typedef uint32_t NV_STATUS;
typedef uint64_t NvP64;
#endif

/* ------------------------------------------------------------------------------------------
 * Escapes [confirmado]: mesmos numeros da r615 (nv_escape.h). No despachante o cmd e 0x7000|ESC.
 * Tamanho = sizeof da struct na r387, lido do wrapper.
 * ---------------------------------------------------------------------------------------- */
#define NV387_ESC_BASE                    0x7000u
#define NV387_ESC_RM_ALLOC_MEMORY         0x27  /* 0x30  NVOS02  (r615: 0x30, igual)        */
#define NV387_ESC_RM_ALLOC_OBJECT         0x28  /* 0x14  NVOS05                             */
#define NV387_ESC_RM_FREE                 0x29  /* 0x10  NVOS00  (r615: 0x10, igual)        */
#define NV387_ESC_RM_CONTROL              0x2A  /* 0x20  NVOS54  (r615: 0x20, igual)        */
#define NV387_ESC_RM_ALLOC                0x2B  /* 0x20  NVOS21  (r615: 0x20, LAYOUT DIFERE)*/
#define NV387_ESC_RM_DIRECT_METHOD_CALL   0x2C  /* 0x18                                     */
#define NV387_ESC_RM_CONFIG_VERSION       0x31  /* 0x10                                     */
#define NV387_ESC_RM_CONFIG_GET           0x32  /* 0x14                                     */
#define NV387_ESC_RM_CONFIG_SET           0x33  /* 0x18                                     */
#define NV387_ESC_RM_DUP_OBJECT           0x34  /* 0x1C  NVOS55  (r615: 0x1C, igual)        */
#define NV387_ESC_RM_CONFIG_GET_EX        0x37  /* 0x20                                     */
#define NV387_ESC_RM_CONFIG_SET_EX        0x38  /* 0x20                                     */
#define NV387_ESC_RM_I2C_ACCESS           0x39  /* 0x20                                     */
#define NV387_ESC_RM_POWER_MANAGEMENT     0x3A  /* 0x20                                     */
#define NV387_ESC_RM_INTERRUPT            0x3B  /* 0x0C                                     */
#define NV387_ESC_RM_OS_CONFIG_GET        0x3C  /* 0x14                                     */
#define NV387_ESC_RM_OS_CONFIG_SET        0x3D  /* 0x18                                     */
#define NV387_ESC_RM_OS_CONFIG_GET_EX     0x3E  /* 0x20                                     */
#define NV387_ESC_RM_OS_CONFIG_SET_EX     0x3F  /* 0x20                                     */
#define NV387_ESC_RM_IDLE_CHANNELS        0x41  /* 0x38                                     */
#define NV387_ESC_RM_IDLE_DEVICE          0x42  /* 0x0C                                     */
#define NV387_ESC_RM_VID_HEAP_CONTROL     0x4A  /* 0xB0  NVOS32  (r615: 0xB8, DIFERE)       */
#define NV387_ESC_RM_UPDATE_CONTEXT_DMA   0x4B  /* 0x24                                     */
#define NV387_ESC_RM_MAP_MEMORY           0x4E  /* 0x30  NVOS33  (r615: 0x30, igual)        */
#define NV387_ESC_RM_UNMAP_MEMORY         0x4F  /* 0x20  NVOS34  (r615: 0x20, igual)        */
#define NV387_ESC_RM_CONFIG               0x50  /* 0x10                                     */
#define NV387_ESC_RM_ALLOC_CONTEXT_DMA2   0x54  /* 0x38                                     */
#define NV387_ESC_RM_ADD_VBLANK_CALLBACK  0x56  /* 0x38                                     */
#define NV387_ESC_RM_MAP_MEMORY_DMA       0x57  /* 0x38  NVOS46  (r615: 0x40, DIFERE)       */
#define NV387_ESC_RM_UNMAP_MEMORY_DMA     0x58  /* 0x28  NVOS47  (r615: 0x30, DIFERE)       */
#define NV387_ESC_RM_BIND_CONTEXT_DMA     0x59  /* 0x10                                     */

/* Tag que o despachante usa para chamada vinda do kernel [confirmado].
 * Qualquer outro valor => caminho de usuario (current_task()). */
#define NV387_KERNEL_CLIENT_TAG           0x600D600Du

/* ------------------------------------------------------------------------------------------
 * Structs NVOS na r387
 * ---------------------------------------------------------------------------------------- */

/* NV_ESC_RM_ALLOC [confirmado]. DIFERE da r615: nao tem paramsSize; status em 0x18.
 * r615: ... pAllocParms@0x10, paramsSize@0x18, status@0x1C.
 * Consequencia: a r387 descobre o tamanho dos params pela classe. A ponte precisa de
 * uma tabela classe -> sizeof(params r387) para validar/copiar. */
typedef struct {
    NvHandle hRoot;          /* 0x00 */
    NvHandle hObjectParent;  /* 0x04 */
    NvHandle hObjectNew;     /* 0x08 */
    NvV32    hClass;         /* 0x0C */
    NvU64    pAllocParms;    /* 0x10 */
    NvV32    status;         /* 0x18 */
    NvU32    _pad;           /* 0x1C */
} NVOS21_PARAMETERS_R387;

/* NV_ESC_RM_FREE [confirmado], igual a r615. */
typedef struct {
    NvHandle hRoot;          /* 0x00 */
    NvHandle hObjectParent;  /* 0x04 */
    NvHandle hObjectOld;     /* 0x08 */
    NvV32    status;         /* 0x0C */
} NVOS00_PARAMETERS_R387;

/* NV_ESC_RM_CONTROL [confirmado], igual a r615 (flags zerado pelo wrapper). */
typedef struct {
    NvHandle hClient;        /* 0x00 */
    NvHandle hObject;        /* 0x04 */
    NvV32    cmd;            /* 0x08 */
    NvU32    flags;          /* 0x0C */
    NvU64    params;         /* 0x10 */
    NvU32    paramsSize;     /* 0x18 */
    NvV32    status;         /* 0x1C */
} NVOS54_PARAMETERS_R387;

/* NV_ESC_RM_ALLOC_MEMORY [confirmado], igual a r615. pMemory e limit sao entrada e saida. */
typedef struct {
    NvHandle hRoot;          /* 0x00 */
    NvHandle hObjectParent;  /* 0x04 */
    NvHandle hObjectNew;     /* 0x08 */
    NvV32    hClass;         /* 0x0C */
    NvV32    flags;          /* 0x10 */
    NvU32    _pad;           /* 0x14 */
    NvU64    pMemory;        /* 0x18 */
    NvU64    limit;          /* 0x20 */
    NvV32    status;         /* 0x28 */
    NvU32    _pad2;          /* 0x2C */
} NVOS02_PARAMETERS_R387;

/* NV_ESC_RM_MAP_MEMORY [confirmado], igual a r615. pLinearAddress e saida. */
typedef struct {
    NvHandle hClient;        /* 0x00 */
    NvHandle hDevice;        /* 0x04 */
    NvHandle hMemory;        /* 0x08 */
    NvU32    _pad;           /* 0x0C */
    NvU64    offset;         /* 0x10 */
    NvU64    length;         /* 0x18 */
    NvU64    pLinearAddress; /* 0x20 */
    NvV32    status;         /* 0x28 */
    NvV32    flags;          /* 0x2C */
} NVOS33_PARAMETERS_R387;

/* NV_ESC_RM_UNMAP_MEMORY [confirmado], igual a r615. */
typedef struct {
    NvHandle hClient;        /* 0x00 */
    NvHandle hDevice;        /* 0x04 */
    NvHandle hMemory;        /* 0x08 */
    NvU32    _pad;           /* 0x0C */
    NvU64    pLinearAddress; /* 0x10 */
    NvV32    status;         /* 0x18 */
    NvV32    flags;          /* 0x1C */
} NVOS34_PARAMETERS_R387;

/* NV_ESC_RM_MAP_MEMORY_DMA [confirmado]. DIFERE da r615 (0x40, dmaOffset@0x30, status@0x38).
 * dmaOffset e entrada (se flags pedir offset fixo) e saida. */
typedef struct {
    NvHandle hClient;        /* 0x00 */
    NvHandle hDevice;        /* 0x04 */
    NvHandle hDma;           /* 0x08  ctx DMA ou objeto de VA */
    NvHandle hMemory;        /* 0x0C */
    NvU64    offset;         /* 0x10 */
    NvU64    length;         /* 0x18 */
    NvV32    flags;          /* 0x20 */
    NvU32    _pad;           /* 0x24  [inferido] na r615 aqui ha campos novos (flags2/kind) */
    NvU64    dmaOffset;      /* 0x28 */
    NvV32    status;         /* 0x30 */
    NvU32    _pad2;          /* 0x34 */
} NVOS46_PARAMETERS_R387;

/* NV_ESC_RM_UNMAP_MEMORY_DMA [confirmado]. DIFERE da r615 (0x30). */
typedef struct {
    NvHandle hClient;        /* 0x00 */
    NvHandle hDevice;        /* 0x04 */
    NvHandle hDma;           /* 0x08 */
    NvHandle hMemory;        /* 0x0C */
    NvV32    flags;          /* 0x10 */
    NvU32    _pad;           /* 0x14 */
    NvU64    dmaOffset;      /* 0x18 */
    NvV32    status;         /* 0x20 */
    NvU32    _pad2;          /* 0x24 */
} NVOS47_PARAMETERS_R387;

/* NV_ESC_RM_DUP_OBJECT [confirmado], igual a r615. */
typedef struct {
    NvHandle hClient;        /* 0x00  destino */
    NvHandle hParent;        /* 0x04 */
    NvHandle hObject;        /* 0x08 */
    NvHandle hClientSrc;     /* 0x0C */
    NvHandle hObjectSrc;     /* 0x10 */
    NvU32    flags;          /* 0x14 */
    NvV32    status;         /* 0x18 */
} NVOS55_PARAMETERS_R387;

/* NV_ESC_RM_ALLOC_CONTEXT_DMA2 [confirmado no wrapper], igual ao NVOS39 da r615.
 * O pai e o cliente; hSubDevice e selector ficam 0 no wrapper. */
typedef struct {
    NvHandle hObjectParent;  /* 0x00 */
    NvHandle hSubDevice;     /* 0x04 */
    NvHandle hObjectNew;     /* 0x08 */
    NvV32    hClass;         /* 0x0C */
    NvV32    flags;          /* 0x10 */
    NvU32    selector;       /* 0x14 */
    NvHandle hMemory;        /* 0x18 */
    NvU32    _pad;           /* 0x1C */
    NvU64    offset;         /* 0x20 */
    NvU64    limit;          /* 0x28 */
    NvV32    status;         /* 0x30 */
    NvU32    _pad2;          /* 0x34 */
} NVOS39_PARAMETERS_R387;

/* NV01_DEVICE_0 alloc params [confirmado no hackintosh: status 0 com flags 0].
 * 0x38 bytes, VA em 0x18/0x20/0x28 como na r615. O GeForceWeb grava 0x908
 * (VASPACE_SIZE|SHARED_MANAGEMENT|RESTRICT_RESERVED_VALIMITS) em 0x14, nao em 0x10:
 * a posicao exata de 'flags' na r387 esta [inferida]. Com flags 0 o VA e gerenciado pelo RM. */
typedef struct {
    NvU32 deviceId;          /* 0x00 */
    NvU32 hClientShare;      /* 0x04 */
    NvU32 hTargetClient;     /* 0x08 */
    NvU32 hTargetDevice;     /* 0x0C */
    NvU32 flags;             /* 0x10 [inferido] */
    NvU32 _r14;              /* 0x14 [o GeForceWeb grava flags aqui] */
    NvU64 vaSpaceSize;       /* 0x18 */
    NvU64 vaStartInternal;   /* 0x20 */
    NvU64 vaLimitInternal;   /* 0x28 */
    NvU32 vaMode;            /* 0x30 */
    NvU32 _pad;              /* 0x34 */
} NV0080_ALLOC_PARAMETERS_R387;

/* Params de alocacao de evento (NvRmAllocEvent monta isto e chama ALLOC) [confirmado].
 * hSrcResource fica 0 no wrapper. Layout igual ao NV0005_ALLOC_PARAMETERS da r615. */
typedef struct {
    NvHandle hParentClient;  /* 0x00 */
    NvHandle hSrcResource;   /* 0x04 */
    NvV32    hClass;         /* 0x08 */
    NvV32    notifyIndex;    /* 0x0C */
    NvU64    data;           /* 0x10 */
} NV0005_ALLOC_PARAMETERS_R387;

/* NV_ESC_RM_VID_HEAP_CONTROL, funcao ALLOC_SIZE: tamanho 0xB0.
 * A r387 tem DOIS campos a mais logo depois de flags (0x38/0x3C); por isso attr@0x40 e format@0x44
 * (na r615, 0x38/0x3C), e o resto 8 bytes depois da r615: size@0x60, offset@0x70,
 * rangeBegin@0x88. Sem MEMORY_HANDLE_PROVIDED, hMemory e gerado pelo RM e o valor devolvido
 * deve ser usado nos mapas. Com esse flag, hMemory e o handle pedido pelo chamador. */
typedef struct {
    NvHandle hRoot;                              /* 0x00 */
    NvHandle hObjectParent;                      /* 0x04 */
    NvU32    function;                           /* 0x08 */
    NvHandle hVASpace;                           /* 0x0C */
    NvU32    ivcHeapNumber;                      /* 0x10 */
    NvV32    status;                             /* 0x14 */
    NvU64    total;                              /* 0x18 */
    NvU64    free;                               /* 0x20 */
    NvU32    owner, hMemory, type, flags;        /* 0x28..0x37 */
    NvU32    _r38, _r3c;                         /* 0x38..0x3F campos a mais da r387 [inferido] */
    NvU32    attr;                               /* 0x40 [confirmado: AllocPageLevel 0x50800000, CreateSurface] */
    NvU32    format;                             /* 0x44 page kind [confirmado: CreateSurface] */
    NvU32    comprCovg, zcullCovg;               /* 0x48..0x4F [inferido: ordem da r615] */
    NvU32    partitionStride, width, height, _r5c; /* 0x50..0x5F [inferido] */
    NvU64    size, alignment, offset, limit;     /* 0x60..0x7F */
    NvU64    address, rangeBegin, rangeEnd;      /* 0x80..0x97 */
    NvU32    attr2, ctagOffset;                  /* 0x98..0x9F */
    uint8_t  _tail[0xB0 - 0xA0];
} NVOS32_ALLOC_R387;

/* Forma generica para os demais function codes, cuja uniao data ainda nao foi mapeada. */
typedef struct {
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvU32    function;
    NvHandle hVASpace;
    NvU32    ivcHeapNumber;
    NvV32    status;
    NvU64    total;
    NvU64    free;
    uint8_t  data[0xB0 - 0x28];
} NVOS32_PARAMETERS_R387;

#define NV387_NVOS32_FUNCTION_ALLOC_SIZE     2u
#define NV387_NVOS32_FUNCTION_FREE           3u
#define NV387_NVOS32_FLAGS_VIRTUAL           0x00080000u
#define NV387_NVOS32_FLAGS_MAP_NOT_REQUIRED  0x00008000u
#define NV387_NVOS32_FLAGS_IGNORE_BANK       0x00000001u
#define NV387_NVOS32_FLAGS_FIXED_ADDRESS     0x00000010u
#define NV387_NVOS32_ATTR_FORMAT_BLOCK_LINEAR 0x00020000u
#define NV387_NVOS46_FLAGS_PAGE_KIND_VIRTUAL (1u << 3)
#define NV387_NVOS46_FLAGS_DMA_OFFSET_FIXED  (1u << 15)
typedef NVOS32_ALLOC_R387 NVOS32_ALLOC_SIZE_R387;

/* ------------------------------------------------------------------------------------------
 * Funcoes exportadas por NVDAResmanWeb (uso dentro do kernel; ligar com OSBundleLibraries
 * com.nvidia.web.NVDAResmanWeb = 1.2.0). Todas retornam o campo status da struct.
 * ---------------------------------------------------------------------------------------- */

/* [confirmado] ALLOC com tudo zero e pAllocParms = phClient; o handle volta em *phClient. */
NV_STATUS NvRmAllocRoot(NvHandle *phClient);

/* [confirmado] */
NV_STATUS NvRmAlloc(NvHandle hClient, NvHandle hParent, NvHandle hObject,
                    NvU32 hClass, void *pAllocParms);

/* [confirmado] */
NV_STATUS NvRmFree(NvHandle hClient, NvHandle hParent, NvHandle hObject);

/* [confirmado] */
NV_STATUS NvRmControl(NvHandle hClient, NvHandle hObject, NvU32 cmd,
                      void *pParams, NvU32 paramsSize);

/* [confirmado] *ppAddress e *pLimit sao entrada e saida. */
NV_STATUS NvRmAllocMemory64(NvHandle hClient, NvHandle hParent, NvHandle hMemory,
                            NvU32 hClass, NvU32 flags, void **ppAddress, NvU64 *pLimit);

/* [confirmado] 7o argumento (flags) vai na pilha. */
NV_STATUS NvRmMapMemory(NvHandle hClient, NvHandle hDevice, NvHandle hMemory,
                        NvU64 offset, NvU64 length, void **ppLinearAddress, NvU32 flags);

/* [confirmado] */
NV_STATUS NvRmUnmapMemory(NvHandle hClient, NvHandle hDevice, NvHandle hMemory,
                          void *pLinearAddress, NvU32 flags);

/* [confirmado] flags e pDmaOffset vao na pilha; *pDmaOffset e entrada e saida. */
NV_STATUS NvRmMapMemoryDma(NvHandle hClient, NvHandle hDevice, NvHandle hDma,
                           NvHandle hMemory, NvU64 offset, NvU64 length,
                           NvU32 flags, NvU64 *pDmaOffset);

/* [confirmado] */
NV_STATUS NvRmUnmapMemoryDma(NvHandle hClient, NvHandle hDevice, NvHandle hDma,
                             NvHandle hMemory, NvU32 flags, NvU64 dmaOffset);

/* [confirmado] */
NV_STATUS NvRmDupObject(NvHandle hClient, NvHandle hParent, NvHandle hObject,
                        NvHandle hClientSrc, NvHandle hObjectSrc, NvU32 flags);

/* [confirmado] Monta NV0005_ALLOC_PARAMETERS_R387 e faz ALLOC de hClass sob hParent. */
NV_STATUS NvRmAllocEvent(NvHandle hClient, NvHandle hParent, NvHandle hObject,
                         NvU32 hClass, NvU32 notifyIndex, void *pData);

/* [confirmado] Copia 0xB0 bytes in/out. */
NV_STATUS NvRmVidHeapControl(NVOS32_PARAMETERS_R387 *pParams);

/* [confirmado no wrapper: rdi..r9 + pilha -> NVOS39 +0x00,+0x08,+0x0C,+0x10,+0x18,+0x20,+0x28; uso no NDRV: (hClient, 0xc1c00000, 2,
 * 0x20000000, hMem, 0, limite)] O pai do ctxdma e o cliente (como no escape NVOS39). */
NV_STATUS NvRmAllocContextDma2(NvHandle hClient, NvHandle hDma, NvU32 hClass, NvU32 flags,
                               NvHandle hMemory, NvU64 offset, NvU64 limit);
/* [confirmado no wrapper: escape 0x59, {hClient, hCanal, hCtxDma, status}] */
NV_STATUS NvRmBindContextDma(NvHandle hClient, NvHandle hChannel, NvHandle hCtxDma);
/* [pendente] */
NV_STATUS NvRmIdleChannels(/* 0x38 */);

/* ------------------------------------------------------------------------------------------
 * Caminho de usuario ja existente: user client NVDAUser do NVDAResmanWeb [confirmado]
 *
 *   IOServiceOpen(<servico NVDA>, task, 0x80000000 (ou 0x80000001), &conn)
 *   IOConnectCallStructMethod(conn, 1, buf, 8 + size, buf, &outSize)
 *
 *   buf = { NvU32 cmd (0x7000|ESC); NvU32 size; uint8_t params[size]; }
 *   A saida e a copia do buffer de entrada depois do escape.
 *
 * Acesso: NVDA::newUserClient so libera uid 0 (callback kauth do escopo "com.nvidia.NVDA"),
 * a menos que o boot-arg nvrm tenha o bit 0x8.
 * Seletores: 1 = rm_api, 3 = ndrv_api, 4 = free_os_event; 0 e 2 vazios.
 * [pendente] Se os ponteiros embutidos (pAllocParms, params de CONTROL) sao lidos na task
 * do chamador: so um teste como root responde.
 * ---------------------------------------------------------------------------------------- */
#define NV387_NVDAUSER_OPEN_TYPE          0x80000000u
#define NV387_NVDAUSER_SEL_RM_API         1u
#define NV387_NVDAUSER_SEL_NDRV_API       3u
#define NV387_NVDAUSER_SEL_FREE_OS_EVENT  4u

typedef struct {
    NvU32 cmd;               /* NV387_ESC_BASE | NV387_ESC_* */
    NvU32 size;              /* sizeof da struct NVOS r387 */
    /* uint8_t params[size]; */
} NV387_NVDAUSER_HEADER;

/* Tamanhos verificados em tempo de compilacao */
_Static_assert(sizeof(NVOS21_PARAMETERS_R387) == 0x20, "NVOS21 r387");
_Static_assert(offsetof(NVOS21_PARAMETERS_R387, status) == 0x18, "NVOS21.status");
_Static_assert(sizeof(NVOS00_PARAMETERS_R387) == 0x10, "NVOS00 r387");
_Static_assert(sizeof(NVOS54_PARAMETERS_R387) == 0x20, "NVOS54 r387");
_Static_assert(sizeof(NVOS02_PARAMETERS_R387) == 0x30, "NVOS02 r387");
_Static_assert(sizeof(NVOS33_PARAMETERS_R387) == 0x30, "NVOS33 r387");
_Static_assert(sizeof(NVOS34_PARAMETERS_R387) == 0x20, "NVOS34 r387");
_Static_assert(sizeof(NVOS46_PARAMETERS_R387) == 0x38, "NVOS46 r387");
_Static_assert(offsetof(NVOS46_PARAMETERS_R387, dmaOffset) == 0x28, "NVOS46.dmaOffset");
_Static_assert(sizeof(NVOS47_PARAMETERS_R387) == 0x28, "NVOS47 r387");
_Static_assert(sizeof(NVOS55_PARAMETERS_R387) == 0x1C, "NVOS55 r387");
_Static_assert(sizeof(NVOS39_PARAMETERS_R387) == 0x38, "NVOS39 r387");
_Static_assert(sizeof(NV0080_ALLOC_PARAMETERS_R387) == 0x38, "NV0080 r387");
_Static_assert(offsetof(NVOS39_PARAMETERS_R387, status) == 0x30, "NVOS39.status");
_Static_assert(sizeof(NV0005_ALLOC_PARAMETERS_R387) == 0x18, "NV0005 r387");
_Static_assert(sizeof(NVOS32_PARAMETERS_R387) == 0xB0, "NVOS32 r387");
_Static_assert(sizeof(NVOS32_ALLOC_SIZE_R387) == 0xB0, "NVOS32 ALLOC_SIZE r387");
_Static_assert(offsetof(NVOS32_ALLOC_R387, attr) == 0x40, "NVOS32.attr");
_Static_assert(offsetof(NVOS32_ALLOC_R387, format) == 0x44, "NVOS32.format");
_Static_assert(offsetof(NVOS32_ALLOC_SIZE_R387, size) == 0x60, "NVOS32.size");
_Static_assert(offsetof(NVOS32_ALLOC_SIZE_R387, offset) == 0x70, "NVOS32.offset");
_Static_assert(offsetof(NVOS32_ALLOC_SIZE_R387, rangeBegin) == 0x88, "NVOS32.rangeBegin");
_Static_assert(sizeof(NVOS32_ALLOC_R387) == 0xB0, "NVOS32 ALLOC_SIZE r387");
_Static_assert(offsetof(NVOS32_ALLOC_R387, size) == 0x60, "NVOS32.size");
_Static_assert(offsetof(NVOS32_ALLOC_R387, offset) == 0x70, "NVOS32.offset");
_Static_assert(offsetof(NVOS32_ALLOC_R387, rangeBegin) == 0x88, "NVOS32.rangeBegin");

/* ------------------------------------------------------------------------------------------
 * NDRV Presentment (csCode 26), subcomando 0x40d: registrar superficie no NDRV do RM.
 * Fonte: GeForceWeb nvDisplayManager::DupCurrentSurfaceInNDRV/GetCurrentSurfaceInfo e o
 * despacho FUN_002c8847/FUN_002b4aaa do NVDAResmanWeb (decompilados pelo autor, 2026-11-07).
 * O RM so le hMemory, size, kind e offset do NDRV_SURFACE [confirmado]; o resto e informativo.
 * ------------------------------------------------------------------------------------------ */
typedef struct {
    NvHandle hMemory;        /* 0x00 handle da memoria no cliente de origem (NvRmDupObject) [confirmado] */
    NvU32    _r04;
    NvU64    size;           /* 0x08 tamanho; vira o tamanho do NvRmMapMemory [confirmado] */
    NvU32    geom[4];        /* 0x10 recurso+0x19c..0x1a8 (largura/altura/pitch/formato?) [inferido] */
    NvU32    f20;            /* 0x20 recurso+0x228 [inferido] */
    NvU32    layout[6];      /* 0x24 recurso+0x1c0..0x1d4 (parametros de layout?) [inferido] */
    NvU32    _r3c[4];        /* 0x3C */
    NvU32    kind;           /* 0x4C page kind [confirmado] */
    NvU64    offset;         /* 0x50 offset na VRAM [confirmado] */
    NvU32    _r58[6];        /* 0x58..0x6F */
} NDRV_SURFACE_R387;

typedef struct {
    NvU32    cmd;            /* 0x00 0x40d */
    NvU32    _r04;
    NvU32    head;           /* 0x08 head de display; inexistente = ponteiro nulo no RM (PANIC) */
    uint8_t  doRegister;     /* 0x0C 1 = registra o descritor (GeForceWeb: 1) */
    uint8_t  doRelease;      /* 0x0D 1 = solta a superficie anterior (GeForceWeb: 0) */
    uint8_t  doApply;        /* 0x0E com applyGate: programa o head (GeForceWeb: 0) */
    uint8_t  _r0f;
    NvHandle hClientSrc;     /* 0x10 cliente RM dono de hMemory [confirmado] */
    NvU32    _r14;
    NDRV_SURFACE_R387 surf;  /* 0x18 */
} NDRV_REGISTER_SURFACE_R387;
/* Os campos 0x7C (u32, gravado no contexto ao aplicar) e 0x80 (u8, applyGate) caem em
 * surf._r58[...]: surf+0x64 e surf+0x68. O GeForceWeb deixa ambos em 0. */
#define NDRV_REG_APPLY_ARG_OFF   0x7Cu
#define NDRV_REG_APPLY_GATE_OFF  0x80u
#define NDRV_CSCODE_PRESENTMENT  26u
#define NDRV_PRESENT_REGISTER    0x40Du
#define NDRV_PRESENT_UPDATE      0x40Au

_Static_assert(sizeof(NDRV_SURFACE_R387) == 0x70, "NDRV_SURFACE r387");
_Static_assert(offsetof(NDRV_SURFACE_R387, kind) == 0x4C, "NDRV_SURFACE.kind");
_Static_assert(offsetof(NDRV_SURFACE_R387, offset) == 0x50, "NDRV_SURFACE.offset");
_Static_assert(sizeof(NDRV_REGISTER_SURFACE_R387) == 0x88, "0x40d r387");
_Static_assert(offsetof(NDRV_REGISTER_SURFACE_R387, surf) == 0x18, "0x40d.surf");

#endif /* NVRM_R387_H */
