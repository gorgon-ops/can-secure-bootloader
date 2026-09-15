/**
 * @file    watchdog.h
 * @brief   Independent watchdog (IWDG) and reset-cause reporting.
 *
 * Driven at register level rather than through the HAL. `stm32f4xx_hal_iwdg.c`
 * is not vendored in this project and `HAL_IWDG_MODULE_ENABLED` is commented
 * out in the HAL config, so using it would mean adding vendor files that a
 * CubeMX regeneration would not put back unless IWDG were also enabled in the
 * .ioc. The IWDG is four registers and a key sequence; doing it directly is
 * smaller than that dependency and cannot be silently undone.
 *
 * Timing is derived from the LSI oscillator, which on the F446 is specified
 * only as **17 kHz to 47 kHz**, not the nominal 32 kHz. Every timeout here is
 * therefore a range, and the number that matters is the *shortest* one: the
 * watchdog must never expire faster than the slowest legitimate loop.
 *
 *   prescaler /32, reload 250  ->  250 ms nominal
 *                                  170 ms at the fast end of LSI
 *                                  470 ms at the slow end
 *
 * The main loop refreshes far more often than every 170 ms, so there is ample
 * margin. Do not tighten this without redoing the arithmetic against 47 kHz.
 *
 * Once started the IWDG cannot be stopped by software. It is frozen while the
 * core is halted by a debugger, so breakpoints do not trigger a reset.
 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/** Nominal timeout at the typical 32 kHz LSI. See the note above. */
#define WATCHDOG_TIMEOUT_NOMINAL_MS 250U

/** Guaranteed minimum timeout, at the fast end of the LSI tolerance. */
#define WATCHDOG_TIMEOUT_MIN_MS 170U

/**
 * @brief Capture the reset cause, then start the watchdog.
 *
 * Must run early: the RCC reset flags are read and cleared here, and anything
 * that clears them first would make a watchdog reset indistinguishable from a
 * power-on.
 */
void Watchdog_Init(void);

/** @brief Kick the watchdog. Call only when the system is known healthy. */
void Watchdog_Refresh(void);

/**
 * @brief Did the last reset come from the watchdog rather than power-on?
 *
 * Without this a watchdog reset looks exactly like a power cycle, and an
 * intermittent hang in the field is invisible.
 */
bool Watchdog_ResetWasWatchdog(void);

/**
 * @brief Which tasks were overdue when the watchdog last bit.
 * @return bitmask, or 0 if the last reset was not a watchdog reset.
 *
 * Kept in a RAM section the startup code does not zero, so it survives the
 * reset that destroys everything else. Guarded by a magic word, because that
 * RAM holds undefined values after a cold boot.
 */
uint32_t Watchdog_PreviousOverdueMask(void);

/** @brief Record which tasks are overdue, so it survives the coming reset. */
void Watchdog_RecordOverdue(uint32_t mask);

#endif /* WATCHDOG_H */
