/* Kept in a .noinit section the startup code does not zero, so it survives the
 * reset. A magic word separates a real record from cold-boot garbage. */
#ifndef FAULT_RECORD_H
#define FAULT_RECORD_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    FAULT_KIND_NONE = 0,
    FAULT_KIND_ERROR_HANDLER,
    FAULT_KIND_WATCHDOG
} fault_kind_t;

typedef struct
{
    fault_kind_t kind;
    uint32_t     overdue_mask;  /* watchdog: which tasks were overdue */
    uint32_t     fault_pc;      /* error handler: return address of the caller */
    uint32_t     consecutive;
} fault_summary_t;

/* Call first thing in main(), before HAL_Init() - anything after it may
 * already call Error_Handler(). */
void FaultRecord_Init(void);

const fault_summary_t *FaultRecord_Previous(void);
void FaultRecord_NoteWatchdog(uint32_t overdue_mask);
uint32_t FaultRecord_NoteError(uint32_t fault_pc);

/* Without this the count would accumulate across unrelated faults and
 * eventually park the board for no reason. */
void FaultRecord_NoteHealthy(void);

#endif
