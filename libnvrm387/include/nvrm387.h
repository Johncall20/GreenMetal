/*
 * nvrm387.h - extras da libnvrm387 (alem de nvrm_xnu.h): troca de transporte para testes
 * e consulta de estado.
 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Transporte: envia um escape r387 (esc = numero NV_ESC_*, sem o 0x7000) com a struct r387
 * em 'params' (entrada e saida). Retorna 0 se o escape chegou ao RM (o resultado do RM fica
 * no campo status da struct), ou um errno. */
typedef int (*nvrm387_transport_fn)(uint32_t esc, void *params, uint32_t size);

/* Troca o transporte (testes). NULL volta ao NVDAUser. */
void nvrm387_set_transport(nvrm387_transport_fn fn);

/* Zera toda a tabela interna (testes). Nao fala com o RM. */
void nvrm387_reset_state(void);

/* Numero de objetos que a lib esta acompanhando (testes/diagnostico). */
int nvrm387_object_count(void);

/* Faixa do alocador de VA emulado (NV50_MEMORY_VIRTUAL). */
#define NVRM387_VA_DEFAULT_BASE  0x0000000200000000ull
#define NVRM387_VA_DEFAULT_END   0x0000008000000000ull

#ifdef __cplusplus
}
#endif
