/*
 * binary_com.c
 *
 *  Created on: 2026
 *      Author: AI Assistant
 *
 *  Binary Communication Library (Binary Protocol v1.0)
 *  Uses the existing Fragment Protocol / XBee API stack with binary payload.
 *
 *  Endianness:
 *    - Application payload (this file): Little-Endian
 *    - Fragment Protocol headers: Big-Endian (unchanged, handled by fragment_rx/tx)
 *
 *  All multi-byte values in the binary application payload use explicit
 *  read_u16le / write_u16le helpers ??never rely on struct member ordering
 *  or pointer casting for LE conversion.
 *
 *  Safety hardening note (for future maintenance):
 *  - Treat App_* return count as untrusted input even though it is internal.
 *  - Reject count > APP_MAX_* before any array access or loop iteration.
 *  - Compute payload length in uint32_t first, verify bounds, then cast to uint16_t.
 *  - Avoid early uint16_t casts in length arithmetic to prevent wraparound bugs.
 */

#include "binary_com.h"
#include "device_hal.h"
#include <string.h>
#include <stddef.h>

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

static void HandleBinaryPacket(BinaryContext *ctx, const uint8_t *data, uint32_t len);

/* Command handlers */
static void HandlePing(BinaryContext *ctx, uint8_t src_id,
                       const uint8_t *payload, uint16_t payload_len);
static void HandleMove(BinaryContext *ctx, uint8_t src_id,
                       const uint8_t *payload, uint16_t payload_len);
static void HandleMotionCtrl(BinaryContext *ctx, uint8_t src_id,
                              const uint8_t *payload, uint16_t payload_len);
static void HandlePowerCtrl(BinaryContext *ctx, uint8_t src_id,
                            const uint8_t *payload, uint16_t payload_len);
static void HandleErrorClear(BinaryContext *ctx, uint8_t src_id,
                             const uint8_t *payload, uint16_t payload_len);
static void HandleGetMotors(BinaryContext *ctx, uint8_t src_id);
static void HandleGetMotorState(BinaryContext *ctx, uint8_t src_id);
static void HandleGetFiles(BinaryContext *ctx, uint8_t src_id);
static void HandleGetFile(BinaryContext *ctx, uint8_t src_id,
                          const uint8_t *payload, uint16_t payload_len);
static void HandleSaveFile(BinaryContext *ctx, uint8_t src_id,
                           const uint8_t *payload, uint16_t payload_len);
static void HandleVerifyFile(BinaryContext *ctx, uint8_t src_id,
                             const uint8_t *payload, uint16_t payload_len);
static void HandleSetOperateTime(BinaryContext *ctx, uint8_t src_id,
                                 const uint8_t *payload, uint16_t payload_len);
static void HandleGetOperateTime(BinaryContext *ctx, uint8_t src_id,
                                 const uint8_t *payload, uint16_t payload_len);

/* XBee / Fragment callbacks */
static void OnXBeeFrame(const XBeeFrame_t *frame, void *user_data);
static void OnXBeeError(const char *error, void *user_data);
static void OnFragRxMessage(const uint8_t *data, uint32_t len,
                             uint64_t source_addr, void *user_data);
static void OnFragRxLog(const char *message, void *user_data);
static void OnFragTxComplete(uint16_t msg_id, bool success, void *user_data);
static void OnFragTxLog(const char *message, void *user_data);

typedef union {
    AppMotorInfo motors[APP_MAX_MOTORS];
    AppMotorState states[APP_MAX_MOTORS];
    AppFileInfo files[APP_MAX_FILES];
    char content[APP_CONTENT_MAX_LEN];
} BinaryScratch_t;

static BinaryScratch_t g_binary_scratch;

typedef enum {
    BIN_SEND_OK = 0,
    BIN_SEND_QUEUED,
    BIN_SEND_TOO_LARGE,
    BIN_SEND_TX_BUSY
} BinarySendStatus;

#define BIN_PONG_PAYLOAD_SIZE              12u
#define BIN_PING_TIME_FMT_LOCAL_TIME_V1   0x01u
#define BIN_PING_TIME_PAYLOAD_SIZE        12u
#define BIN_OPERATE_TIME_PAYLOAD_SIZE     APP_OPERATE_TIME_PAYLOAD_SIZE
#define BIN_OPERATE_TIME_FORMAT_VERSION   1u
#define BIN_OPERATE_TIME_DAY_COUNT        7u

/* ============================================================================
 * Little-Endian Helpers
 * ============================================================================ */

static inline uint8_t *write_u8(uint8_t *p, uint8_t v) {
    *p++ = v;
    return p;
}

static inline uint8_t *write_u16le(uint8_t *p, uint16_t v) {
    *p++ = (uint8_t)(v & 0xFFu);
    *p++ = (uint8_t)(v >> 8u);
    return p;
}

static inline uint8_t *write_u32le(uint8_t *p, uint32_t v) {
    *p++ = (uint8_t)(v       & 0xFFu);
    *p++ = (uint8_t)((v >>  8u) & 0xFFu);
    *p++ = (uint8_t)((v >> 16u) & 0xFFu);
    *p++ = (uint8_t)((v >> 24u) & 0xFFu);
    return p;
}

static inline uint16_t read_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static inline uint32_t read_u32le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8u)
         | ((uint32_t)p[2] << 16u)
         | ((uint32_t)p[3] << 24u);
}

/** Clamp int32 to uint16 range [0, 65535]. */
static inline uint16_t clamp_u16(int32_t v) {
    if (v < 0)     return 0u;
    if (v > 65535) return 65535u;
    return (uint16_t)v;
}

static BinarySendStatus FinalizeBufferedResponse(BinaryContext *ctx,
                                                 uint8_t tar_id,
                                                 uint8_t cmd,
                                                 BinStatus status,
                                                 uint16_t payload_len);
static void SendErrorForStatus(BinaryContext *ctx, uint8_t tar_id, uint8_t cmd,
                               BinarySendStatus send_status);
static void SendErrorResponse(BinaryContext *ctx,
                              uint8_t tar_id,
                              uint8_t cmd,
                              BinErrorCode code,
                              const char *msg);
static uint8_t *WritePingStatusPayload(uint8_t *p, const AppPingStatus *status);
static bool ParsePingHostDateTime(const uint8_t *payload, uint16_t payload_len,
                                  AppHostDateTime *out_host_time);
static bool ValidateOperateTimePayload(const uint8_t *payload, uint16_t payload_len);
static void RecordTxBusyDrop(BinaryContext *ctx);
static BinarySendStatus QueueResponseFrame(BinaryContext *ctx,
                                           uint8_t tar_id,
                                           uint8_t cmd,
                                           BinStatus status,
                                           const uint8_t *payload,
                                           uint16_t payload_len);
static void TryFlushPendingResponse(BinaryContext *ctx);
static bool BoundedCStrLen(const char *s, size_t max_scan, uint16_t *out_len);
static uint8_t BoundedLen255(const char *s);
static bool ValidateResponsePayloadLen(uint32_t payload_len_u32,
                                       uint16_t *payload_len_out);

static void RecordTxBusyDrop(BinaryContext *ctx)
{
    ctx->tx_busy_drop_count++;

    if (ctx->frag_tx.on_log != NULL) {
        ctx->frag_tx.on_log("binary_com: TX busy, response dropped",
                            ctx->frag_tx.callback_user_data);
    }
}

static BinarySendStatus QueueResponseFrame(BinaryContext *ctx,
                                           uint8_t tar_id,
                                           uint8_t cmd,
                                           BinStatus status,
                                           const uint8_t *payload,
                                           uint16_t payload_len)
{
    uint8_t *p = ctx->pending_tx_buffer;
    uint32_t total = (uint32_t)BIN_RESP_HEADER_SIZE + (uint32_t)payload_len;

    if (total > BIN_TX_BUFFER_SIZE) {
        return BIN_SEND_TOO_LARGE;
    }
    if (ctx->pending_tx_valid) {
        RecordTxBusyDrop(ctx);
        return BIN_SEND_TX_BUSY;
    }
    if (payload_len > 0u && payload == NULL) {
        RecordTxBusyDrop(ctx);
        return BIN_SEND_TX_BUSY;
    }

    p = write_u8(p, ctx->my_id);
    p = write_u8(p, tar_id);
    p = write_u8(p, cmd);
    p = write_u8(p, (uint8_t)status);
    p = write_u16le(p, payload_len);
    if (payload_len > 0u) {
        memcpy(p, payload, payload_len);
    }

    ctx->pending_tx_len = total;
    ctx->pending_tx_dest_addr = ctx->current_source_addr;
    ctx->pending_tx_valid = true;
    return BIN_SEND_QUEUED;
}

static void TryFlushPendingResponse(BinaryContext *ctx)
{
    if (!ctx->pending_tx_valid) {
        return;
    }
    if (frag_tx_is_busy(&ctx->frag_tx)) {
        return;
    }

    if (frag_tx_send(&ctx->frag_tx,
                     ctx->pending_tx_buffer,
                     ctx->pending_tx_len,
                     ctx->pending_tx_dest_addr) == 0u) {
        return;
    }

    ctx->pending_tx_valid = false;
    ctx->pending_tx_len = 0u;
    ctx->pending_tx_dest_addr = 0u;
}

static bool BoundedCStrLen(const char *s, size_t max_scan, uint16_t *out_len)
{
    size_t len = 0u;

    if (s == NULL || out_len == NULL) {
        return false;
    }

    while (len < max_scan && s[len] != '\0') {
        len++;
    }

    if (len == max_scan) {
        return false;
    }

    *out_len = (uint16_t)len;
    return true;
}

static uint8_t BoundedLen255(const char *s)
{
    size_t len = 0u;

    if (s == NULL) {
        return 0u;
    }

    while (len < 255u && s[len] != '\0') {
        len++;
    }

    return (uint8_t)len;
}

static bool ValidateResponsePayloadLen(uint32_t payload_len_u32,
                                       uint16_t *payload_len_out)
{
    if (payload_len_out == NULL) {
        return false;
    }
    if (payload_len_u32 > UINT16_MAX) {
        return false;
    }
    if ((uint32_t)BIN_RESP_HEADER_SIZE + payload_len_u32 > BIN_TX_BUFFER_SIZE) {
        return false;
    }

    *payload_len_out = (uint16_t)payload_len_u32;
    return true;
}

/* ============================================================================
 * Response Sending Helpers
 * ============================================================================ */

/**
 * @brief Assemble a complete binary response in ctx->tx_buffer and send it.
 *
 * Layout: [BinRespHeader 6B] [payload payload_len B]
 *
 * @param ctx         BinaryContext
 * @param tar_id      Target (requester) device ID
 * @param cmd         Command being responded to
 * @param status      BIN_STATUS_OK or BIN_STATUS_ERROR
 * @param payload     Pointer to payload bytes (may be NULL if payload_len==0)
 * @param payload_len Number of payload bytes
 */
static BinarySendStatus SendBinaryResponse(BinaryContext *ctx,
                                           uint8_t tar_id,
                                           uint8_t cmd,
                                           BinStatus status,
                                           const uint8_t *payload,
                                           uint16_t payload_len)
{
    uint32_t total = (uint32_t)BIN_RESP_HEADER_SIZE + (uint32_t)payload_len;

    if (total > BIN_TX_BUFFER_SIZE) {
        return BIN_SEND_TOO_LARGE;
    }

    if (frag_tx_is_busy(&ctx->frag_tx)) {
        return QueueResponseFrame(ctx, tar_id, cmd, status, payload, payload_len);
    }

    uint8_t *p = ctx->tx_buffer;

    /* Response header (6 bytes, all single-byte fields except payload_len) */
    p = write_u8(p, ctx->my_id);
    p = write_u8(p, tar_id);
    p = write_u8(p, cmd);
    p = write_u8(p, (uint8_t)status);
    p = write_u16le(p, payload_len);

    /* Payload */
    if (payload != NULL && payload_len > 0u) {
        memcpy(p, payload, payload_len);
    }

    ctx->tx_buffer_len = total;
    if (frag_tx_send(&ctx->frag_tx, ctx->tx_buffer, total,
                     ctx->current_source_addr) == 0u) {
        return QueueResponseFrame(ctx, tar_id, cmd, status, payload, payload_len);
    }

    return BIN_SEND_OK;
}

static BinarySendStatus FinalizeBufferedResponse(BinaryContext *ctx,
                                                 uint8_t tar_id,
                                                 uint8_t cmd,
                                                 BinStatus status,
                                                 uint16_t payload_len)
{
    const uint8_t *payload = ctx->tx_buffer + BIN_RESP_HEADER_SIZE;

    if (frag_tx_is_busy(&ctx->frag_tx)) {
        return QueueResponseFrame(ctx, tar_id, cmd, status, payload, payload_len);
    }

    uint8_t *hdr = ctx->tx_buffer;
    hdr = write_u8(hdr, ctx->my_id);
    hdr = write_u8(hdr, tar_id);
    hdr = write_u8(hdr, cmd);
    hdr = write_u8(hdr, (uint8_t)status);
    (void)write_u16le(hdr, payload_len);

    ctx->tx_buffer_len = (uint32_t)BIN_RESP_HEADER_SIZE + (uint32_t)payload_len;
    if (frag_tx_send(&ctx->frag_tx, ctx->tx_buffer, ctx->tx_buffer_len,
                     ctx->current_source_addr) == 0u) {
        return QueueResponseFrame(ctx, tar_id, cmd, status, payload, payload_len);
    }

    return BIN_SEND_OK;
}

static void SendErrorForStatus(BinaryContext *ctx, uint8_t tar_id, uint8_t cmd,
                               BinarySendStatus send_status)
{
    switch (send_status) {
        case BIN_SEND_TOO_LARGE:
            SendErrorResponse(ctx, tar_id, cmd, ERR_RESPONSE_TOO_LARGE,
                              "Response too large");
            break;

        case BIN_SEND_TX_BUSY:
            /*
             * BUSY while queue is already occupied:
             * do not recurse into additional CMD_ERROR responses.
             */
            break;

        case BIN_SEND_QUEUED:
        case BIN_SEND_OK:
        default:
            break;
    }
}

/**
 * @brief Send a CMD_ERROR response.
 *
 * Payload: error_code(1) | msg_len(1) | message(msg_len bytes, UTF-8, no NUL)
 *
 * @param ctx  BinaryContext
 * @param tar_id  Target device ID
 * @param cmd     Original command that failed
 * @param code    Error code
 * @param msg     Optional error message string (may be NULL)
 */
static void SendErrorResponse(BinaryContext *ctx,
                               uint8_t tar_id,
                               uint8_t cmd,
                               BinErrorCode code,
                               const char *msg)
{
    uint8_t  err_payload[258];  /* 1 (code) + 1 (msg_len) + 255 (max message) + 1 spare */
    uint8_t  msg_len = 0u;
    uint8_t *p = err_payload;

    if (msg != NULL) {
        msg_len = BoundedLen255(msg);
    }

    p = write_u8(p, (uint8_t)code);
    p = write_u8(p, msg_len);
    if (msg_len > 0u) {
        memcpy(p, msg, msg_len);
    }

    uint16_t err_payload_len = (uint16_t)(2u + msg_len);
    (void)SendBinaryResponse(ctx, tar_id, (uint8_t)CMD_ERROR,
                             BIN_STATUS_ERROR, err_payload, err_payload_len);
}

static uint8_t *WritePingStatusPayload(uint8_t *p, const AppPingStatus *status)
{
    p = write_u8(p, (uint8_t)status->state);
    p = write_u8(p, status->init_state);
    p = write_u32le(p, status->current_ms);
    p = write_u32le(p, status->total_ms);
    p = write_u8(p, status->power_status);
    p = write_u8(p, status->error_status);
    return p;
}

static bool NormalizeCountryCodeByte(uint8_t c, char *out)
{
    if (out == NULL) {
        return false;
    }
    if (c >= (uint8_t)'A' && c <= (uint8_t)'Z') {
        *out = (char)c;
        return true;
    }
    if (c >= (uint8_t)'a' && c <= (uint8_t)'z') {
        *out = (char)(c - ((uint8_t)'a' - (uint8_t)'A'));
        return true;
    }
    return false;
}

static bool ParsePingHostDateTime(const uint8_t *payload, uint16_t payload_len,
                                  AppHostDateTime *out_host_time)
{
    char country0;
    char country1;
    int16_t utc_offset_min;

    if (out_host_time == NULL) {
        return false;
    }
    if (payload == NULL || payload_len != BIN_PING_TIME_PAYLOAD_SIZE) {
        return false;
    }
    if (payload[0] != BIN_PING_TIME_FMT_LOCAL_TIME_V1) {
        return false;
    }
    if (!NormalizeCountryCodeByte(payload[1], &country0) ||
        !NormalizeCountryCodeByte(payload[2], &country1)) {
        return false;
    }

    out_host_time->country_code[0] = country0;
    out_host_time->country_code[1] = country1;
    out_host_time->country_code[2] = '\0';
    out_host_time->year = read_u16le(payload + 3u);
    out_host_time->month = payload[5];
    out_host_time->day = payload[6];
    out_host_time->hour = payload[7];
    out_host_time->minute = payload[8];
    out_host_time->second = payload[9];
    utc_offset_min = (int16_t)read_u16le(payload + 10u);
    out_host_time->utc_offset_min = utc_offset_min;

    if (out_host_time->year == 0u ||
        out_host_time->month < 1u || out_host_time->month > 12u ||
        out_host_time->day < 1u || out_host_time->day > 31u ||
        out_host_time->hour > 23u ||
        out_host_time->minute > 59u ||
        out_host_time->second > 59u) {
        return false;
    }
    if (utc_offset_min < -720 || utc_offset_min > 840) {
        return false;
    }

    return true;
}

static bool ValidateOperateTimePayload(const uint8_t *payload, uint16_t payload_len)
{
    if (payload == NULL || payload_len != BIN_OPERATE_TIME_PAYLOAD_SIZE) {
        return false;
    }
    if (payload[0] != BIN_OPERATE_TIME_FORMAT_VERSION) {
        return false;
    }
    if (payload[7] != BIN_OPERATE_TIME_DAY_COUNT) {
        return false;
    }

    for (uint8_t i = 0u; i < BIN_OPERATE_TIME_DAY_COUNT; i++) {
        uint8_t day_of_week = payload[8u + ((uint16_t)i * 5u)];
        if (day_of_week < 1u || day_of_week > 7u) {
            return false;
        }
    }

    return true;
}

/* ============================================================================
 * Command Handlers
 * ============================================================================ */

static void HandlePing(BinaryContext *ctx, uint8_t src_id,
                       const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len != 0u) {
        AppHostDateTime host_time;

        if (!ParsePingHostDateTime(payload, payload_len, &host_time)) {
            SendErrorResponse(ctx, src_id, (uint8_t)CMD_PONG, ERR_INVALID_INPUT, NULL);
            return;
        }
        if (!App_SetHostDateTime(&host_time)) {
            SendErrorResponse(ctx, src_id, (uint8_t)CMD_PONG, ERR_INVALID_PARAM, NULL);
            return;
        }
    }

    bool ok = App_Ping();
    if (ok) {
        AppPingStatus ping_status;
        uint8_t payload[BIN_PONG_PAYLOAD_SIZE];

        if (!App_GetPingStatus(&ping_status)) {
            SendErrorResponse(ctx, src_id, (uint8_t)CMD_PONG, ERR_UNKNOWN, NULL);
            return;
        }

        (void)WritePingStatusPayload(payload, &ping_status);
        BinarySendStatus send_status =
            SendBinaryResponse(ctx, src_id, (uint8_t)CMD_PONG, BIN_STATUS_OK,
                               payload, BIN_PONG_PAYLOAD_SIZE);
        SendErrorForStatus(ctx, src_id, (uint8_t)CMD_PONG, send_status);
    } else {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_PONG, ERR_UNKNOWN, NULL);
    }
}

static void HandleMove(BinaryContext *ctx, uint8_t src_id,
                       const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len != 3u) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_MOVE, ERR_INVALID_INPUT, NULL);
        return;
    }

    uint8_t  motor_id = payload[0];
    uint16_t pos16    = read_u16le(payload + 1u);
    int32_t  raw_pos  = (int32_t)pos16;

    bool ok = App_Move(motor_id, raw_pos);
    if (ok) {
        uint8_t resp[2];
        uint8_t *p = resp;
        p = write_u8(p, ctx->my_id);
        (void)write_u8(p, motor_id);
        BinarySendStatus send_status =
            SendBinaryResponse(ctx, src_id, (uint8_t)CMD_MOVE, BIN_STATUS_OK, resp, 2u);
        SendErrorForStatus(ctx, src_id, (uint8_t)CMD_MOVE, send_status);
    } else {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_MOVE, ERR_MOTOR_NOT_FOUND, NULL);
    }
}

static void HandleMotionCtrl(BinaryContext *ctx, uint8_t src_id,
                              const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < 1u) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_MOTION_CTRL, ERR_INVALID_INPUT, NULL);
        return;
    }

    MotionAction action = (MotionAction)payload[0];
    bool ok = false;

    switch (action) {
        case MOTION_ACTION_PLAY:
            ok = App_MotionPlay(ctx->my_id);
            break;

        case MOTION_ACTION_REPEAT_PLAY:
            if (payload_len != 1u) {
                SendErrorResponse(ctx, src_id, (uint8_t)CMD_MOTION_CTRL,
                                  ERR_INVALID_INPUT, NULL);
                return;
            }
            ok = App_MotionRepeatPlay(ctx->my_id);
            break;

        case MOTION_ACTION_STOP:
            ok = App_MotionStop(ctx->my_id);
            break;

        case MOTION_ACTION_PAUSE:
            ok = App_MotionPause(ctx->my_id);
            break;

        case MOTION_ACTION_SEEK:
            if (payload_len != 5u) {
                SendErrorResponse(ctx, src_id, (uint8_t)CMD_MOTION_CTRL,
                                  ERR_INVALID_INPUT, NULL);
                return;
            }
            {
                uint32_t time_ms = read_u32le(payload + 1u);
                ok = App_MotionSeek(ctx->my_id, time_ms);
            }
            break;

        default:
            SendErrorResponse(ctx, src_id, (uint8_t)CMD_MOTION_CTRL,
                              ERR_INVALID_PARAM, NULL);
            return;
    }

    if (ok) {
        uint8_t resp[2];
        uint8_t *p = resp;
        p = write_u8(p, (uint8_t)action);
        (void)write_u8(p, ctx->my_id);
        BinarySendStatus send_status =
            SendBinaryResponse(ctx, src_id, (uint8_t)CMD_MOTION_CTRL, BIN_STATUS_OK, resp, 2u);
        SendErrorForStatus(ctx, src_id, (uint8_t)CMD_MOTION_CTRL, send_status);
    } else {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_MOTION_CTRL, ERR_UNKNOWN, NULL);
    }
}

static void HandlePowerCtrl(BinaryContext *ctx, uint8_t src_id,
                            const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len != 1u) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_POWER_CTRL, ERR_INVALID_INPUT, NULL);
        return;
    }

    PowerAction action = (PowerAction)payload[0];
    switch (action) {
        case POWER_ACTION_OFF:
        case POWER_ACTION_ON:
        case POWER_ACTION_REBOOT:
            break;

        default:
            SendErrorResponse(ctx, src_id, (uint8_t)CMD_POWER_CTRL,
                              ERR_INVALID_PARAM, NULL);
            return;
    }

    bool accepted = App_PowerControl((uint8_t)action);
    uint8_t resp[2];
    uint8_t *p = resp;
    p = write_u8(p, (uint8_t)action);
    (void)write_u8(p, accepted ? 0x01u : 0x00u);
    BinarySendStatus send_status =
        SendBinaryResponse(ctx, src_id, (uint8_t)CMD_POWER_CTRL, BIN_STATUS_OK, resp, 2u);
    SendErrorForStatus(ctx, src_id, (uint8_t)CMD_POWER_CTRL, send_status);
}

static void HandleErrorClear(BinaryContext *ctx, uint8_t src_id,
                             const uint8_t *payload, uint16_t payload_len)
{
    (void)payload;

    if (payload_len != 0u) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_ERROR_CLEAR,
                          ERR_INVALID_INPUT, NULL);
        return;
    }

    if (!App_ErrorClear()) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_ERROR_CLEAR,
                          ERR_UNKNOWN, NULL);
        return;
    }

    BinarySendStatus send_status =
        SendBinaryResponse(ctx, src_id, (uint8_t)CMD_ERROR_CLEAR,
                           BIN_STATUS_OK, NULL, 0u);
    SendErrorForStatus(ctx, src_id, (uint8_t)CMD_ERROR_CLEAR, send_status);
}

static void HandleSetOperateTime(BinaryContext *ctx, uint8_t src_id,
                                 const uint8_t *payload, uint16_t payload_len)
{
    if (!ValidateOperateTimePayload(payload, payload_len)) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_SET_OPERATE_TIME,
                          ERR_INVALID_INPUT, NULL);
        return;
    }

    if (!App_SetOperateTime(payload, payload_len)) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_SET_OPERATE_TIME,
                          ERR_UNKNOWN, NULL);
        return;
    }

    uint32_t schedule_checksum = read_u32le(payload + 3u);
    uint8_t resp[4];
    (void)write_u32le(resp, schedule_checksum);
    BinarySendStatus send_status =
        SendBinaryResponse(ctx, src_id, (uint8_t)CMD_SET_OPERATE_TIME,
                           BIN_STATUS_OK, resp, 4u);
    SendErrorForStatus(ctx, src_id, (uint8_t)CMD_SET_OPERATE_TIME, send_status);
}

static void HandleGetOperateTime(BinaryContext *ctx, uint8_t src_id,
                                 const uint8_t *payload, uint16_t payload_len)
{
    uint8_t resp[BIN_OPERATE_TIME_PAYLOAD_SIZE];
    uint16_t resp_len = 0u;

    (void)payload;

    if (payload_len != 0u) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_OPERATE_TIME,
                          ERR_INVALID_INPUT, NULL);
        return;
    }

    if (!App_GetOperateTime(resp, BIN_OPERATE_TIME_PAYLOAD_SIZE, &resp_len) ||
        resp_len != BIN_OPERATE_TIME_PAYLOAD_SIZE ||
        !ValidateOperateTimePayload(resp, resp_len)) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_OPERATE_TIME,
                          ERR_UNKNOWN, NULL);
        return;
    }

    BinarySendStatus send_status =
        SendBinaryResponse(ctx, src_id, (uint8_t)CMD_GET_OPERATE_TIME,
                           BIN_STATUS_OK, resp, resp_len);
    SendErrorForStatus(ctx, src_id, (uint8_t)CMD_GET_OPERATE_TIME, send_status);
}

static void HandleGetMotors(BinaryContext *ctx, uint8_t src_id)
{
    /* Static allocation prevents stack overflow (32 × sizeof(AppMotorInfo)) */
    AppMotorInfo *motors = g_binary_scratch.motors;

    int count = App_GetMotors(motors, APP_MAX_MOTORS);
    if (count < 0) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_MOTORS, ERR_UNKNOWN, NULL);
        return;
    }
    if (count > APP_MAX_MOTORS) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_MOTORS,
                          ERR_INVALID_PARAM, "Invalid motor count");
        return;
    }

    /*
     * Payload: motor_count(1) + motors[] each 17 bytes
     *   id(1) group_id(1) sub_id(1) type(1) status(1)
     *   position(2 LE) velocity(2 LE ×100)
     *   min_angle(2 LE ×10 int16) max_angle(2 LE ×10 int16)
     *   min_raw(2 LE) max_raw(2 LE)
     */
    uint32_t payload_len_u32 = 1u + (uint32_t)count * 17u;
    uint16_t payload_len = 0u;
    if (!ValidateResponsePayloadLen(payload_len_u32, &payload_len)) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_MOTORS,
                          ERR_RESPONSE_TOO_LARGE, "Response too large");
        return;
    }

    uint8_t *p = ctx->tx_buffer + BIN_RESP_HEADER_SIZE;

    p = write_u8(p, (uint8_t)count);

    for (int i = 0; i < count; i++) {
        p = write_u8(p, motors[i].id);
        p = write_u8(p, motors[i].group_id);
        p = write_u8(p, motors[i].sub_id);
        p = write_u8(p, (uint8_t)motors[i].type);
        p = write_u8(p, (uint8_t)motors[i].status);
        p = write_u16le(p, clamp_u16(motors[i].position));
        p = write_u16le(p, (uint16_t)(motors[i].velocity * 100.0f + 0.5f));
        p = write_u16le(p, (uint16_t)(int16_t)(motors[i].min_angle * 10.0f));
        p = write_u16le(p, (uint16_t)(int16_t)(motors[i].max_angle * 10.0f));
        p = write_u16le(p, clamp_u16(motors[i].min_raw));
        p = write_u16le(p, clamp_u16(motors[i].max_raw));
    }

    SendErrorForStatus(ctx, src_id, (uint8_t)CMD_GET_MOTORS,
                       FinalizeBufferedResponse(ctx, src_id,
                                                (uint8_t)CMD_GET_MOTORS,
                                                BIN_STATUS_OK, payload_len));
}

static void HandleGetMotorState(BinaryContext *ctx, uint8_t src_id)
{
    AppMotorState *states = g_binary_scratch.states;

    int count = App_GetMotorState(states, APP_MAX_MOTORS);
    if (count < 0) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_MOTOR_STATE, ERR_UNKNOWN, NULL);
        return;
    }
    if (count > APP_MAX_MOTORS) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_MOTOR_STATE,
                          ERR_INVALID_PARAM, "Invalid motor count");
        return;
    }

    /*
     * Payload: motor_count(1) + states[] each 6 bytes
     *   id(1) position(2 LE) velocity(2 LE ×100) status(1)
     */
    uint32_t payload_len_u32 = 1u + (uint32_t)count * 6u;
    uint16_t payload_len = 0u;
    if (!ValidateResponsePayloadLen(payload_len_u32, &payload_len)) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_MOTOR_STATE,
                          ERR_RESPONSE_TOO_LARGE, "Response too large");
        return;
    }

    uint8_t *p = ctx->tx_buffer + BIN_RESP_HEADER_SIZE;

    p = write_u8(p, (uint8_t)count);

    for (int i = 0; i < count; i++) {
        p = write_u8(p, states[i].id);
        p = write_u16le(p, clamp_u16(states[i].position));
        p = write_u16le(p, (uint16_t)(states[i].velocity * 100.0f + 0.5f));
        p = write_u8(p, (uint8_t)states[i].status);
    }

    SendErrorForStatus(ctx, src_id, (uint8_t)CMD_GET_MOTOR_STATE,
                       FinalizeBufferedResponse(ctx, src_id,
                                                (uint8_t)CMD_GET_MOTOR_STATE,
                                                BIN_STATUS_OK, payload_len));
}

static void HandleGetFiles(BinaryContext *ctx, uint8_t src_id)
{
    AppFileInfo *files = g_binary_scratch.files;

    int count = App_GetFiles(files, APP_MAX_FILES);
    if (count < 0) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILES, ERR_UNKNOWN, NULL);
        return;
    }
    if (count > APP_MAX_FILES) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILES,
                          ERR_INVALID_PARAM, "Invalid file count");
        return;
    }

    /*
     * Payload:
     *   entry_count(2 LE)
     *   entries[]:
     *     flags(1) parent_index(2 LE int16) size(4 LE)
     *     name_len(1) name(name_len bytes)
     *     path_len(2 LE) path(path_len bytes)
     */
    uint8_t *p = ctx->tx_buffer + BIN_RESP_HEADER_SIZE;
    uint8_t *payload_start = p;

    p = write_u16le(p, (uint16_t)count);

    for (int i = 0; i < count; i++) {
        uint16_t name_len_u16 = 0u;
        uint16_t path_len_u16 = 0u;

        if (!BoundedCStrLen(files[i].name, APP_NAME_MAX_LEN, &name_len_u16) ||
            !BoundedCStrLen(files[i].path, APP_PATH_MAX_LEN, &path_len_u16)) {
            SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILES,
                              ERR_INVALID_PARAM, "Invalid file entry");
            return;
        }

        uint8_t flags    = files[i].is_directory ? 0x01u : 0x00u;
        uint8_t name_len = (uint8_t)name_len_u16;
        uint16_t path_len = path_len_u16;

        ptrdiff_t used = p - ctx->tx_buffer;
        uint32_t needed = (uint32_t)used + 1u + 2u + 4u + 1u + name_len + 2u + path_len;
        if (needed > BIN_TX_BUFFER_SIZE) {
            SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILES,
                              ERR_RESPONSE_TOO_LARGE, "Response too large");
            return;
        }

        p = write_u8(p, flags);
        p = write_u16le(p, (uint16_t)(int16_t)files[i].parent_index);
        p = write_u32le(p, files[i].size);
        p = write_u8(p, name_len);
        memcpy(p, files[i].name, name_len);
        p += name_len;
        p = write_u16le(p, path_len);
        memcpy(p, files[i].path, path_len);
        p += path_len;
    }

    uint32_t payload_len_u32 = (uint32_t)(p - payload_start);
    uint16_t payload_len = 0u;
    if (!ValidateResponsePayloadLen(payload_len_u32, &payload_len)) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILES,
                          ERR_RESPONSE_TOO_LARGE, "Response too large");
        return;
    }

    SendErrorForStatus(ctx, src_id, (uint8_t)CMD_GET_FILES,
                       FinalizeBufferedResponse(ctx, src_id,
                                                (uint8_t)CMD_GET_FILES,
                                                BIN_STATUS_OK, payload_len));
}

static void HandleGetFile(BinaryContext *ctx, uint8_t src_id,
                          const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < 2u) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILE, ERR_INVALID_INPUT, NULL);
        return;
    }

    uint16_t path_len = read_u16le(payload);
    if ((uint32_t)2u + path_len != payload_len) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILE, ERR_INVALID_INPUT, NULL);
        return;
    }
    if (path_len >= APP_PATH_MAX_LEN) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILE, ERR_INVALID_PARAM, NULL);
        return;
    }

    char path_buf[APP_PATH_MAX_LEN];
    memcpy(path_buf, payload + 2u, path_len);
    path_buf[path_len] = '\0';

    char *content_buf = g_binary_scratch.content;
    bool ok = App_GetFile(path_buf, content_buf, APP_CONTENT_MAX_LEN);
    if (!ok) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILE, ERR_FILE_NOT_FOUND, NULL);
        return;
    }

    uint16_t content_len = 0u;
    if (!BoundedCStrLen(content_buf, APP_CONTENT_MAX_LEN, &content_len)) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILE,
                          ERR_INVALID_PARAM, "Invalid file content");
        return;
    }

    /*
     * Response payload:
     *   path_len(2 LE) path(path_len) content_len(2 LE) content(content_len)
     */
    uint16_t resp_payload_len = (uint16_t)(2u + path_len + 2u + content_len);

    if ((uint32_t)BIN_RESP_HEADER_SIZE + resp_payload_len > BIN_TX_BUFFER_SIZE) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_GET_FILE,
                          ERR_RESPONSE_TOO_LARGE, "Response too large");
        return;
    }

    uint8_t *p = ctx->tx_buffer + BIN_RESP_HEADER_SIZE;
    p = write_u16le(p, path_len);
    memcpy(p, path_buf, path_len);
    p += path_len;
    p = write_u16le(p, content_len);
    memcpy(p, content_buf, content_len);

    SendErrorForStatus(ctx, src_id, (uint8_t)CMD_GET_FILE,
                       FinalizeBufferedResponse(ctx, src_id,
                                                (uint8_t)CMD_GET_FILE,
                                                BIN_STATUS_OK, resp_payload_len));
}

static void HandleSaveFile(BinaryContext *ctx, uint8_t src_id,
                           const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < 4u) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_SAVE_FILE, ERR_INVALID_INPUT, NULL);
        return;
    }

    uint16_t path_len = read_u16le(payload);
    if ((uint32_t)path_len + 4u > payload_len) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_SAVE_FILE, ERR_INVALID_INPUT, NULL);
        return;
    }
    if (path_len >= APP_PATH_MAX_LEN) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_SAVE_FILE, ERR_INVALID_PARAM, NULL);
        return;
    }

    char path_buf[APP_PATH_MAX_LEN];
    memcpy(path_buf, payload + 2u, path_len);
    path_buf[path_len] = '\0';

    uint16_t content_len = read_u16le(payload + 2u + path_len);
    const uint8_t *content_ptr = payload + 2u + path_len + 2u;

    if ((uint32_t)2u + path_len + 2u + content_len != payload_len) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_SAVE_FILE, ERR_INVALID_INPUT, NULL);
        return;
    }
    if (content_len >= APP_CONTENT_MAX_LEN) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_SAVE_FILE, ERR_INVALID_PARAM, NULL);
        return;
    }

    char *content_buf = g_binary_scratch.content;
    memcpy(content_buf, content_ptr, content_len);
    content_buf[content_len] = '\0';

    bool ok = App_SaveFile(path_buf, content_buf);
    if (!ok) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_SAVE_FILE, ERR_FILE_NOT_FOUND, NULL);
        return;
    }

    /* Response payload: path_len(2 LE) path(path_len) */
    uint16_t resp_payload_len = (uint16_t)(2u + path_len);

    if ((uint32_t)BIN_RESP_HEADER_SIZE + resp_payload_len > BIN_TX_BUFFER_SIZE) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_SAVE_FILE,
                          ERR_RESPONSE_TOO_LARGE, "Response too large");
        return;
    }

    uint8_t *p = ctx->tx_buffer + BIN_RESP_HEADER_SIZE;
    p = write_u16le(p, path_len);
    memcpy(p, path_buf, path_len);

    SendErrorForStatus(ctx, src_id, (uint8_t)CMD_SAVE_FILE,
                       FinalizeBufferedResponse(ctx, src_id,
                                                (uint8_t)CMD_SAVE_FILE,
                                                BIN_STATUS_OK, resp_payload_len));
}

static void HandleVerifyFile(BinaryContext *ctx, uint8_t src_id,
                             const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < 4u) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_VERIFY_FILE, ERR_INVALID_INPUT, NULL);
        return;
    }

    uint16_t path_len = read_u16le(payload);
    if ((uint32_t)path_len + 4u > payload_len) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_VERIFY_FILE, ERR_INVALID_INPUT, NULL);
        return;
    }
    if (path_len >= APP_PATH_MAX_LEN) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_VERIFY_FILE, ERR_INVALID_PARAM, NULL);
        return;
    }

    char path_buf[APP_PATH_MAX_LEN];
    memcpy(path_buf, payload + 2u, path_len);
    path_buf[path_len] = '\0';

    uint16_t content_len = read_u16le(payload + 2u + path_len);
    const uint8_t *content_ptr = payload + 2u + path_len + 2u;

    if ((uint32_t)2u + path_len + 2u + content_len != payload_len) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_VERIFY_FILE, ERR_INVALID_INPUT, NULL);
        return;
    }
    if (content_len >= APP_CONTENT_MAX_LEN) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_VERIFY_FILE, ERR_INVALID_PARAM, NULL);
        return;
    }

    char *content_buf = g_binary_scratch.content;
    memcpy(content_buf, content_ptr, content_len);
    content_buf[content_len] = '\0';

    bool match = false;
    bool ok = App_VerifyFile(path_buf, content_buf, &match);
    if (!ok) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_VERIFY_FILE, ERR_FILE_NOT_FOUND, NULL);
        return;
    }

    /* Response payload: path_len(2 LE) path(path_len) match(1) */
    uint16_t resp_payload_len = (uint16_t)(2u + path_len + 1u);

    if ((uint32_t)BIN_RESP_HEADER_SIZE + resp_payload_len > BIN_TX_BUFFER_SIZE) {
        SendErrorResponse(ctx, src_id, (uint8_t)CMD_VERIFY_FILE,
                          ERR_RESPONSE_TOO_LARGE, "Response too large");
        return;
    }

    uint8_t *p = ctx->tx_buffer + BIN_RESP_HEADER_SIZE;
    p = write_u16le(p, path_len);
    memcpy(p, path_buf, path_len);
    p += path_len;
    p = write_u8(p, match ? 0x01u : 0x00u);

    SendErrorForStatus(ctx, src_id, (uint8_t)CMD_VERIFY_FILE,
                       FinalizeBufferedResponse(ctx, src_id,
                                                (uint8_t)CMD_VERIFY_FILE,
                                                BIN_STATUS_OK, resp_payload_len));
}

/* ============================================================================
 * Packet Dispatcher
 * ============================================================================ */

/**
 * @brief Parse and dispatch a fully reassembled binary message.
 *
 * Called from OnFragRxMessage after Fragment Protocol delivers the complete payload.
 * Applies tar_id filtering, validates payload_len, then dispatches to a handler.
 */
static void HandleBinaryPacket(BinaryContext *ctx, const uint8_t *data, uint32_t len)
{
    if (len < (uint32_t)BIN_REQ_HEADER_SIZE) {
        return;
    }

    /* Copy header bytes to local struct (avoid unaligned access) */
    BinReqHeader hdr;
    memcpy(&hdr, data, BIN_REQ_HEADER_SIZE);
    /* Explicitly decode LE payload_len ??the packed struct may still have
       endianness issues depending on compiler/target */
    hdr.payload_len = read_u16le(data + 3u);

    /* Filter: only process messages directed at us or broadcast */
    if (hdr.tar_id != ctx->my_id && hdr.tar_id != BINARY_BROADCAST_ID) {
        return;
    }

    /* Validate total length */
    if ((uint32_t)BIN_REQ_HEADER_SIZE + (uint32_t)hdr.payload_len != len) {
        SendErrorResponse(ctx, hdr.src_id, hdr.cmd, ERR_INVALID_INPUT, NULL);
        return;
    }

    const uint8_t *payload = data + BIN_REQ_HEADER_SIZE;

    switch ((BinCmd)hdr.cmd) {
        case CMD_PING:
            HandlePing(ctx, hdr.src_id, payload, hdr.payload_len);
            break;

        case CMD_MOVE:
            HandleMove(ctx, hdr.src_id, payload, hdr.payload_len);
            break;

        case CMD_MOTION_CTRL:
            HandleMotionCtrl(ctx, hdr.src_id, payload, hdr.payload_len);
            break;

        case CMD_POWER_CTRL:
            HandlePowerCtrl(ctx, hdr.src_id, payload, hdr.payload_len);
            break;

        case CMD_ERROR_CLEAR:
            HandleErrorClear(ctx, hdr.src_id, payload, hdr.payload_len);
            break;

        case CMD_GET_MOTORS:
            HandleGetMotors(ctx, hdr.src_id);
            break;

        case CMD_GET_MOTOR_STATE:
            HandleGetMotorState(ctx, hdr.src_id);
            break;

        case CMD_GET_FILES:
            HandleGetFiles(ctx, hdr.src_id);
            break;

        case CMD_GET_FILE:
            HandleGetFile(ctx, hdr.src_id, payload, hdr.payload_len);
            break;

        case CMD_SAVE_FILE:
            HandleSaveFile(ctx, hdr.src_id, payload, hdr.payload_len);
            break;

        case CMD_VERIFY_FILE:
            HandleVerifyFile(ctx, hdr.src_id, payload, hdr.payload_len);
            break;

        case CMD_SET_OPERATE_TIME:
            HandleSetOperateTime(ctx, hdr.src_id, payload, hdr.payload_len);
            break;

        case CMD_GET_OPERATE_TIME:
            HandleGetOperateTime(ctx, hdr.src_id, payload, hdr.payload_len);
            break;

        default:
            SendErrorResponse(ctx, hdr.src_id, hdr.cmd, ERR_UNKNOWN_CMD, NULL);
            break;
    }
}

/* ============================================================================
 * XBee / Fragment Callbacks
 * ============================================================================ */

static void OnXBeeFrame(const XBeeFrame_t *frame, void *user_data)
{
    BinaryContext *ctx = (BinaryContext *)user_data;
    const uint8_t *rf_data     = NULL;
    uint16_t       rf_data_len = 0u;
    uint64_t       src_addr    = 0u;

    if (frame->frame_type == XBEE_FRAME_RX_PACKET) {
        rf_data     = frame->parsed.rx_packet.rf_data;
        rf_data_len = frame->parsed.rx_packet.rf_data_len;
        src_addr    = frame->parsed.rx_packet.source_addr64;
    } else if (frame->frame_type == XBEE_FRAME_EXPLICIT_RX) {
        rf_data     = frame->parsed.explicit_rx.rf_data;
        rf_data_len = frame->parsed.explicit_rx.rf_data_len;
        src_addr    = frame->parsed.explicit_rx.source_addr64;
    } else {
        return;  /* TX status and other frames ??no action needed */
    }

    if (rf_data == NULL || rf_data_len == 0u) {
        return;
    }

    /* Route NACK/DONE control messages to FragTX for retransmission handling */
    if (frag_rx_is_nack(rf_data, rf_data_len)) {
        NackMessage_t nack;
        if (frag_rx_parse_nack(rf_data, rf_data_len, &nack)) {
            frag_tx_handle_nack(&ctx->frag_tx, &nack, src_addr);
        }
    } else if (frag_rx_is_done(rf_data, rf_data_len)) {
        uint16_t msg_id;
        if (frag_rx_parse_done(rf_data, rf_data_len, &msg_id)) {
            frag_tx_handle_done(&ctx->frag_tx, msg_id, src_addr);
        }
    } else {
        /* Normal data fragment ??pass to Fragment RX reassembler */
        frag_rx_process(&ctx->frag_rx, rf_data, rf_data_len, src_addr);
    }
}

static void OnXBeeError(const char *error, void *user_data)
{
    (void)error;
    (void)user_data;
    /* Errors are silently ignored ??not critical for operation */
}

static void OnFragRxMessage(const uint8_t *data, uint32_t len,
                             uint64_t source_addr, void *user_data)
{
    BinaryContext *ctx = (BinaryContext *)user_data;

    /* Store source address so handlers can reply to the correct device */
    ctx->current_source_addr = source_addr;

    HandleBinaryPacket(ctx, data, len);
}

static void OnFragRxLog(const char *message, void *user_data)
{
    (void)message;
    (void)user_data;
}

static void OnFragTxComplete(uint16_t msg_id, bool success, void *user_data)
{
    (void)msg_id;
    (void)success;
    (void)user_data;
}

static void OnFragTxLog(const char *message, void *user_data)
{
    (void)message;
    (void)user_data;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void BIN_COM_Init(BinaryContext *ctx, UART_Context *uart, uint8_t my_id)
{
    memset(ctx, 0, sizeof(BinaryContext));
    ctx->uart  = uart;
    ctx->my_id = my_id;

    /* Initialize XBee */
    xbee_init(&ctx->xbee, uart);
    xbee_set_callbacks(&ctx->xbee, OnXBeeFrame, OnXBeeError, ctx);

    /* Initialize Fragment RX */
    frag_rx_init(&ctx->frag_rx, &ctx->xbee);
    frag_rx_set_callbacks(&ctx->frag_rx, OnFragRxMessage, OnFragRxLog, ctx);

    /* Initialize Fragment TX */
    frag_tx_init(&ctx->frag_tx, &ctx->xbee);
    frag_tx_set_callbacks(&ctx->frag_tx, OnFragTxComplete, OnFragTxLog, ctx);
}

void BIN_COM_Process(BinaryContext *ctx)
{
    /* Read UART bytes and feed them into the XBee frame parser */
    xbee_process(&ctx->xbee);

    /* Advance Fragment TX state machine (sends pending fragments) */
    frag_tx_tick(&ctx->frag_tx);

    /* Flush one-slot queued response when TX session becomes available. */
    TryFlushPendingResponse(ctx);
}

void BIN_COM_Tick(BinaryContext *ctx)
{
    /* Advance Fragment RX timeout handling */
    frag_rx_tick(&ctx->frag_rx);

    /* Advance Fragment TX timeout handling */
    frag_tx_tick(&ctx->frag_tx);

    /* Flush one-slot queued response when TX session becomes available. */
    TryFlushPendingResponse(ctx);
}

void BIN_COM_SetDestAddress(BinaryContext *ctx, uint64_t addr64)
{
    ctx->current_source_addr = addr64;
}

uint16_t BIN_COM_Send(BinaryContext *ctx, const uint8_t *data, uint32_t len,
                      uint64_t dest_addr64)
{
    return frag_tx_send(&ctx->frag_tx, data, len, dest_addr64);
}

bool BIN_COM_IsTxBusy(BinaryContext *ctx)
{
    return frag_tx_is_busy(&ctx->frag_tx);
}
