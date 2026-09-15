/**
 * @file    fault_record.h
 * @brief   Fault information that survives a reset.
 *
 * A reset destroys the evidence of whatever caused it, which is why an
 * intermittent hang in the field is so hard to chase: the board comes back
 * looking exactly as if someone had power-cycled it.
 *
 * This keeps a small record in a `.noinit` RAM section, placed outside
 * `_sbss.._ebss` so the startup code does not zero it. RAM contents are
 * undefined after a cold boot, so a magic word separates a real record from
 * whatever happened to be in those bytes.
 *
 * FaultRecord_Init() must run before anything that could fail, which means
 * before HAL_Init() and the peripheral setup -- those call Error_Handler(),
 * and a fault recorded into an unvalidated buffer would be worse than none.
 */
#ifndef FAULT_RECORD_H
#define FAULT_RECORD_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    FAULT_KIND_NONE = 0,
    FAULT_KIND_ERROR_HANDLER, /*!< Error_Handler() was reached      */
    FAULT_KIND_WATCHDOG       /*!< a supervised task missed its deadline */
} fault_kind_t;

/** How the previous run ended. */
typedef struct
{
    fault_kind_t kind;
    uint32_t     overdue_mask;  /*!< watchdog: which tasks were overdue   */
    uint32_t     fault_pc;      /*!< error handler: who called it         */
    uint32_t     consecutive;   /*!< consecutive faults without a healthy run */
} fault_summary_t;

/**
 * @brief Validate the record, snapshot the previous run, arm for this one.
 *
 * Call as the very first thing in main().
 */
void FaultRecord_Init(void);

/** @brief How the previous run ended, or FAULT_KIND_NONE after a cold boot. */
const fault_summary_t *FaultRecord_Previous(void);

/** @brief Note that a supervised task went overdue. */
void FaultRecord_NoteWatchdog(uint32_t overdue_mask);

/**
 * @brief Note that Error_Handler() was reached.
 * @param fault_pc return address of the caller, so the record names the site
 * @return the consecutive fault count including this one
 */
uint32_t FaultRecord_NoteError(uint32_t fault_pc);

/**
 * @brief Declare the system healthy, clearing the consecutive fault count.
 *
 * Without this the count would accumulate across unrelated faults over the
 * life of the board and eventually latch it into the parked state for no good
 * reason.
 */
void FaultRecord_NoteHealthy(void);

#endif /* FAULT_RECORD_H */
