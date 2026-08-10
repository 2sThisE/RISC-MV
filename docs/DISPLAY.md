# Framebuffer display

현재 디스플레이는 guest RAM framebuffer, 외부 VIO 장치 DLL, 교체 가능한 host frontend의 세 계층으로 나뉜다.

```text
guest program -> display BAR -> display_device.dll
                              -> DMA read from guest RAM
                              -> display.present host service
                              -> headless or Win32 frontend
```

따라서 VM의 CPU는 픽셀을 직접 그리지 않는다. 게스트가 RAM에 픽셀을 쓰고 `PRESENT`를 요청하면 장치가 한 프레임을 안전하게 복사한다. 기본값은 자동 실행과 테스트에 적합한 headless frontend이며 Windows에서는 `-display window`로 실제 Win32 창을 사용할 수 있다. GPU 가속은 아직 없으며 키보드 입력은 별도의 VIO HID 장치로 분리되어 있다. 입력 규격은 `KEYBOARD.md`를 참고한다.

## 실행

```powershell
.\build.ps1
.\devices\build_display.ps1

# 마지막 프레임만 메모리에 보관
.\build\main.exe -r 300000 -l .\build\examples\display_demo.bin `
    -d .\build\devices\display_device.dll -display headless

# Win32 창에 표시
.\build\main.exe -r 300000 -l .\build\examples\display_demo.bin `
    -d .\build\devices\display_device.dll -display window
```

`-display`의 기본값은 `headless`다. window 모드에서 게스트가 한 프레임 이상 제출하고 HALT하면 창을 사용자가 닫을 때까지 host 프로세스가 기다린다. 프레임을 제출하지 않았으면 VM 종료와 함께 창도 바로 닫힌다.

데모 바이너리는 다음 명령으로 다시 생성할 수 있다.

```powershell
.\examples\build_display_demo.ps1
```

데모는 320×200 색상 패턴과 `HELLO VM` 텍스트를 바이너리 안에 넣고, 부팅 후 display BAR를 설정해 한 프레임을 제출한 다음 HALT한다. 디스플레이가 첫 번째 외부 장치여서 BAR 0가 `0xFFFFFFFFF0000000`에 배치되는 실행을 전제로 한다.

텍스트에는 Public Domain인 `font8x8`의 printable ASCII 테이블을 사용한다. `examples/font8x8_basic.h`가 글리프 데이터이고 `display_demo_builder.c`의 `draw_char`와 `draw_string`이 글리프 비트를 XRGB8888 픽셀로 바꾼다. 현재 데모는 빌드 단계에서 텍스트가 포함된 framebuffer를 생성한다. 장차 게스트용 C 컴파일러나 assembler가 생기면 같은 두 함수를 게스트 라이브러리로 옮겨 실행 중 문자열을 그릴 수 있다.

## 픽셀 형식과 제한

- 형식: `XRGB8888` (`FORMAT=1`), 픽셀당 4바이트
- 최대 크기: 1920×1080
- byte order가 little-endian인 host에서 한 픽셀의 RAM 배치는 `B, G, R, X`다.
- framebuffer 주소는 정렬되지 않아도 된다.
- `STRIDE`는 최소 `WIDTH * 4`, `BUFFER_SIZE`는 최소 `STRIDE * HEIGHT`여야 한다.
- host service ABI에 맞춰 `STRIDE`는 `UINT32_MAX` 이하여야 한다.
- DMA는 일반 물리 RAM만 접근한다. framebuffer 주소와 크기가 RAM을 벗어나면 제출이 실패한다.

## MMIO 레지스터

모든 레지스터는 BAR 0 기준 8바이트이며 BAR 크기는 104바이트다.

| 오프셋 | 이름 | 접근 | 의미 |
|---:|---|---|---|
| `0x00` | `CONTROL` | R/W | bit 0 enable, bit 1 IRQ enable |
| `0x08` | `WIDTH` | R/W | 픽셀 너비 |
| `0x10` | `HEIGHT` | R/W | 픽셀 높이 |
| `0x18` | `STRIDE` | R/W | 한 scanline의 byte 수 |
| `0x20` | `FORMAT` | R/W | `1 = XRGB8888` |
| `0x28` | `FRAMEBUFFER` | R/W | guest 물리 RAM 주소 |
| `0x30` | `BUFFER_SIZE` | R/W | 접근 가능한 framebuffer byte 수 |
| `0x38` | `COMMAND` | W | `1 = PRESENT` |
| `0x40` | `STATUS` | R | bit 0 ready, bit 1 busy, bit 2 error |
| `0x48` | `FRAME_NUMBER` | R | 성공한 present 횟수 |
| `0x50` | `IRQ_STATUS` | R/W1C | bit 0 완료, bit 1 오류 |
| `0x58` | `CAPABILITIES` | R | bit 0 XRGB8888, bit 1 explicit present |
| `0x60` | `ERROR_CODE` | R | 마지막 오류 코드 |

`IRQ_STATUS`는 write-one-to-clear다. `CONTROL.IRQ_ENABLE`이 켜져 있으면 present 완료나 오류가 장치에 할당된 IRQ를 발생시킨다.

## 프레임 제출 순서

1. guest가 framebuffer 영역에 XRGB8888 픽셀을 쓴다.
2. `WIDTH`, `HEIGHT`, `STRIDE`, `FORMAT`, `FRAMEBUFFER`, `BUFFER_SIZE`를 설정한다.
3. `CONTROL`에 enable과 필요하면 IRQ enable을 쓴다.
4. `COMMAND`에 `PRESENT(1)`를 쓴다.
5. polling이면 `STATUS`와 `FRAME_NUMBER`, interrupt 방식이면 `IRQ_STATUS`를 확인한다.
6. 처리한 interrupt bit를 `IRQ_STATUS`에 다시 써서 지운다.

장치는 제출 시점의 프레임 전체를 staging buffer로 복사한다. 제출 뒤 게스트가 원본 RAM을 바로 수정해도 host가 받은 프레임은 변하지 않는다. 장치 자체의 vsync는 아직 없으며 frontend가 프레임 소비 정책을 결정한다.

Win32 frontend에는 용량 3의 thread-safe frame queue가 있다. VM이 창보다 빨리 프레임을 제출하면 가장 오래된 프레임부터 버리고 최신 프레임을 표시하므로 VM 코어가 렌더링을 기다리지 않는다. `STRIDE`에 padding이 있으면 큐가 각 행을 `WIDTH * 4`바이트로 정규화한다. 창은 원본 종횡비를 유지하고 남는 영역을 검은색으로 채운다.

## 오류 코드

| 값 | 의미 |
|---:|---|
| `0` | 오류 없음 |
| `1` | 장치가 enable되지 않음 |
| `2` | 잘못된 width 또는 height |
| `3` | 지원하지 않는 픽셀 형식 |
| `4` | 잘못된 stride |
| `5` | 크기 산술 overflow 또는 buffer 부족 |
| `6` | host staging buffer 할당 실패 |
| `7` | DMA 범위 오류 |
| `8` | host frontend 처리 실패 |
| `9` | 지원하지 않는 command |

오류가 발생하면 `STATUS.ERROR`와 `ERROR_CODE`가 설정되고, IRQ가 활성화되어 있으면 오류 IRQ도 발생한다. 다음으로 유효한 `PRESENT`가 성공하면 상태와 오류 코드는 정상으로 돌아온다.
