#ifndef _CVM_INTRIN_H
#define _CVM_INTRIN_H

#include <stdint.h>

#if defined(__clang__) || defined(__GNUC__)
#define CVM_NORETURN __attribute__((noreturn))
#else
#define CVM_NORETURN
#endif

#ifdef __cplusplus
extern "C" {
#endif

CVM_NORETURN void cvm_halt(void);
void cvm_nop(void);
void cvm_enable_interrupts(void);
void cvm_disable_interrupts(void);
void cvm_wait(void);
void cvm_fence(void);

uint64_t cvm_core_id(void);
uint64_t cvm_thread_id(void);
uint64_t cvm_exception_cause(void);
uint64_t cvm_exception_address(void);
uint64_t cvm_exception_pc(void);
uint64_t cvm_exception_info(void);

void cvm_set_vbr(uint64_t address);
uint64_t cvm_get_vbr(void);
uint64_t cvm_get_mode(void);
void cvm_enter_user(void);

void cvm_set_ptbr(uint64_t address);
uint64_t cvm_get_ptbr(void);
void cvm_mmu_on(void);
void cvm_mmu_off(void);
uint64_t cvm_get_mmu(void);

void cvm_set_ksp(uint64_t address);
uint64_t cvm_get_ksp(void);

uint64_t cvm_cas64(volatile uint64_t *address,
                   uint64_t expected,
                   uint64_t desired);
uint64_t cvm_xchg64(volatile uint64_t *address, uint64_t value);
uint64_t cvm_atomic_add64(volatile uint64_t *address, uint64_t value);

#ifdef __cplusplus
}
#endif

#undef CVM_NORETURN

#endif
