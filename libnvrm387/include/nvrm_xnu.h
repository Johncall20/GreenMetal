/*
 * nvrm_xnu.h - interface do backend nvkmd/nvrm do NVK do NullMoth (mesma assinatura do
 * nvrm_xnu.h original), implementada por libnvrm387 sobre o RM r387 do Web Driver 10.3.3.
 *
 * As structs passadas a nvrm_xnu_ioctl sao as da r615 (nvos.h / nv-ioctl.h do
 * open-gpu-kernel-modules); a libnvrm387 traduz para a r387 e fala com o user client
 * NVDAUser do NVDAResmanWeb. Exige root (ou boot-arg nvrm=0x8).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int   nvrm_xnu_open(const char *node);
int   nvrm_xnu_ioctl(int fd, uint32_t cmd, void *data, uint32_t size);
void *nvrm_xnu_mmap(int fd, size_t length);
void *nvrm_xnu_mmap_wc(int fd, size_t length, int writeCombine);
int   nvrm_xnu_munmap(void *addr, size_t length);
int   nvrm_xnu_close(int fd);
int64_t nvrm_xnu_wait_event(int fd, uint32_t seen, uint32_t timeout_us);
#ifdef __cplusplus
}
#endif
