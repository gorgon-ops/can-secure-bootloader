#ifndef DIAG_H
#define DIAG_H

#include "uds.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t requests;
    uint32_t responses;
    uint32_t tx_failures;
    uint8_t  session;
    uint8_t  unlocked;
    uint8_t  download_active;
    uint32_t bytes_written;
} diag_state_t;

extern volatile diag_state_t g_diag;

void Diag_Init(const uint8_t *uid, uint32_t now_ms);

/* Returns true if the frame belonged to the diagnostic stack. */
bool Diag_OnFrame(uint32_t id, const uint8_t *data, uint8_t dlc, uint32_t now_ms);

void Diag_Task(uint32_t now_ms);

#endif
