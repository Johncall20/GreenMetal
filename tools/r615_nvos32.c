#include <stdio.h>
#include <stddef.h>
#include "nvtypes.h"
#include "nvos.h"
#define O(f) printf("  %-28s @0x%03zx (%zu)\n", #f, offsetof(NVOS32_PARAMETERS, data.AllocSize.f), sizeof(((NVOS32_PARAMETERS*)0)->data.AllocSize.f))
int main(void){
 printf("NVOS32 %zu, data @0x%zx, AllocSize %zu\n", sizeof(NVOS32_PARAMETERS), offsetof(NVOS32_PARAMETERS,data), sizeof(((NVOS32_PARAMETERS*)0)->data.AllocSize));
 printf("  function @0x%zx hVASpace @0x%zx status @0x%zx total @0x%zx free @0x%zx\n", offsetof(NVOS32_PARAMETERS,function), offsetof(NVOS32_PARAMETERS,hVASpace), offsetof(NVOS32_PARAMETERS,status), offsetof(NVOS32_PARAMETERS,total), offsetof(NVOS32_PARAMETERS,free));
 O(owner);O(hMemory);O(type);O(flags);O(attr);O(format);O(comprCovg);O(zcullCovg);O(partitionStride);O(width);O(height);O(size);O(alignment);O(offset);O(limit);O(address);O(rangeBegin);O(rangeEnd);O(attr2);O(ctagOffset);O(numaNode);
 printf("FUNCTION_ALLOC_SIZE=%d FREE=%d VIRTUAL=0x%x FIXED=0x%x\n", NVOS32_FUNCTION_ALLOC_SIZE, NVOS32_FUNCTION_FREE, NVOS32_ALLOC_FLAGS_VIRTUAL, NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE);
 return 0;}
