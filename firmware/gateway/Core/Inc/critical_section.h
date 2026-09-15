/**
 * @file    critical_section.h
 * @brief   Nesting-safe critical sections.
 *
 * `__disable_irq()` followed by `__enable_irq()` is the obvious pattern and it
 * is wrong as soon as the code is called from anywhere that already had
 * interrupts masked: the unconditional enable turns them back on early and
 * silently breaks the caller's critical section.
 *
 * These save and restore PRIMASK instead, so a nested section leaves interrupts
 * exactly as it found them.
 *
 *     uint32_t s = CriticalSection_Enter();
 *     ... short, bounded work ...
 *     CriticalSection_Exit(s);
 */
#ifndef CRITICAL_SECTION_H
#define CRITICAL_SECTION_H

#include "main.h"

#include <stdint.h>

/** @brief Mask interrupts and return the previous PRIMASK. */
static inline uint32_t CriticalSection_Enter(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    return primask;
}

/** @brief Restore the interrupt state returned by CriticalSection_Enter(). */
static inline void CriticalSection_Exit(uint32_t primask)
{
    __set_PRIMASK(primask);
}

#endif /* CRITICAL_SECTION_H */
