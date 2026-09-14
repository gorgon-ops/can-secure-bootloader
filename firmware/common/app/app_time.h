/**
 * Wrap-safe millisecond timekeeping.
 *
 * Every module in this layer takes the current time as a parameter rather than
 * calling HAL_GetTick() itself. That is the whole reason the application logic
 * is testable on a host: a test can advance a synthetic clock instantly and
 * assert on behaviour that would take minutes to provoke on real hardware.
 */
#ifndef APP_TIME_H
#define APP_TIME_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Milliseconds elapsed from @p past to @p now.
 *
 * HAL_GetTick() is a free-running uint32 that wraps roughly every 49.7 days.
 * Unsigned subtraction gives the correct interval across that wrap, whereas the
 * obvious signed comparison (now > past) does not. Expressing it once here
 * keeps the mistake out of every call site.
 */
static inline uint32_t AppTime_Elapsed(uint32_t past, uint32_t now)
{
    return (uint32_t)(now - past);
}

/** @brief True when at least @p limit_ms has passed since @p past. */
static inline bool AppTime_Expired(uint32_t past, uint32_t now, uint32_t limit_ms)
{
    return AppTime_Elapsed(past, now) >= limit_ms;
}

#endif /* APP_TIME_H */
