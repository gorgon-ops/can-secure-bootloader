#include "watchdog.h"
#include "fault_record.h"
#include "main.h"

#define IWDG_KEY_RELOAD  0x0000AAAAU
#define IWDG_KEY_ENABLE  0x0000CCCCU
#define IWDG_KEY_UNLOCK  0x00005555U  /* allow writes to PR and RLR */

#define IWDG_PRESCALER_DIV32 3U

/* 32 kHz LSI / 32 = 1 kHz, so the reload is milliseconds at nominal LSI. */
#define IWDG_RELOAD_COUNT 250U

static bool     s_reset_was_watchdog;
static uint32_t s_previous_overdue;

void Watchdog_Init(void)
{
    /* The flags are sticky across resets: left set, every later reset would look
     * like a watchdog reset. */
    s_reset_was_watchdog = (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    /* Report the overdue mask only when the reset really was the watchdog and the
     * record agrees: either alone could be a coincidence. */
    if (s_reset_was_watchdog &&
        (FaultRecord_Previous()->kind == FAULT_KIND_WATCHDOG))
    {
        s_previous_overdue = FaultRecord_Previous()->overdue_mask;
    }
    else
    {
        s_previous_overdue = 0U;
    }

    /* So a breakpoint does not reset the board. */
    __HAL_DBGMCU_FREEZE_IWDG();

    IWDG->KR = IWDG_KEY_UNLOCK;

    /* PVU/RVU are synchronised into the much slower LSI domain. */
    while ((IWDG->SR & IWDG_SR_PVU) != 0U)
    {
    }
    IWDG->PR = IWDG_PRESCALER_DIV32;

    while ((IWDG->SR & IWDG_SR_RVU) != 0U)
    {
    }
    IWDG->RLR = IWDG_RELOAD_COUNT;

    IWDG->KR = IWDG_KEY_RELOAD;
    IWDG->KR = IWDG_KEY_ENABLE;
}

void Watchdog_Refresh(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}

bool Watchdog_ResetWasWatchdog(void)
{
    return s_reset_was_watchdog;
}

uint32_t Watchdog_PreviousOverdueMask(void)
{
    return s_previous_overdue;
}

void Watchdog_RecordOverdue(uint32_t mask)
{
    FaultRecord_NoteWatchdog(mask);
}
