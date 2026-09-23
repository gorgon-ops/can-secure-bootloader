/**
 * @file    diag.h
 * @brief   Makes the diagnostic stack reachable on the bus.
 *
 * Joins three pieces that were until now only a library: the CAN transport
 * underneath, ISO-TP segmentation in the middle, and the UDS server on top.
 * Without this file the stack compiles and passes its tests but no tester can
 * reach it, which is the same failure as a safety layer that is never called.
 *
 * Addressing uses the two identifiers already reserved for diagnostics in the
 * message matrix: requests arrive on DIAG_REQ and responses leave on
 * DIAG_RESP. They sit in the highest-numbered range on purpose, so a
 * programming session flooding the bus still loses arbitration to every
 * control and safety message.
 *
 * This file is deliberately thin. It owns no protocol logic — that all lives in
 * the host-tested modules — and exists only to give them a transport, a clock,
 * and somewhere to put their output.
 */
#ifndef DIAG_H
#define DIAG_H

#include "uds.h"

#include <stdbool.h>
#include <stdint.h>

/** Observable state. Watch as Live Expressions in the debugger. */
typedef struct
{
    uint32_t requests;       /*!< complete UDS requests handled            */
    uint32_t responses;      /*!< responses handed to the transport        */
    uint32_t tx_failures;    /*!< transport refused to send a response     */
    uint8_t  session;        /*!< current diagnostic session               */
    uint8_t  unlocked;       /*!< 1 while programming services are allowed */
    uint8_t  download_active;/*!< 1 while a transfer is open               */
    uint32_t bytes_written;  /*!< bytes accepted into the slot so far      */
} diag_state_t;

extern volatile diag_state_t g_diag;

/**
 * @brief Bring up the diagnostic stack.
 * @param uid device identifier, bound into every security challenge.
 */
void Diag_Init(const uint8_t *uid, uint32_t now_ms);

/**
 * @brief Feed a received frame. Safe to call for any identifier.
 * @return true if the frame belonged to the diagnostic stack.
 *
 * Called from the application's receive routing rather than hooking the
 * transport directly, so there is exactly one place that decides what a given
 * identifier means.
 */
bool Diag_OnFrame(uint32_t id, const uint8_t *data, uint8_t dlc, uint32_t now_ms);

/** @brief Drive transport and session timeouts. Call every loop. */
void Diag_Task(uint32_t now_ms);

#endif /* DIAG_H */
