# VIO HID 키보드

키보드는 host 입력 frontend와 게스트 장치를 분리한다.

```text
Win32 WM_KEYDOWN/WM_KEYUP
        ↓
KeyboardInput (`input.keyboard` service)
        ↓
keyboard_device.dll 이벤트 큐
        ↓ BAR MMIO + IRQ
게스트 키보드 드라이버
```

`window_display`는 Windows virtual key를 USB HID keyboard usage ID로 정규화한다. 키보드 모듈은 Win32를 포함하지 않으므로 다른 플랫폼 frontend나 디버거도 `keyboard_input_emit()`을 통해 같은 장치를 사용할 수 있다. Headless 모드에도 Host Service가 등록되지만 기본 물리 입력 frontend는 없으며 테스트나 디버거가 이벤트를 주입할 수 있다.

## 빌드와 자동 연결

```powershell
.\build.ps1 -e keyboard -d keyboard
.\build\main.exe -r 4096 -l .\build\examples\keyboard_demo.bin `
    -display window
```

데모는 VIO Hub의 모든 슬롯을 검색하므로 키보드의 슬롯, BAR와 IRQ를 하드코딩하지 않는다. 키보드를 찾으면 UART에 `K`를 출력하고 대기한다. 창에 포커스를 둔 채 키를 누르면 터미널에 다음처럼 HID usage가 표시된다.

```text
D:04
U:04
```

`D`는 key-down, `U`는 key-up이며 `04`는 A 키의 HID usage다. Escape key-down(`D:29`)은 VM을 `HALT`해 창과 프로그램을 종료한다. 키보드 모듈을 찾지 못하면 `N`을 출력하고 즉시 종료한다.

키보드 장치는 VIO Hub에서 `VM_DEVICE_CLASS_INPUT` 장치로 검색해야 한다. 자동 할당된 BAR나 IRQ 번호를 하드코딩하면 안 된다. `examples/keyboard_demo.asm`이 장치 검색, 동적 IRQ 벡터 설치, 큐 drain과 UART 출력을 포함한 최소 게스트 드라이버 예제다.

## BAR 0 MMIO

모든 레지스터 접근 폭은 64비트다. BAR 크기는 `0x40`이다.

| 오프셋 | 이름 | 접근 | 의미 |
|---:|---|---|---|
| `0x00` | `CONTROL` | R/W | bit 0 enable, bit 1 IRQ enable |
| `0x08` | `STATUS` | R/W1C | bit 0 event available, bit 1 overflow |
| `0x10` | `EVENT_COUNT` | R | 대기 중인 이벤트 수 |
| `0x18` | `EVENT_DATA` | R/pop | 가장 오래된 이벤트를 읽고 제거, 비어 있으면 0 |
| `0x20` | `CAPACITY` | R | 이벤트 큐 용량, 현재 256 |
| `0x28` | `IRQ_STATUS` | R/W1C | bit 0 event, bit 1 overflow |
| `0x30` | `DROPPED` | R/W | 버린 이벤트 수, 0이 아닌 값을 쓰면 초기화 |
| `0x38` | `VERSION` | R | 키보드 프로토콜 version, 현재 1 |

장치를 disable하면 큐, overflow와 IRQ 상태가 초기화된다. 큐가 가득 차면 기존 이벤트는 유지하고 새 이벤트를 버리며 `DROPPED`를 증가시킨다.

## 이벤트 형식

`EVENT_DATA`의 64비트 구성은 다음과 같다.

| 비트 | 의미 |
|---:|---|
| `0..15` | USB HID keyboard usage ID |
| `16` | 1이면 key-down, 0이면 key-up |
| `17` | 자동 반복 key-down |
| `18` | host extended key 표시 |
| `24..31` | 현재 modifier 상태 |
| `32..63` | Host Service가 붙인 단조 증가 이벤트 순번 |

modifier 비트는 왼쪽 Ctrl, Shift, Alt, GUI와 오른쪽 Ctrl, Shift, Alt, GUI 순서다. 장치는 문자나 키보드 배열을 해석하지 않는다. 예를 들어 물리적인 A 키는 HID usage `0x04`로 전달하며 `a`, `A`, `ㅁ` 같은 문자 변환은 게스트 운영체제의 키보드 레이아웃 계층이 담당한다.

창이 포커스를 잃거나 닫힐 때 `window_display`는 아직 눌려 있는 모든 키에 key-up 이벤트를 생성해 게스트에 키가 눌린 채 남는 현상을 방지한다.

## 인터럽트 처리

이벤트이거나 overflow가 발생할 때 IRQ enable 상태라면 할당된 IRQ를 발생시킨다. 게스트 드라이버의 기본 순서는 다음과 같다.

1. `IRQ_STATUS`를 읽는다.
2. `EVENT_COUNT`가 0이 될 때까지 `EVENT_DATA`를 읽는다.
3. 처리한 `IRQ_STATUS` 비트를 W1C로 해제한다.
4. overflow라면 `STATUS` bit 1도 W1C로 해제한다.
5. IRQ Controller의 `EOI`에 할당된 IRQ 번호를 기록한다.

새 이벤트이지만 IRQ pending bit가 이미 설정된 경우에도 이벤트는 큐에 보존되므로 드라이버는 한 번의 인터럽트에서 큐를 전부 비워야 한다.
