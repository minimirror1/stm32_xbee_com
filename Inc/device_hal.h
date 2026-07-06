/*
 * device_hal.h
 *
 *  Created on: Dec 31, 2025
 *      Author: AI Assistant
 *
 *  Application Hardware Abstraction Layer
 *  - Defines interface between binary communication library and application implementation
 *  - All App_* functions are __weak stubs that developers override
 *
 *  ============================================================================
 *  DESIGN PRINCIPLE:
 *  ============================================================================
 *  - App_* functions handle PURE APPLICATION LOGIC only
 *  - NO communication parsing or response formatting in App_* functions
 *  - Communication layer (binary_com.c) handles packet parsing/response building
 *  ============================================================================
 */

#ifndef INC_DEVICE_HAL_H_
#define INC_DEVICE_HAL_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*******************************************************************************
 * Constants
 ******************************************************************************/
#define APP_NAME_MAX_LEN      64
#define APP_PATH_MAX_LEN      128
#define APP_CONTENT_MAX_LEN   512
#define APP_MAX_FILES         64    /* Maximum files returned by App_GetFiles */
#define APP_MAX_DEPTH         10    /* Maximum folder depth (0=root, 1, 2, ... 9) */
#define APP_MAX_MOTORS        32    /* Maximum motors */
#define APP_OPERATE_TIME_PAYLOAD_SIZE 43u

/*******************************************************************************
 * Motor Enums (Binary Protocol v1.0)
 ******************************************************************************/

/** @brief Motor type enum (wire value: uint8) */
typedef enum {
    APP_MOTOR_TYPE_NULL = 0x00,
    APP_MOTOR_TYPE_RC   = 0x01,
    APP_MOTOR_TYPE_AC   = 0x02,
    APP_MOTOR_TYPE_BL   = 0x03,
    APP_MOTOR_TYPE_ZER  = 0x04,
    APP_MOTOR_TYPE_DXL  = 0x05,
    APP_MOTOR_TYPE_AC2  = 0x06
} AppMotorType;

/** @brief Motor status enum (wire value: uint8) */
typedef enum {
    APP_MOTOR_STATUS_NORMAL       = 0x00,
    APP_MOTOR_STATUS_ERROR        = 0x01,
    APP_MOTOR_STATUS_OVERLOAD     = 0x02,
    APP_MOTOR_STATUS_DISCONNECTED = 0x03
} AppMotorStatus;

/** @brief Device status enum for CMD_PONG payload (wire value: uint8) */
typedef enum {
    APP_PING_STATE_STOPPED      = 0x00,
    APP_PING_STATE_PLAYING      = 0x01,
    APP_PING_STATE_INIT_BUSY    = 0x02,
    APP_PING_STATE_INIT_DONE    = 0x03,
    APP_PING_STATE_ERROR        = 0x04
} AppPingState;

/*******************************************************************************
 * Data Structures (Pure application data - no communication formatting)
 ******************************************************************************/

/**
 * @brief File/folder information for file system operations
 * 
 * Tree structure is represented by depth and parent_index:
 * - depth: 0 = root level, 1 = first subfolder, 2 = second subfolder (max APP_MAX_DEPTH-1)
 * - parent_index: index of parent folder in the array (-1 for root items)
 * 
 * Items MUST be ordered: parent folders must appear before their children.
 * 
 * Example for structure:
 *   Error/
 *     err_lv.ini
 *   Log/
 *     BOOT.TXT
 * 
 * Array representation:
 *   [0] name="Error",      depth=0, parent_index=-1, is_directory=true
 *   [1] name="err_lv.ini", depth=1, parent_index=0,  is_directory=false
 *   [2] name="Log",        depth=0, parent_index=-1, is_directory=true
 *   [3] name="BOOT.TXT",   depth=1, parent_index=2,  is_directory=false
 */
typedef struct {
    char name[APP_NAME_MAX_LEN];      /* File/folder name */
    char path[APP_PATH_MAX_LEN];      /* Full path */
    bool is_directory;                 /* true if directory, false if file */
    uint32_t size;                     /* File size in bytes (0 for directories) */
    uint8_t depth;                     /* Folder depth: 0=root, 1=subfolder, 2=sub-subfolder */
    int16_t parent_index;              /* Index of parent folder (-1 for root items) */
} AppFileInfo;

/**
 * @brief Full motor information for get_motors command
 * 
 * Contains complete motor configuration and current state.
 * Used for initial motor list loading in GUI.
 */
typedef struct {
    uint8_t id;                           /* Motor unique ID */
    uint8_t group_id;                     /* Group ID (for DisplayId = GroupId-SubId) */
    uint8_t sub_id;                       /* Sub ID within group */
    AppMotorType   type;                  /* Motor type (NULL/RC/AC/BL/ZER/DXL/AC2) */
    AppMotorStatus status;                /* Motor status (Normal/Error/Overload/Disconnected) */
    int32_t position;                     /* Current raw position value */
    float velocity;                       /* Current velocity */
    float min_angle;                      /* Minimum display angle */
    float max_angle;                      /* Maximum display angle */
    int32_t min_raw;                      /* Minimum raw position value */
    int32_t max_raw;                      /* Maximum raw position value */
} AppMotorInfo;

/**
 * @brief Motor state for get_motor_state command (polling)
 * 
 * Contains only runtime state (no configuration).
 * Used for periodic state updates in GUI.
 */
typedef struct {
    uint8_t id;                           /* Motor unique ID */
    AppMotorStatus status;                /* Motor status (Normal/Error/Overload/Disconnected) */
    int32_t position;                     /* Current raw position value */
    float velocity;                       /* Current velocity */
} AppMotorState;

/**
 * @brief Status snapshot for CMD_PONG payload
 *
 * Wire format:
 *   state(1) | init_state(1) | current_ms(4 LE) | total_ms(4 LE) |
 *   power_status(1) | error_status(1)
 */
typedef struct {
    AppPingState state;                   /* Device state for PONG payload */
    uint8_t init_state;                   /* Initialization stage code */
    uint32_t current_ms;                  /* Current motion time in milliseconds */
    uint32_t total_ms;                    /* Total motion time in milliseconds */
    uint8_t power_status;                 /* 0x01 = ON, 0x00 = OFF */
    uint8_t error_status;                 /* 0x01 = error present, 0x00 = normal */
} AppPingStatus;

/**
 * @brief Parsed operate-time row for one day.
 *
 * The wire payload remains the canonical storage/GET response format. This
 * struct is for application-side logic after explicit little-endian parsing.
 */
typedef struct {
    uint8_t day_of_week;                  /* 1=MON ... 7=SUN */
    uint16_t open_minutes;                /* Minutes since 00:00 */
    uint16_t close_minutes;               /* Minutes since 00:00; 0/0 means closed */
} AppOperateTimeRow;

/**
 * @brief Parsed operate-time schedule.
 */
typedef struct {
    uint8_t format_version;               /* Must be 1 */
    int16_t timezone_offset_min;          /* Local offset from UTC in minutes */
    uint32_t schedule_checksum;           /* Host-provided checksum */
    uint8_t day_count;                    /* Must be 7 */
    AppOperateTimeRow rows[7];            /* Monday through Sunday rows */
} AppOperateTimeSchedule;

/**
 * @brief Host local date/time delivered in CMD_PING payload format 1
 *
 * Payload layout:
 *   time_fmt(1) | country_code[2] | year(2 LE) | month(1) | day(1) |
 *   hour(1) | minute(1) | second(1) | utc_offset_min(2 LE)
 *
 * The app layer receives only the decoded values below.
 */
typedef struct {
    char country_code[3];                 /* 2-char ISO-like code, null-terminated */
    uint16_t year;                        /* Local year */
    uint8_t month;                        /* 1..12 */
    uint8_t day;                          /* 1..31 */
    uint8_t hour;                         /* 0..23 */
    uint8_t minute;                       /* 0..59 */
    uint8_t second;                       /* 0..59 */
    int16_t utc_offset_min;               /* Local offset from UTC in minutes */
} AppHostDateTime;

/*******************************************************************************
 * Application Function Declarations (__weak stubs)
 *
 * Return values:
 *   - bool functions: true = success, false = failure/not implemented
 *   - int functions:  >= 0 = success (count), -1 = failure/not implemented
 ******************************************************************************/

/**
 * @brief Ping check - verify device is responsive
 * @return true if device is ready, false otherwise
 *
 * @example
 *   bool App_Ping(void) {
 *       return true;  // Device is alive
 *   }
 */
bool App_Ping(void);

/**
 * @brief Apply host local date/time received via CMD_PING payload
 * @param host_time Decoded local date/time snapshot from the PC
 * @return true on success, false on validation/apply failure
 *
 * @example
 *   bool App_SetHostDateTime(const AppHostDateTime *host_time) {
 *       RTC_SetLocalDateTime(host_time);
 *       return true;
 *   }
 */
bool App_SetHostDateTime(const AppHostDateTime *host_time);

/**
 * @brief Get current status snapshot for CMD_PONG payload
 * @param out_status Output status snapshot
 * @return true on success, false on failure
 *
 * @example
 *   bool App_GetPingStatus(AppPingStatus *out_status) {
 *       out_status->state = APP_PING_STATE_STOPPED;
 *       out_status->init_state = 0;
 *       out_status->current_ms = 0;
 *       out_status->total_ms = 0;
 *       out_status->power_status = 0;
 *       return true;
 *   }
 */
bool App_GetPingStatus(AppPingStatus *out_status);

/**
 * @brief Execute move command
 * @param motor_id Target motor ID
 * @param raw_pos Target raw position value
 * @return true on success, false on failure
 *
 * @example
 *   bool App_Move(uint8_t motor_id, int32_t raw_pos) {
 *       Motor_MoveToRawPosition(motor_id, raw_pos);
 *       return true;
 *   }
 */
bool App_Move(uint8_t motor_id, int32_t raw_pos);

/**
 * @brief Play motion sequence
 * @param device_id Target device ID
 * @return true on success, false on failure
 *
 * @example
 *   bool App_MotionPlay(uint8_t device_id) {
 *       Motion_Start(device_id);
 *       return true;
 *   }
 */
bool App_MotionPlay(uint8_t device_id);

/**
 * @brief Play motion sequence repeatedly from the beginning after it ends
 * @param device_id Target device ID
 * @return true on success, false on failure
 */
bool App_MotionRepeatPlay(uint8_t device_id);

/**
 * @brief Stop motion sequence
 * @param device_id Target device ID
 * @return true on success, false on failure
 */
bool App_MotionStop(uint8_t device_id);

/**
 * @brief Pause motion sequence
 * @param device_id Target device ID
 * @return true on success, false on failure
 */
bool App_MotionPause(uint8_t device_id);

/**
 * @brief Seek to position in motion sequence
 * @param device_id Target device ID
 * @param time_ms Time offset in milliseconds
 * @return true on success, false on failure
 */
bool App_MotionSeek(uint8_t device_id, uint32_t time_ms);

/**
 * @brief Control relay-backed device power output
 * @param action Power action: 0x00=OFF, 0x01=ON, 0x02=REBOOT
 * @return true on accepted/executed action, false on invalid or failed action
 */
bool App_PowerControl(uint8_t action);

/**
 * @brief Clear current device error latch/status when possible
 * @return true on success, false on failure
 */
bool App_ErrorClear(void);

/**
 * @brief Get file/folder list from storage
 * @param out_files Output array of file info structures
 * @param max_count Maximum number of files to return
 * @return Number of files (>= 0) on success, -1 on failure
 *
 * @example
 *   int App_GetFiles(AppFileInfo *out_files, uint16_t max_count) {
 *       int count = 0;
 *       // Read SD card directory...
 *       strcpy(out_files[count].name, "config.txt");
 *       strcpy(out_files[count].path, "Setting/config.txt");
 *       out_files[count].is_directory = false;
 *       out_files[count].size = 128;
 *       count++;
 *       return count;
 *   }
 */
int App_GetFiles(AppFileInfo *out_files, uint16_t max_count);

/**
 * @brief Read file content
 * @param path File path to read
 * @param out_content Output buffer for file content
 * @param max_len Maximum buffer size
 * @return true on success, false on failure
 *
 * @example
 *   bool App_GetFile(const char *path, char *out_content, uint16_t max_len) {
 *       return SD_ReadFile(path, out_content, max_len) == 0;
 *   }
 */
bool App_GetFile(const char *path, char *out_content, uint16_t max_len);

/**
 * @brief Save content to file
 * @param path File path to write
 * @param content Content to write
 * @return true on success, false on failure
 *
 * @example
 *   bool App_SaveFile(const char *path, const char *content) {
 *       return SD_WriteFile(path, content) == 0;
 *   }
 */
bool App_SaveFile(const char *path, const char *content);

/**
 * @brief Verify file content matches expected content
 * @param path File path to verify
 * @param content Expected content
 * @param out_match Output: true if content matches, false otherwise
 * @return true on success (verification performed), false on failure (couldn't read file)
 *
 * @example
 *   bool App_VerifyFile(const char *path, const char *content, bool *out_match) {
 *       char buffer[512];
 *       if (SD_ReadFile(path, buffer, sizeof(buffer)) != 0) {
 *           return false;  // Couldn't read file
 *       }
 *       *out_match = (strcmp(buffer, content) == 0);
 *       return true;
 *   }
 */
bool App_VerifyFile(const char *path, const char *content, bool *out_match);

/**
 * @brief Store the validated operate-time payload exactly as received.
 * @param payload 43-byte payload from CMD_SET_OPERATE_TIME
 * @param payload_len Must be APP_OPERATE_TIME_PAYLOAD_SIZE
 * @return true on success, false on storage failure
 */
bool App_SetOperateTime(const uint8_t *payload, uint16_t payload_len);

/**
 * @brief Read the last successfully stored operate-time payload.
 * @param out_payload Output buffer
 * @param max_len Output buffer size
 * @param out_len Number of bytes written on success
 * @return true when a stored schedule exists and was copied, false otherwise
 */
bool App_GetOperateTime(uint8_t *out_payload, uint16_t max_len, uint16_t *out_len);

/**
 * @brief Get list of all motors with full information
 * @param out_motors Output array of motor info structures
 * @param max_count Maximum number of motors to return
 * @return Number of motors (>= 0) on success, -1 on failure
 *
 * @example
 *   int App_GetMotors(AppMotorInfo *out_motors, uint16_t max_count) {
 *       int idx = 0;
 *       if (idx < max_count) {
 *           out_motors[idx].id = 1;
 *           out_motors[idx].group_id = 1;
 *           out_motors[idx].sub_id = 1;
 *           out_motors[idx].type = APP_MOTOR_TYPE_RC;
 *           out_motors[idx].status = APP_MOTOR_STATUS_NORMAL;
 *           out_motors[idx].position = 2048;
 *           out_motors[idx].velocity = 0.5f;
 *           out_motors[idx].min_angle = 0.0f;
 *           out_motors[idx].max_angle = 180.0f;
 *           out_motors[idx].min_raw = 0;
 *           out_motors[idx].max_raw = 3072;
 *           idx++;
 *       }
 *       return idx;
 *   }
 */
int App_GetMotors(AppMotorInfo *out_motors, uint16_t max_count);

/**
 * @brief Get current state of all motors (for polling)
 * @param out_states Output array of motor state structures
 * @param max_count Maximum number of motors to return
 * @return Number of motors (>= 0) on success, -1 on failure
 *
 * @note This function is called periodically for state updates.
 *       Only id, status, raw position, and velocity are returned.
 *
 * @example
 *   int App_GetMotorState(AppMotorState *out_states, uint16_t max_count) {
 *       int idx = 0;
 *       if (idx < max_count) {
 *           out_states[idx].id = 1;
 *           out_states[idx].status = Motor_HasError(1) ? APP_MOTOR_STATUS_ERROR : APP_MOTOR_STATUS_NORMAL;
 *           out_states[idx].position = Motor_GetRawPosition(1);
 *           out_states[idx].velocity = Motor_GetVelocity(1);
 *           idx++;
 *       }
 *       return idx;
 *   }
 */
int App_GetMotorState(AppMotorState *out_states, uint16_t max_count);

#endif /* INC_DEVICE_HAL_H_ */
