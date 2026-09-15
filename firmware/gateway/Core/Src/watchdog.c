#include "watchdog.h"
#include "fault_record.h"
#include "main.h"

/* IWDG key register commands. */
#define IWDG_KEY_RELOAD  0x0000AAAAU  /*!< refresh the counter            */
#define IWDG_KEY_ENABLE  0x0000CCCCU  /*!< start the watchdog             */
#define IWDG_KEY_UNLOCK  0x00005555U  /*!< allow writes to PR and RLR     */

/* Prescaler field value 3 selects a divide-by-32. */
#define IWDG_PRESCALER_DIV32 3U

/* 32 kHz LSI / 32 = 1 kHz, so the reload value is milliseconds at nominal LSI. */
#define IWDG_RELOAD_COUNT 250U

static bool     s_reset_was_watchdog;
static uint32_t s_previous_overdue;

void Watchdog_Init(void)
{
    /* Read before clearing, and clear afterwards: the flags are sticky across
       resets, so leaving them set would make every later reset look like a
       watchdog reset. */
    s_reset_was_watchdog = (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    /* The persistent record is owned by fault_record.c, which has already
       validated it in FaultRecord_Init(). Only report an overdue mask when the
       reset really was the watchdog *and* the record says so: either alone
       could be a coincidence. */
    if (s_reset_was_watchdog &&
        (FaultRecord_Previous()->kind == FAULT_KIND_WATCHDOG))
    {
        s_previous_overdue = FaultRecord_Previous()->overdue_mask;
    }
    else
    {
        s_previous_overdue = 0U;
    }

    /* Stop the counter while the core is halted by a debugger, so a breakpoint
       does not reset the board. No effect when no debugger is attached. */
    __HAL_DBGMCU_FREEZE_IWDG();

    IWDG->KR = IWDG_KEY_UNLOCK;

    /* PVU and RVU must clear before the registers accept a write; they are
       synchronised into the LSI domain, which is far slower than the core. */
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
