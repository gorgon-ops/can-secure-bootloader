#include "diag.h"

#include "bfmc.h"      /* DIAG_REQ / DIAG_RESP */
#include "can_if.h"
#include "flash_stm32.h"
#include "flash_writer.h"
#include "isotp.h"

#include <string.h>

volatile diag_state_t g_diag;

static isotp_t      s_tp;
static uds_server_t s_uds;
static flash_writer_t s_writer;

static can_if_port_t s_port = CAN_IF_PORT_1;

/* ------------------------------------------------------------------ */
/* Security record persistence                                         */
/* ------------------------------------------------------------------ */

/* PLACEHOLDER: the security record lives in RAM until it moves to the metadata
 * sector, so this build does NOT yet resist the power-cycle bypass even though
 * the logic above it does. */
static uds_sec_record_t s_sec_ram;
static bool             s_sec_valid;

static bool Diag_SecLoad(void *ctx, uds_sec_record_t *out)
{
    (void)ctx;
    if (!s_sec_valid)
    {
        return false;
    }
    *out = s_sec_ram;
    return true;
}

static bool Diag_SecStore(void *ctx, const uds_sec_record_t *rec)
{
    (void)ctx;
    s_sec_ram   = *rec;
    s_sec_valid = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Data identifiers                                                    */
/* ------------------------------------------------------------------ */

#define DID_ECU_SERIAL   0xF18CU
#define DID_SW_VERSION   0xF195U

static uint16_t Diag_ReadDid(void *ctx, uint16_t did, uint8_t *out, uint16_t cap)
{
    (void)ctx;

    switch (did)
    {
        case DID_ECU_SERIAL:
            if (cap < UDS_UID_LEN)
            {
                return 0U;
            }
            /* Same UID that binds a security challenge to this device. */
            memcpy(out, (const void *)0x1FFF7A10UL, UDS_UID_LEN);
            return UDS_UID_LEN;

        case DID_SW_VERSION:
            if (cap < 4U)
            {
                return 0U;
            }
            out[0] = 0x00U; out[1] = 0x01U; out[2] = 0x00U; out[3] = 0x00U;
            return 4U;

        default:
            return 0U;
    }
}

/* ------------------------------------------------------------------ */
/* Download backend                                                    */
/* ------------------------------------------------------------------ */

/* ISO-TP's limit, less the SID and block counter that share the message. */
#define DIAG_MAX_BLOCK (ISOTP_MAX_MESSAGE - 2U)

/* Only a slot's exact base address is accepted: an update replaces a whole
 * image, and an arbitrary offset would mean writing into the middle of one. */
static uint8_t Diag_DownloadBegin(void *ctx, uint32_t addr, uint32_t size,
                                  uint32_t *max_block)
{
    uint8_t slot;
    flash_write_result_t r;

    (void)ctx;

    if (addr == FLASH_SLOT_A_BASE)
    {
        slot = (uint8_t)FLASH_SLOT_A;
    }
    else if (addr == FLASH_SLOT_B_BASE)
    {
        slot = (uint8_t)FLASH_SLOT_B;
    }
    else
    {
        return UDS_NRC_REQUEST_OUT_OF_RANGE;
    }

    r = FlashWriter_Begin(&s_writer, slot, size);
    if (r != FLASH_WRITE_OK)
    {
        /* A request problem (running slot, oversized image) is reported differently
         * from an ECU problem (failed erase). */
        if ((r == FLASH_WRITE_ERR_TOO_LARGE) ||
            (r == FLASH_WRITE_ERR_ACTIVE_SLOT) ||
            (r == FLASH_WRITE_ERR_BAD_SLOT) ||
            (r == FLASH_WRITE_ERR_PROTECTED))
        {
            return UDS_NRC_REQUEST_OUT_OF_RANGE;
        }
        return UDS_NRC_PROGRAMMING_FAILURE;
    }

    *max_block = DIAG_MAX_BLOCK;
    return 0U;
}

static uint8_t Diag_DownloadTransfer(void *ctx, const uint8_t *data,
                                     uint16_t len)
{
    (void)ctx;

    if (FlashWriter_Write(&s_writer, data, len) != FLASH_WRITE_OK)
    {
        return UDS_NRC_PROGRAMMING_FAILURE;
    }
    return 0U;
}

static uint8_t Diag_DownloadFinish(void *ctx)
{
    (void)ctx;

    if (FlashWriter_Finish(&s_writer) != FLASH_WRITE_OK)
    {
        return UDS_NRC_PROGRAMMING_FAILURE;
    }

    /* Not verified here: the slot can be corrupted afterwards, so the bootloader
     * has to check at boot anyway. */
    return 0U;
}

static void Diag_DownloadAbort(void *ctx)
{
    (void)ctx;
    FlashWriter_Abort(&s_writer);
}

/* ------------------------------------------------------------------ */
/* Transport glue                                                      */
/* ------------------------------------------------------------------ */

static bool Diag_SendFrame(void *ctx, const uint8_t *data, uint8_t dlc)
{
    (void)ctx;
    return CanIf_Send(s_port, (uint32_t)BFMC_DIAG_RESP_FRAME_ID, data, dlc);
}

void Diag_Init(const uint8_t *uid, uint32_t now_ms)
{
    memset((void *)&g_diag, 0, sizeof(g_diag));

    IsoTp_Init(&s_tp, Diag_SendFrame, NULL);

    Uds_SetSecurityStore(&s_uds, Diag_SecLoad, Diag_SecStore, NULL);
    Uds_SetDidReader(&s_uds, Diag_ReadDid, NULL);

    /* No verifier is provisioned, so every unlock fails: fail closed. See
     * docs/secure-bootloader.md. */
    Uds_SetVerifier(&s_uds, NULL, NULL);

    {
        flash_ops_t    fops;
        uds_download_t dl;

        FlashStm32_GetOps(&fops);
        FlashWriter_SetOps(&s_writer, &fops);

        /* Slot A holds the running image in this build. */
        FlashWriter_Init(&s_writer, (uint8_t)FLASH_SLOT_A);
        FlashWriter_SetOps(&s_writer, &fops);

        dl.begin    = Diag_DownloadBegin;
        dl.transfer = Diag_DownloadTransfer;
        dl.finish   = Diag_DownloadFinish;
        dl.abort    = Diag_DownloadAbort;
        dl.ctx      = NULL;
        Uds_SetDownload(&s_uds, &dl);
    }

    Uds_Init(&s_uds, uid, now_ms);

    g_diag.session  = Uds_Session(&s_uds);
    g_diag.unlocked = Uds_IsUnlocked(&s_uds) ? 1U : 0U;
    g_diag.download_active = Uds_DownloadActive(&s_uds) ? 1U : 0U;
    g_diag.bytes_written   = FlashWriter_Written(&s_writer);
}

bool Diag_OnFrame(uint32_t id, const uint8_t *data, uint8_t dlc, uint32_t now_ms)
{
    if (id != (uint32_t)BFMC_DIAG_REQ_FRAME_ID)
    {
        return false;
    }

    /* In interrupt context so the transport sees every frame as it arrives;
     * bounded work. The completed request is handled in the task. */
    (void)IsoTp_OnFrame(&s_tp, data, dlc, now_ms);
    return true;
}

void Diag_Task(uint32_t now_ms)
{
    const uint8_t *req;
    uint16_t       req_len = 0U;

    IsoTp_Poll(&s_tp, now_ms);
    Uds_Poll(&s_uds, now_ms);

    req = IsoTp_Message(&s_tp, &req_len);
    if ((req != NULL) && (req_len > 0U))
    {
        /* Static: a segmented send borrows this buffer across later polls. */
        static uint8_t resp[64];
        uint16_t resp_len;

        resp_len = Uds_HandleRequest(&s_uds, req, req_len,
                                     resp, (uint16_t)sizeof(resp), now_ms);

        /* Cleared before transmitting: if sending fails, a pending message would be
         * handled again on the next pass. */
        IsoTp_Reset(&s_tp);
        g_diag.requests++;

        if (resp_len > 0U)
        {
            if (IsoTp_Send(&s_tp, resp, resp_len, now_ms))
            {
                g_diag.responses++;
            }
            else
            {
                g_diag.tx_failures++;
            }
        }
    }

    g_diag.session  = Uds_Session(&s_uds);
    g_diag.unlocked = Uds_IsUnlocked(&s_uds) ? 1U : 0U;
}
