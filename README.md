# STM32 Binary Communication Library

# >버전 : v1.1.10.0 - 2026-05-20

XBee DigiMesh 기반 바이너리 통신 라이브러리입니다.  
Fragment Protocol을 사용해 대용량 메시지를 분할 전송합니다.

## 지원 MCU


| 시리즈     | 정의 매크로    | 상태        |
| ------- | --------- | --------- |
| STM32F3 | `STM32F3` | Supported |
| STM32F4 | `STM32F4` | Supported |
| STM32F7 | `STM32F7` | Supported |
| STM32H7 | `STM32H7` | Supported |
| STM32G4 | `STM32G4` | Supported |
| STM32L4 | `STM32L4` | Supported |


## 폴더 구조

```text
stm32_xbee_com/
├── Inc/
│   ├── binary_com.h         # Binary 통신 메인 API
│   ├── uart_queue.h         # UART 큐 드라이버
│   ├── xbee_api.h           # XBee API Mode 2 파서
│   ├── fragment_protocol.h  # Fragment Protocol 정의
│   ├── fragment_rx.h        # Fragment 수신기
│   ├── fragment_tx.h        # Fragment 송신기
│   ├── device_hal.h         # App_* 함수 인터페이스
│   └── crc16.h              # CRC-16 계산
└── Src/
    ├── binary_com.c
    ├── uart_queue.c
    ├── xbee_api.c
    ├── fragment_rx.c
    ├── fragment_tx.c
    └── crc16.c
```

## 빌드 설정

### STM32CubeIDE

1. Include Paths 추가

- Project -> Properties -> C/C++ Build -> Settings
- MCU GCC Compiler -> Include paths
- 추가: `../Lib/stm32_xbee_com/Inc`

1. Source Folders 추가

- Project -> Properties -> C/C++ General -> Paths and Symbols
- Source Location -> Add Folder
- 추가: `Lib/stm32_xbee_com/Src`

1. MCU 시리즈 정의 확인

- Project -> Properties -> C/C++ Build -> Settings
- MCU GCC Compiler -> Preprocessor
- `STM32F7` 또는 해당 시리즈 매크로 정의 확인

### Makefile

```makefile
C_INCLUDES += -ILib/stm32_xbee_com/Inc

C_SOURCES += \
Lib/stm32_xbee_com/Src/binary_com.c \
Lib/stm32_xbee_com/Src/uart_queue.c \
Lib/stm32_xbee_com/Src/xbee_api.c \
Lib/stm32_xbee_com/Src/fragment_rx.c \
Lib/stm32_xbee_com/Src/fragment_tx.c \
Lib/stm32_xbee_com/Src/crc16.c

C_DEFS += -DSTM32F7
```

## 사용법

```c
#include "uart_queue.h"
#include "binary_com.h"

UART_Context uart_ctx;
BinaryContext bin_ctx;

int main(void) {
    UART_Queue_Init(&uart_ctx, &huart1);
    BIN_COM_Init(&bin_ctx, &uart_ctx, 1);

    while (1) {
        BIN_COM_Process(&bin_ctx);

        if (HAL_GetTick() - last_tick >= 100) {
            BIN_COM_Tick(&bin_ctx);
            last_tick = HAL_GetTick();
        }
    }
}
```

## 프로토콜 요약

- Request Header (5B): `src_id(1) | tar_id(1) | cmd(1) | payload_len(2 LE)`
- Response Header (6B): `src_id(1) | tar_id(1) | cmd(1) | status(1) | payload_len(2 LE)`
- Payload는 `binary_com.h` 및 `motion_recorder_packet_categories.html` 기준으로 해석

## Git Submodule

```bash
git submodule add <repository_url> Lib/stm32_xbee_com
git clone --recursive <project_url>
git submodule update --init --recursive
```

