/* Time is always passed in rather than read from HAL_GetTick(), so the
 * application logic runs on a host against a synthetic clock. */
#ifndef APP_TIME_H
#define APP_TIME_H

#include <stdbool.h>
#include <stdint.h>

/* Unsigned subtraction stays correct across the 49.7-day tick wrap; the
 * obvious now > past does not. */
static inline uint32_t AppTime_Elapsed(uint32_t past, uint32_t now)
{
    return (uint32_t)(now - past);
}

static inline bool AppTime_Expired(uint32_t past, uint32_t now, uint32_t limit_ms)
{
    return AppTime_Elapsed(past, now) >= limit_ms;
}

#endif
