#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>

#define MM_BACKEND_TEST_TYPE 0x4D58424Bu   /* 'MXBK' */

void mmBackendInit(void);
extern int gMxFlip, gMxFlipIso, gMxFlipExc, gMxFlipCode, gMxFlipStorage, gMxFlipCoreStorage, gMxFlipCount, gMxFlipStuck, gMxVramMappedKB, gMxVramRefused, gMxVramUnmappedKB, gMxVramTempMaps;   /* debug.mxflip* (MaxwellBackend.cpp) */
void mmBackendShutdown(void);
IOReturn mmBackendPlatformFunction(const OSSymbol *fn, void *p1, void *p2);
IOUserClient *mmBackendNewTestClient(IOService *provider, task_t task, void *securityID, UInt32 type);
