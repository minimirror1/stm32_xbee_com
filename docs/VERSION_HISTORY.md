# STM32 Binary Communication Library 버전 이력

이 문서는 `stm32_xbee_com` 라이브러리의 버전별 변경 사항, 프로토콜 변경,
App 레이어 계약, 구현 위치, 검증 방법을 누적 기록합니다.

작성 원칙:

- 통신 레이어(`binary_com.c`)와 App 레이어(`device_real.c`, `device_mock.c`)의
  책임을 분리해서 기록합니다.
- wire format은 `binary_com.h`와 실제 직렬화 코드 기준으로 기록합니다.
- 멀티바이트 값은 little-endian 여부를 명확히 적습니다.
- mock/weak 구현을 함께 갱신해야 하는 경우 반드시 구현 위치에 남깁니다.

## SW v1.1.10.0 - 2026-05-20

### 요약

binary serial protocol에 운영시간 schedule 저장/조회 명령을 추가했습니다.

Host UI는 `CMD_SET_OPERATE_TIME`으로 7일 전체 schedule을 저장하고,
`CMD_GET_OPERATE_TIME`으로 저장된 schedule을 다시 읽어 같은
`schedule_checksum`으로 검증합니다.

### Protocol

- `CMD_SET_OPERATE_TIME = 0x30`
- `CMD_GET_OPERATE_TIME = 0x31`

#### SET_OPERATE_TIME Request Payload

Payload 길이는 고정 43 bytes입니다.

```text
format_version(1)
timezone_offset_min(2 LE, int16)
schedule_checksum(4 LE, uint32)
day_count(1)
rows[7]
```

각 row는 5 bytes입니다.

```text
day_of_week(1) | open_minutes(2 LE) | close_minutes(2 LE)
```

Field 규칙:

- `format_version`은 반드시 `1`이어야 합니다.
- `day_count`는 반드시 `7`이어야 합니다.
- `day_of_week`는 반드시 `1..7` 범위여야 합니다.
- `open_minutes == 0 && close_minutes == 0`이면 휴무로 처리합니다.
- `is_closed` field는 없습니다.
- `00:00 ~ 00:00`은 24시간 영업이 아니라 휴무입니다.

#### SET_OPERATE_TIME Success Response

Payload 길이는 4 bytes입니다.

```text
schedule_checksum(4 LE)
```

장치는 request offset `3..6`에 들어 있던 저장 schedule checksum을 그대로
echo합니다.

#### GET_OPERATE_TIME Request Payload

Payload 길이는 반드시 0이어야 합니다.

#### GET_OPERATE_TIME Success Response

Payload 길이는 43 bytes이며 `SET_OPERATE_TIME` request payload와 같은
layout을 사용합니다. 장치는 마지막으로 정상 저장된 payload를 저장된 그대로
반환합니다.

저장된 schedule이 없으면 `CMD_ERROR`를 반환합니다.

### 검증 및 Error 처리

communication layer는 아래 조건을 거부합니다.

- SET payload 길이가 43 bytes가 아님
- `format_version != 1`
- `day_count != 7`
- `day_of_week`가 `1..7` 범위를 벗어남
- app-layer 저장 실패
- GET payload 길이가 0이 아님
- GET 시 저장된 schedule이 없음

### App Layer Contract

`device_hal.h`는 아래 contract를 제공합니다.

- `APP_OPERATE_TIME_PAYLOAD_SIZE = 43`
- `AppOperateTimeRow`
- `AppOperateTimeSchedule`
- `App_SetOperateTime(const uint8_t *payload, uint16_t payload_len)`
- `App_GetOperateTime(uint8_t *out_payload, uint16_t max_len, uint16_t *out_len)`

raw 43-byte payload는 storage와 GET response의 기준 format으로 유지합니다.
파싱된 `AppOperateTimeSchedule` struct는 명시적인 little-endian parsing 이후
application-side logic에서 사용하기 위한 구조입니다.

### 구현 Notes

- `Src/binary_com.c`는 wire payload를 검증하고 SET/GET dispatch를 처리합니다.
- 현재 mock 동작은 마지막 정상 43-byte payload를 RAM에 저장합니다.
- mock app은 payload를 `AppOperateTimeSchedule`로도 파싱합니다.
- real-device weak stub은 application에서 SD 또는 비휘발 저장소 구현을 붙이기
  전까지 failure를 반환합니다.

### 변경 파일

| File | Change |
|---|---|
| `Inc/binary_com.h` | operate-time command ID 추가 |
| `Inc/device_hal.h` | payload size, parsed schedule struct, app storage API 추가 |
| `Src/binary_com.c` | validation, SET response checksum echo, GET response payload 추가 |

## SW v1.1.9.0 - 2026-05-15

### Summary

Added `CMD_POWER_CTRL` for relay-backed device power control.

### Protocol

- Command: `CMD_POWER_CTRL = 0x05`
- Request payload length: 1 byte
- Request payload values:
  - `0x00`: OFF
  - `0x01`: ON
  - `0x02`: REBOOT
- Success response payload: `[action][accepted]`
  - `action`: requested action value
  - `accepted`: `0x01` accepted, `0x00` not accepted

Examples:

```text
ON     00 02 05 01 00 01
OFF    00 02 05 01 00 00
REBOOT 00 02 05 01 00 02
```

### Device Implementation Notes

Device-side application code must implement `App_PowerControl(uint8_t action)` and map it to the relay GPIO:

- OFF: call `power_output_off()`
- ON: call `power_output_on()`
- REBOOT: call `power_output_off()`, wait an internal delay, then call `power_output_on()`
- Keep the current power state as `0x00 = OFF` or `0x01 = ON`
- Return that current power state through `App_GetPingStatus().power_status` so the next `CMD_PONG` reflects it

### Changed Files

| File | Change |
|---|---|
| `Inc/binary_com.h` | Added `CMD_POWER_CTRL` and `PowerAction` values |
| `Inc/device_hal.h` | Added `App_PowerControl(uint8_t action)` contract |
| `Src/binary_com.c` | Added payload validation, command dispatch, and `[action][accepted]` response |

## SW v1.1.8.0 - 2026-05-13

### Summary

Added `CMD_POWER_CTRL` for relay-backed device power control.

### Protocol

- Command: `CMD_POWER_CTRL = 0x05`
- Request payload length: 1 byte
- Request payload values:
  - `0x00`: OFF
  - `0x01`: ON
  - `0x02`: REBOOT
- Success response payload: `[action][accepted]`
  - `action`: requested action value
  - `accepted`: `0x01` accepted, `0x00` not accepted

Examples:

```text
ON     00 02 05 01 00 01
OFF    00 02 05 01 00 00
REBOOT 00 02 05 01 00 02
```

### Device Implementation Notes

Device-side application code must implement `App_PowerControl(uint8_t action)` and map it to the relay GPIO:

- OFF: call `power_output_off()`
- ON: call `power_output_on()`
- REBOOT: call `power_output_off()`, wait an internal delay, then call `power_output_on()`
- Keep the current power state as `0x00 = OFF` or `0x01 = ON`
- Return that current power state through `App_GetPingStatus().power_status` so the next `CMD_PONG` reflects it

### Changed Files

| File | Change |
|---|---|
| `Inc/binary_com.h` | Added `CMD_POWER_CTRL` and `PowerAction` values |
| `Inc/device_hal.h` | Added `App_PowerControl(uint8_t action)` contract |
| `Src/binary_com.c` | Added payload validation, command dispatch, and `[action][accepted]` response |

## v1.1.7.0 - 2026-05-11

### 요약

`CMD_PONG` 응답 payload에 장치 전원 상태인 `power_status`를 추가했습니다.

이 버전부터 Host는 PING/PONG 폴링 한 번으로 아래 상태를 함께 읽을 수 있습니다.

- motion 상태: `state`
- 초기화 상태: `init_state`
- 현재 재생 위치: `current_ms`
- 전체 재생 길이: `total_ms`
- 전원 상태: `power_status`

### 추가된 기능

- `AppPingStatus`에 `power_status` 필드를 추가했습니다.
- `CMD_PONG` 정상 응답 payload 길이를 10 bytes에서 11 bytes로 확장했습니다.
- PONG payload 마지막 바이트에 `power_status`를 직렬화합니다.
- mock 장치에서 `power_status`가 5초마다 ON/OFF 토글되도록 했습니다.

### 프로토콜 변경

#### CMD_PONG 응답 헤더

응답 헤더는 기존과 동일하게 6 bytes입니다.

```text
src_id(1) | tar_id(1) | cmd(1) | status(1) | payload_len(2 LE)
```

정상 응답 값:

```text
cmd = 0x02
status = 0x00
payload_len = 0x000B
```

#### CMD_PONG payload

payload 길이는 11 bytes입니다.

```text
state(1) | init_state(1) | current_ms(4 LE) | total_ms(4 LE) | power_status(1)
```

| Offset | 필드 | 크기 | 설명 |
|---:|---|---:|---|
| 0 | `state` | 1 | motion 상태 |
| 1 | `init_state` | 1 | 초기화 상태 또는 초기화 단계 코드 |
| 2..5 | `current_ms` | 4 | 현재 재생 위치 ms, `uint32` little-endian |
| 6..9 | `total_ms` | 4 | 전체 재생 길이 ms, `uint32` little-endian |
| 10 | `power_status` | 1 | `0x01 = ON`, `0x00 = OFF` |

`state` 값:

| 값 | 이름 | 의미 |
|---:|---|---|
| `0x00` | `APP_PING_STATE_STOPPED` | 정지 |
| `0x01` | `APP_PING_STATE_PLAYING` | 재생 중 |
| `0x02` | `APP_PING_STATE_INIT_BUSY` | 초기화 중 |
| `0x03` | `APP_PING_STATE_INIT_DONE` | 준비 완료 |
| `0x04` | `APP_PING_STATE_ERROR` | 에러 |

예시: 장치 ID 2, 정지 상태, 현재/전체 시간 0, 전원 ON.

```text
02 00 02 00 0B 00 00 00 00 00 00 00 00 00 00 00 01
```

앞 6 bytes는 응답 헤더이고, 뒤 11 bytes는 PONG payload입니다.

### App 레이어 계약

`App_GetPingStatus()` 구현체는 통신 포맷을 직접 만들지 않습니다.
App 레이어는 순수 상태 값만 채우고, `binary_com.c`가 wire format으로 직렬화합니다.

```c
bool App_GetPingStatus(AppPingStatus *out_status)
{
    if (out_status == NULL) {
        return false;
    }

    out_status->state = APP_PING_STATE_STOPPED;
    out_status->init_state = 0u;
    out_status->current_ms = 0u;
    out_status->total_ms = 0u;
    out_status->power_status = 1u;
    return true;
}
```

`power_status` 값:

- `1u`: 전원 ON
- `0u`: 전원 OFF

### 구현 위치

| 파일 | 변경 내용 |
|---|---|
| `Inc/device_hal.h` | `AppPingStatus.power_status` 추가, PONG wire format 주석 갱신 |
| `Src/binary_com.c` | `BIN_PONG_PAYLOAD_SIZE`를 `11u`로 변경, `power_status`를 마지막 바이트로 직렬화 |
| `Core/Src/device_real.c` | weak 기본 구현에서 `power_status = 0u` 반환 |
| `Core/Src/device_mock.c` | mock 초기값은 전원 ON, `HAL_GetTick()` 기준 5초마다 `power_status` 토글 |
| `motion_recorder_packet_categories.html` | PONG payload 문서를 11 bytes 기준으로 갱신 |

### 수정할 때 지켜야 할 점

- `binary_com.c`에서만 응답 헤더와 payload를 직렬화합니다.
- App 레이어의 `App_*` 함수에 통신 패킷 생성 코드를 넣지 않습니다.
- `current_ms`, `total_ms`는 반드시 little-endian helper로 직렬화합니다.
- payload 길이는 응답 헤더의 `payload_len`과 실제 payload 크기가 일치해야 합니다.
- `AppPingStatus` 필드를 바꾸면 weak 구현과 mock 구현도 함께 갱신합니다.

### 검증 방법

메인 펌웨어 저장소 루트에서 아래 스크립트를 실행합니다.

```powershell
powershell -ExecutionPolicy Bypass -File tests\pong_power_status_check.ps1
powershell -ExecutionPolicy Bypass -File tests\bin_status_enum_check.ps1
powershell -ExecutionPolicy Bypass -File tests\motor_type_enum_check.ps1
```

### 마이그레이션 노트

Host 앱은 PONG 정상 응답에서 11-byte payload를 처리해야 합니다.

기존 10-byte PONG 응답과의 호환이 필요하면, `power_status`가 없는 경우를 별도로
처리해야 합니다. 정책은 Host 앱에서 정하되, 현재 앱 요구사항 기준으로는 누락 시
OFF로 보는 것이 안전합니다.

## 새 버전 작성 템플릿

새 버전을 추가할 때는 아래 형식을 복사해서 위쪽에 누적합니다.

```md
## vX.Y.Z.W - YYYY-MM-DD

### 요약

이번 버전의 핵심 변경을 짧게 설명합니다.

### 추가된 기능

- 새로 추가된 기능 또는 프로토콜 필드.

### 변경된 기능

- 기존 동작에서 달라진 부분.

### 수정된 문제

- 버그 수정 또는 예외 처리 변경.

### 프로토콜 변경

명령, 응답, payload layout, endian, 예시 hex를 기록합니다.

### App 레이어 계약

필요한 `App_*` 구현 규칙과 예시 코드를 기록합니다.

### 구현 위치

어떤 파일을 어떤 목적으로 수정했는지 기록합니다.

### 수정할 때 지켜야 할 점

통신 레이어/App 레이어 분리, LE helper 사용, payload 길이 일치 등 주의사항을 기록합니다.

### 검증 방법

실행한 테스트 또는 빌드 명령을 기록합니다.

### 마이그레이션 노트

Host 앱, backend, firmware 간 호환성 이슈를 기록합니다.
```
