# System Information과 System Control

두 장치는 운영체제와 펌웨어가 host 실행 인자를 하드코딩하지 않고 플랫폼 구성을 읽고 전원 상태를 요청할 수 있게 하는 고정 MMIO 장치다. 모든 레지스터는 64비트이며 8바이트 정렬 `LOAD64`/`STORE64`로만 접근한다. 두 영역에서 명령어 fetch는 허용되지 않는다.

## System Information

물리 주소 범위는 `0xFFFFFFFFFFFC1000`~`0xFFFFFFFFFFFC10FF`다. 전체 영역은 읽기 전용이며 쓰기는 `DATA_ACCESS` 예외가 된다.

| 오프셋 | 이름 | 값 |
|---:|---|---|
| `0x00` | `MAGIC` | `0x464E4953` (`SINF`) |
| `0x08` | `VERSION` | 장치 규격 version, 현재 1 |
| `0x10` | `FEATURES` | 플랫폼 기능 비트 |
| `0x18` | `RAM_BASE` | 현재 0 |
| `0x20` | `RAM_SIZE` | `-r/--ram` 입력을 b/KiB/MiB/GiB 단위에서 환산한 byte 수 |
| `0x28` | `ROM_BASE` | Boot ROM이 없으면 0 |
| `0x30` | `ROM_SIZE` | 실제 매핑된 ROM byte 수 |
| `0x38` | `RESET_VECTOR` | power-on 및 warm reset 진입 주소 |
| `0x40` | `CORE_COUNT` | 구성된 코어 수 |
| `0x48` | `THREADS_PER_CORE` | 코어당 하드웨어 스레드 수 |
| `0x50` | `LOGICAL_PROCESSORS` | 두 값의 곱 |
| `0x58` | `PHYSICAL_ADDRESS_BITS` | 현재 64 |
| `0x60` | `PAGE_SIZE` | 현재 4096 |
| `0x68` | `VIRTUAL_ADDRESS_BITS` | 현재 39 |
| `0x70` | `TIMER_FREQUENCY` | timer tick/초, 현재 1,000,000,000 |
| `0x78` | `INTERRUPT_LINES` | 현재 64 |
| `0x80` | `VIO_HUB_BASE` | VIO Hub 물리주소 |
| `0x88` | `VIO_SLOT_COUNT` | 현재 16 |
| `0x90` | `DYNAMIC_MMIO_BASE` | 외부 BAR 자동 할당 시작 주소 |
| `0x98` | `DYNAMIC_MMIO_SIZE` | 자동 할당 영역 byte 수 |
| `0xA0` | `SYSTEM_CONTROL_BASE` | System Control 물리주소 |
| `0xA8` | `ISA_VERSION` | 현재 1 |
| `0xB0` | `EXTERNAL_IRQ_COUNT` | 외부 장치 IRQ 수, 현재 48 |
| `0xB8` | `IPI_BASE` | IPI 전용 IRQ 시작, 현재 48 |
| `0xC0` | `IRQ_CONTROLLER_BASE` | IRQ Controller 물리주소 |

`FEATURES`는 bit 0부터 MMU, multicore, interrupt, atomic, SIMD, floating point, Boot ROM 존재, VIO, System Control, programmable IRQ Controller를 나타낸다. Boot ROM 비트만 현재 실행 구성에 따라 달라지고 나머지는 VM 구현 능력을 나타낸다.

## System Control

물리 주소 범위는 `0xFFFFFFFFFFFC2000`~`0xFFFFFFFFFFFC203F`다.

| 오프셋 | 이름 | 접근 | 의미 |
|---:|---|---|---|
| `0x00` | `MAGIC` | R | `0x4C525443` (`CTRL`) |
| `0x08` | `VERSION` | R | 현재 1 |
| `0x10` | `FEATURES` | R | bit 0 shutdown, bit 1 warm reset |
| `0x18` | `COMMAND` | R/W | 마지막 명령 읽기 또는 새 명령 실행 |
| `0x20` | `STATUS` | R | 0 running, 1 shutdown pending, 2 reset pending |
| `0x28` | `RESULT` | R | 0 none, 1 accepted, 2 busy, 3 invalid command |
| `0x30` | `RESET_CAUSE` | R | 0 power-on, 1 software warm reset |
| `0x38` | `RESET_COUNT` | R | 완료된 warm reset 횟수 |

`COMMAND` 값은 `0=NONE`, `1=SHUTDOWN`, `2=WARM_RESET/REBOOT`다. 유효한 요청을 접수한 STORE 자체는 성공하며 실제 결과는 `RESULT`에서 확인한다. 이미 시스템 동작이 pending이면 새 요청은 `BUSY`가 된다.

warm reset은 현재 명령을 끝낸 뒤 모든 VM worker를 정지하고 다음 상태로 전환한다.

1. timer, UART, Core Control 및 외부 장치의 reset callback을 실행한다.
2. 모든 CPU 레지스터, MMU 상태, 예외 상태와 pending interrupt를 초기화한다.
3. boot hardware thread만 RUNNABLE로 만들고 `RESET_VECTOR`에서 다시 시작한다.
4. 일반 RAM, 적재된 프로그램, Boot ROM 매핑과 MMIO 주소 배치는 보존한다.

따라서 이것은 RAM을 지우는 cold boot가 아니라 실제 플랫폼의 warm reset에 해당한다. `SHUTDOWN`은 VM worker를 정상 종료하고 `main`을 성공 상태로 끝낸다.
