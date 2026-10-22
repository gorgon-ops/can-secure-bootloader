#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/* LSI is only specified as 17..47 kHz, so the timeout is a range. The minimum
 * is the one that matters: no legitimate loop may take longer than that. */
#define WATCHDOG_TIMEOUT_NOMINAL_MS 250U

#define WATCHDOG_TIMEOUT_MIN_MS 170U

void Watchdog_Init(void);
void Watchdog_Refresh(void);
bool Watchdog_ResetWasWatchdog(void);
uint32_t Watchdog_PreviousOverdueMask(void);
void Watchdog_RecordOverdue(uint32_t mask);

#endif
