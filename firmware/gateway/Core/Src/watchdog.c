#include "watchdog.h"
#include "main.h"

/* IWDG key register commands. */
#define IWDG_KEY_RELOAD  0x0000AAAAU  /*!< refresh the counter            */
#define IWDG_KEY_ENABLE  0x0000CCCCU  /*!< start the watchdog             */
#define IWDG_KEY_UNLOCK  0x00005555U  /*!< allow writes to PR and RLR     */

/* Prescaler field value 3 selects a divide-by-32. */
#define IWDG_PRESCALER_DIV32 3U

/* 32 kHz LSI / 32 = 1 kHz, so the reload value is milliseconds at nominal LSI. */
#define IWDG_RELOAD_COUNT 250U

/**
 * Survives reset: placed in a section the startup code does not zero, so the
 * reason the watchdog fired outlives the reset it caused. The magic word is
 * what separates a real record from whatever this RAM held after a cold boot.
 */
#define WATCHDOG_RECORD_MAGIC 0x57445431UL /* "WDT1" */

typedef struct
{
    uint32_t magic;
    uint32_t overdue_mask;
} watchdog_record_t;

static watchdog_record_t s_record __attribute__((section(".noinit")));

static bool     s_reset_was_watchdog;
static uint32_t s_previous_overdue;

void Watchdog_Init(void)
{
    /* Read before clearing, and clear afterwards: the flags are sticky across
       resets, so leaving them set would make every later reset look like a
       watchdog reset. */
    s_reset_was_watchdog = (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    if (s_reset_was_watchdog && (s_record.magic == WATCHDOG_RECORD_MAGIC))
    {
        s_previous_overdue = s_record.overdue_mask;
    }
    else
    {
        /* Cold boot, or a reset from another cause: the RAM contents are not a
           record and must not be reported as one. */
        s_previous_overdue = 0U;
    }

    s_record.magic        = WATCHDOG_RECORD_MAGIC;
    s_record.overdue_mask = 0U;

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
    s_record.magic        = WATCHDOG_RECORD_MAGIC;
    s_record.overdue_mask = mask;
}
