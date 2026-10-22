/* Saves and restores PRIMASK rather than a bare __disable_irq/__enable_irq
 * pair, which would re-enable interrupts early inside a caller's own critical
 * section. */
#ifndef CRITICAL_SECTION_H
#define CRITICAL_SECTION_H

#include "main.h"

#include <stdint.h>

static inline uint32_t CriticalSection_Enter(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    return primask;
}

static inline void CriticalSection_Exit(uint32_t primask)
{
    __set_PRIMASK(primask);
}

#endif
