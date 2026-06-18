/*
 * binary_com.h
 *
 *  Created on: 2026
 *      Author: AI Assistant
 *
 *  Binary Communication Library (Binary Protocol v1.0)
 *  Uses the existing Fragment Protocol / XBee API stack with binary payload.
 *
 *  Request Header  (5 bytes, Little-Endian):
 *    src_id(1) | tar_id(1) | cmd(1) | payload_len(2 LE)
 *
 *  Response Header (6 bytes, Little-Endian):
 *    src_id(1) | tar_id(1) | cmd(1) | status(1) | payload_len(2 LE)
 */

#ifndef INC_BINARY_COM_H_
#define INC_BINARY_COM_H_

#include "uart_queue.h"
#include "xbee_api.h"
#include "fragment_rx.h"
#include "fragment_tx.h"
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Protocol Constants
 * ============================================================================ */

/** @brief Protocol broadcast target ID ??received by all devices */
#define BINARY_BROADCAST_ID  0xFF

/** @brief PC / host source ID */
#define BINARY_PC_ID         0x00

/** @brief Request header size in bytes */
#define BIN_REQ_HEADER_SIZE   5

/** @brief Response header size in bytes */
#define BIN_RESP_HEADER_SIZE  6

/**
 * @brief TX buffer size matches the Fragment Protocol message ceiling.
 *        Responses above 4096 B must be rejected with CMD_ERROR.
 *        GET_FILES now returns ERR_RESPONSE_TOO_LARGE instead of truncating.
 */
#define BIN_TX_BUFFER_SIZE   4096

/* ============================================================================
 * Command Enum  (cmd field, uint8)
 * ============================================================================ */

typedef enum {
    CMD_PING            = 0x01,
    CMD_PONG            = 0x02,
    CMD_MOVE            = 0x03,
    CMD_MOTION_CTRL     = 0x04,
    CMD_POWER_CTRL      = 0x05,
    CMD_GET_MOTORS      = 0x10,
    CMD_GET_MOTOR_STATE = 0x11,
    CMD_GET_FILES       = 0x20,
    CMD_GET_FILE        = 0x21,
    CMD_SAVE_FILE       = 0x22,
    CMD_VERIFY_FILE     = 0x23,
    CMD_SET_OPERATE_TIME = 0x30,
    CMD_GET_OPERATE_TIME = 0x31,
    CMD_ERROR           = 0xFF
} BinCmd;

/* ============================================================================
 * Response Status Enum  (status field, uint8)
 * ============================================================================ */

typedef enum {
    BIN_STATUS_OK    = 0x00,
    BIN_STATUS_ERROR = 0x01
} BinStatus;

/* ============================================================================
 * MotionAction Enum  (action field in CMD_MOTION_CTRL payload, uint8)
 * ============================================================================ */

typedef enum {
    MOTION_ACTION_PLAY        = 0x00,
    MOTION_ACTION_STOP        = 0x01,
    MOTION_ACTION_PAUSE       = 0x02,
    MOTION_ACTION_SEEK        = 0x03,
    MOTION_ACTION_REPEAT_PLAY = 0x04
} MotionAction;

/* ============================================================================
 * PowerAction Enum  (action field in CMD_POWER_CTRL payload, uint8)
 * ============================================================================ */

typedef enum {
    POWER_ACTION_OFF    = 0x00,
    POWER_ACTION_ON     = 0x01,
    POWER_ACTION_REBOOT = 0x02
} PowerAction;

/* ============================================================================
 * Error Code Enum  (error_code field in CMD_ERROR payload, uint8)
 * ============================================================================ */

typedef enum {
    ERR_UNKNOWN         = 0x00,
    ERR_INVALID_INPUT   = 0x01,
    ERR_UNKNOWN_CMD     = 0x02,
    ERR_INVALID_PARAM   = 0x03,
    ERR_FILE_NOT_FOUND  = 0x04,
    ERR_MOTOR_NOT_FOUND = 0x05,
    ERR_RESPONSE_TOO_LARGE = 0x06,
    ERR_TX_BUSY         = 0x07
} BinErrorCode;

/* ============================================================================
 * Wire Header Structs (packed, Little-Endian multi-byte fields)
 * NOTE: Always use read_u16le() / write_u16le() for payload_len ??do NOT
 *       rely on the struct layout for multi-byte fields on big-endian hosts.
 * ============================================================================ */

#pragma pack(push, 1)

/** @brief Request header ??5 bytes */
typedef struct {
    uint8_t  src_id;
    uint8_t  tar_id;
    uint8_t  cmd;
    uint16_t payload_len;   /* Little-Endian */
} BinReqHeader;

/** @brief Response header ??6 bytes */
typedef struct {
    uint8_t  src_id;
    uint8_t  tar_id;
    uint8_t  cmd;
    uint8_t  status;
    uint16_t payload_len;   /* Little-Endian */
} BinRespHeader;

#pragma pack(pop)

/* ============================================================================
 * Context
 * ============================================================================ */

typedef struct {
    /* UART context (dependency) */
    UART_Context *uart;

    /* Device ID */
    uint8_t my_id;

    /* XBee context for RF communication */
    XBeeContext_t xbee;

    /* Fragment Protocol contexts */
    FragRxContext_t frag_rx;
    FragTxContext_t frag_tx;

    /* Source address of most recently received message ??used for replies */
    uint64_t current_source_addr;

    /* TX buffer for building responses */
    uint8_t  tx_buffer[BIN_TX_BUFFER_SIZE];
    uint32_t tx_buffer_len;

    /* One-slot pending response queue for TX-busy backpressure */
    uint8_t  pending_tx_buffer[BIN_TX_BUFFER_SIZE];
    uint32_t pending_tx_len;
    uint64_t pending_tx_dest_addr;
    bool     pending_tx_valid;

    /* Number of responses dropped because TX was busy */
    uint32_t tx_busy_drop_count;
} BinaryContext;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * @brief Initialize binary communication
 * @param ctx   Pointer to BinaryContext
 * @param uart  Pointer to UART context (connected to XBee)
 * @param my_id Device ID for addressing
 */
void BIN_COM_Init(BinaryContext *ctx, UART_Context *uart, uint8_t my_id);

/**
 * @brief Process incoming data ??call from main loop.
 *
 * 1. Reads bytes from UART and feeds to XBee parser.
 * 2. XBee frames are forwarded to Fragment Protocol receiver.
 * 3. Reassembled binary messages are parsed and commands executed.
 * 4. Responses are sent back via Fragment Protocol.
 */
void BIN_COM_Process(BinaryContext *ctx);

/**
 * @brief Periodic tick for Fragment Protocol timeout handling.
 *        Call every ~100 ms from main loop or timer.
 */
void BIN_COM_Tick(BinaryContext *ctx);

/**
 * @brief Override the default destination XBee address for outgoing messages.
 * @param ctx    Pointer to BinaryContext
 * @param addr64 64-bit XBee address
 */
void BIN_COM_SetDestAddress(BinaryContext *ctx, uint64_t addr64);

/**
 * @brief Send raw binary data via Fragment Protocol.
 * @param ctx        Pointer to BinaryContext
 * @param data       Data buffer to send
 * @param len        Number of bytes to send
 * @param dest_addr64 Destination 64-bit XBee address
 * @return Message ID on success, 0 on failure
 */
uint16_t BIN_COM_Send(BinaryContext *ctx, const uint8_t *data, uint32_t len,
                      uint64_t dest_addr64);

/**
 * @brief Check whether there is an active outgoing transmission.
 * @return true if TX is busy
 */
bool BIN_COM_IsTxBusy(BinaryContext *ctx);

#endif /* INC_BINARY_COM_H_ */
