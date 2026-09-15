#include "fault_record.h"

#include <string.h>

#define FAULT_RECORD_MAGIC 0x46525031UL /* "FRP1" */

typedef struct
{
    uint32_t magic;
    uint32_t kind;
    uint32_t overdue_mask;
    uint32_t fault_pc;
    uint32_t consecutive;
} fault_record_t;

/**
 * Lives in RAM the startup code does not zero, so it outlives the reset it
 * describes. See the .noinit section in STM32F446RETX_FLASH.ld.
 */
static fault_record_t s_record __attribute__((section(".noinit")));

static fault_summary_t s_previous;

void FaultRecord_Init(void)
{
    if (s_record.magic == FAULT_RECORD_MAGIC)
    {
        s_previous.kind         = (fault_kind_t)s_record.kind;
        s_previous.overdue_mask = s_record.overdue_mask;
        s_previous.fault_pc     = s_record.fault_pc;
        s_previous.consecutive  = s_record.consecutive;
    }
    else
    {
        /* Cold boot: these bytes are undefined, not a record. Reporting them
           would invent a fault that never happened. */
        memset(&s_previous, 0, sizeof(s_previous));
        s_record.consecutive = 0U;
    }

    /* Arm for this run. The consecutive count deliberately carries over: it is
       the only way to tell a one-off glitch from a fault that reproduces on
       every boot. */
    s_record.magic        = FAULT_RECORD_MAGIC;
    s_record.kind         = (uint32_t)FAULT_KIND_NONE;
    s_record.overdue_mask = 0U;
    s_record.fault_pc     = 0U;
}

const fault_summary_t *FaultRecord_Previous(void)
{
    return &s_previous;
}

void FaultRecord_NoteWatchdog(uint32_t overdue_mask)
{
    s_record.magic        = FAULT_RECORD_MAGIC;
    s_record.kind         = (uint32_t)FAULT_KIND_WATCHDOG;
    s_record.overdue_mask = overdue_mask;
}

uint32_t FaultRecord_NoteError(uint32_t fault_pc)
{
    s_record.magic    = FAULT_RECORD_MAGIC;
    s_record.kind     = (uint32_t)FAULT_KIND_ERROR_HANDLER;
    s_record.fault_pc = fault_pc;
    s_record.consecutive++;

    return s_record.consecutive;
}

void FaultRecord_NoteHealthy(void)
{
    s_record.consecutive = 0U;
}
