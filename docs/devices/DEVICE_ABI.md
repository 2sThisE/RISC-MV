# VIO 외부 장치 ABI

## 구조

VIO는 VM 본체, 장치 구현, host frontend와 게스트 드라이버를 분리한다.

```text
게스트 드라이버
  ↕ VIO Hub 설정 공간
  ↕ 할당된 BAR MMIO
Device Manager
  ↕ 안정된 C ABI
외부 DLL/SO 장치 모듈
  ↕ host callback
DMA · IRQ · 로그 · GUI/디버거 frontend
```

공용 ABI는 `device_abi.h`에 있다. 모듈은 VM 내부 헤더나 구조체를 포함하지 않고 이 파일만 사용한다. ABI version 1은 64비트 host를 기준으로 고정 크기 정수형과 C 함수 포인터만 사용한다.

외부 모듈은 아래 함수 하나를 export한다.

```c
const VmDeviceModule *vm_device_query(uint32_t host_abi_version);
```

반환한 `VmDeviceModule`에는 descriptor와 `create`, `destroy`, `read`, `write`, `tick`, `reset` 함수가 들어 있다. 모든 ABI 구조체는 `abi_version` 또는 `struct_size`를 포함한다. VM은 지원하지 않는 ABI, 잘못된 BAR 크기·정렬, 누락된 필수 함수를 거부한다.

## 장치 자원

Device Manager가 모듈의 descriptor를 검사한 뒤 BAR 물리주소와 IRQ를 할당한다. 외부 모듈 하나는 BAR와 IRQ를 각각 최대 4개까지 사용할 수 있다. 각 BAR는 독립적으로 크기와 2의 거듭제곱 정렬 조건을 선언하며, Device Manager가 빈 동적 MMIO 영역을 first-fit 방식으로 찾아 할당한다.

외부 BAR 할당 범위는 다음과 같다.

```text
0xFFFFFFFFF0000000 - 0xFFFFFFFFFFFBFFFF
```

장치는 할당된 주소를 하드코딩하지 않고 `VmDeviceResources`에서 읽어야 한다. 외부 장치 IRQ 범위는 0~47이고 현재 IRQ 0은 타이머, IRQ 1은 UART가 사용하며 동적 장치는 IRQ 2~47에서 할당받는다. IRQ 48~63은 IPI 전용이다. reset route는 논리 프로세서 0이며 게스트가 IRQ Controller에서 장치 IRQ를 명시적으로 활성화해야 한다.

## Host API와 격리

모듈에는 RAM, CPU, Bus, VM 구조체 포인터를 제공하지 않는다. `VmDeviceHostApi`의 다음 함수만 제공한다.

| 함수 | 의미 |
|---|---|
| `dma_read` | 검증된 물리 RAM 범위를 장치 버퍼로 복사 |
| `dma_write` | 장치 버퍼를 검증된 물리 RAM 범위에 복사 |
| `raise_irq` | 장치에 할당된 IRQ를 router로 전달 |
| `log` | host가 선택한 로그 frontend로 메시지 전달 |
| `get_service` | 이름과 version으로 선택적 host frontend를 조회 |

DMA는 MMIO가 아니라 일반 물리 RAM만 접근하며 범위 밖 요청은 거부된다. 모듈은 in-process로 실행되므로 잘못된 포인터 사용이나 무한 루프는 VM 전체에 영향을 줄 수 있다. 신뢰할 수 없는 장치를 격리하려면 향후 동일 프로토콜을 별도 프로세스 IPC backend로 확장해야 한다.

`get_service`는 장치 ABI를 특정 GUI 라이브러리나 운영체제 API에 묶지 않기 위한 확장점이다. 현재 `display.present` version 1과 `input.keyboard` version 1 서비스가 있다. 디스플레이 장치는 완성된 프레임을 host frontend로 전달하고, 키보드 장치는 정규화된 HID 입력 sink를 등록한다. 등록되지 않은 서비스나 다른 version을 요청하면 `NULL`을 반환한다. 기존 ABI version 1 모듈과의 prefix 호환성을 위해 이 함수는 `VmDeviceHostApi` 끝에 추가되어 있다.

장치 callback은 코어 worker 또는 장치 worker에서 실행될 수 있다. 오래 block하지 않아야 하며 같은 장치 BAR로 재진입하면 안 된다. Device Manager는 같은 BAR의 MMIO를 직렬화하고 `tick`은 BAR 0과 직렬화한다. 서로 다른 BAR callback은 동시에 실행될 수 있으므로 공유 장치 상태는 모듈이 직접 동기화해야 한다. 외부 ABI v1 모듈의 `tick` 인자는 경과 밀리초다. VM 내부의 1GHz 나노초 tick은 Device Manager가 누적해 완성된 밀리초만 모듈에 전달하므로 기존 모듈의 시간 의미를 유지한다.

## VIO Hub

게스트가 장치를 검색하는 고정 MMIO 설정 공간이다.

```text
VIO Hub: 0xFFFFFFFFFFFC0000 - 0xFFFFFFFFFFFC0FFF
```

전역 레지스터는 모두 64비트다.

| 오프셋 | 이름 | 의미 |
|---:|---|---|
| `0x00` | `MAGIC` | `0x314F4956` (`VIO1`) |
| `0x08` | `ABI` | VIO Hub ABI version, 현재 2 |
| `0x10` | `SLOT_COUNT` | 검색 가능한 슬롯 수 16 |
| `0x18` | `GENERATION` | 장치가 추가되거나 제거될 때마다 증가 |
| `0x20` | `EVENT_STATUS` | bit 0 장치 추가, bit 1 장치 제거 |
| `0x28` | `EVENT_ACK` | 1을 쓴 이벤트 비트를 해제 |

슬롯 설정 공간은 `0x100 + slot * 0xC0`에서 시작한다. Hub ABI 1의 BAR0·IRQ0 오프셋은 그대로 유지된다.

| 슬롯 내 오프셋 | 이름 | 의미 |
|---:|---|---|
| `0x00` | `STATUS` | bit 0 present, bit 1 external module |
| `0x08` | `CLASS` | system, timer, serial, display, USB, storage 등 |
| `0x10` | `VENDOR` | vendor ID |
| `0x18` | `DEVICE` | device ID |
| `0x20` | `VERSION` | 장치 version |
| `0x28` | `BAR0_BASE` | BAR 0 물리주소 |
| `0x30` | `BAR0_SIZE` | BAR 0 크기 |
| `0x38` | `IRQ0` | IRQ 번호, 없으면 `UINT64_MAX` |
| `0x40` | `FEATURES` | 장치별 기능 비트 |
| `0x48` | `DEVICE_ABI` | 장치가 사용하는 ABI version |
| `0x50` | `BAR_COUNT` | 할당된 BAR 개수 |
| `0x58` | `IRQ_COUNT` | 할당된 IRQ 개수 |
| `0x60`, `0x68` | `BAR1_BASE`, `BAR1_SIZE` | BAR 1 정보 |
| `0x70`, `0x78` | `BAR2_BASE`, `BAR2_SIZE` | BAR 2 정보 |
| `0x80`, `0x88` | `BAR3_BASE`, `BAR3_SIZE` | BAR 3 정보 |
| `0x90` | `IRQ1` | IRQ 1 번호, 없으면 `UINT64_MAX` |
| `0x98` | `IRQ2` | IRQ 2 번호, 없으면 `UINT64_MAX` |
| `0xA0` | `IRQ3` | IRQ 3 번호, 없으면 `UINT64_MAX` |

부팅 시 슬롯 0은 타이머, 슬롯 1은 UART, 슬롯 2는 IRQ Controller, 슬롯 3은 Core Control, 슬롯 4는 System Information, 슬롯 5는 System Control이다. 외부 장치는 다음 빈 슬롯부터 연결된다. `device_manager_detach_module()`은 장치 BAR를 원자적으로 비활성화하고, 진행 중인 callback이 끝날 때까지 기다린 뒤 IRQ와 MMIO 자원을 반환한다. 제거된 주소와 IRQ는 이후 장치가 다시 사용할 수 있다. 분리는 장치 callback 내부가 아닌 host 제어 스레드에서 호출해야 한다.

현재 `main`은 시작 시 모듈을 검색하지만 폴더 변경을 실시간 감시하지는 않는다. 실행 중 hot-plug를 제공하는 관리 UI나 디버거는 attach/detach API를 호출하고, 게스트는 `GENERATION`과 이벤트 비트를 확인해 Hub 슬롯을 다시 검색하면 된다.

## 동적 로딩

Windows는 `LoadLibrary/GetProcAddress`, POSIX는 `dlopen/dlsym`을 사용하는 얇은 `module_loader.c` 계층으로 분리되어 있다. `main`은 현재 작업 디렉터리가 아니라 실행 파일이 위치한 경로를 기준으로 `modules` 폴더를 검색한다. Windows에서는 `.dll`, POSIX에서는 `.so`, `.dylib`, `.dll`을 파일명 순서대로 자동 연결한다.

예를 들어 `build/main.exe`가 실행된다면 다음 파일이 자동 연결된다.

```text
build/modules/sample_counter.dll
build/modules/display_device.dll
```

설정이 필요한 모듈은 확장자를 `.conf`로 바꾼 sidecar 파일을 사용한다.

```text
build/modules/block_device.dll
build/modules/block_device.conf
```

`block_device.conf` 예시:

```text
path=.\disk.img;create=67108864
```

파일의 앞뒤 공백과 마지막 줄바꿈은 제거되고 나머지 문자열이 `create()`의 `configuration`으로 전달된다. 설정 안의 상대 경로는 VM을 실행한 현재 작업 디렉터리를 기준으로 한다. 모듈 폴더가 없거나 비어 있으면 자동 연결 없이 정상적으로 시작한다.

직접 지정하는 기존 방식도 유지된다. `modules` 폴더에서 이미 자동 연결된
DLL을 `-d`로 다시 지정하면 별도 장치 인스턴스가 하나 더 연결된다. 하나만
사용하려면 자동 모듈 또는 명시적 `-d` 중 한 경로만 선택한다.

```text
.\build\main.exe -r 1024 `
    -l .\build\examples\calculation.bin `
    -d device1.dll -d device2.dll
```

모듈의 `create()`에 설정을 전달하려면 해당 `-d` 뒤에 `-dc`를 사용한다.

```powershell
.\build\main.exe -r 4096 -l guest.bin `
    -d .\build\devices\block_device.dll `
    -dc "path=.\disk.img;create=67108864" `
    -d .\build\devices\display_device.dll
```

동일한 `-d` 옵션을 여러 번 사용할 수 있다. POSIX에서 직접 링크할 때는 환경에 따라 VM 빌드에 `-ldl`이 필요할 수 있다.

## 샘플 외부 장치

`devices/sample_counter/sample_counter.c`는 VM 본체와 별도로 빌드되는 counter/IRQ 장치다.

```powershell
.\build.ps1 -e calculation -d sample_counter
.\build\main.exe -r 1024 `
    -l .\build\examples\calculation.bin
```

성공하면 모듈 이름, VIO 슬롯, 할당된 BAR와 IRQ가 출력된다. 이 샘플은 외부 디스플레이·USB controller·저장장치를 구현할 때 사용할 수 있는 최소 골격이다.

## 디스플레이 장치

`devices/display/display_device.c`는 guest RAM framebuffer를 DMA로 읽어 `display.present` host service에 제출하는 독립 DLL이다.

```powershell
.\build.ps1 -e display -d display
.\build\main.exe -r 300000 -l .\build\examples\display_demo.bin `
    -display headless
```

main은 기본적으로 프레임을 헤드리스 frontend에 보관하고, Windows에서는 `-display window`로 Win32 frontend를 선택할 수 있다. frontend가 바뀌어도 장치 DLL과 게스트 드라이버 규격은 유지된다. 자세한 MMIO 규격은 `DISPLAY.md`에 있다.

## 블록 저장장치

`devices/block/block_device.c`는 raw host 파일을 512바이트 LBA 장치로 노출한다. 최대 64KiB DMA READ/WRITE, 명시적 FLUSH, 비동기 worker, 완료 IRQ와 읽기 전용 모드를 지원한다.

```powershell
.\build.ps1 -e block -d block
.\build\main.exe -r 4096 -l .\build\examples\block_demo.bin
```

빌드가 생성한 `build/modules/block_device.conf`가 자동 연결 장치의 backing
image를 지정한다. 다른 이미지나 읽기 전용 설정을 사용하려면 이 sidecar를
수정한다. 장치 MMIO, 오류 코드와 backing image 규칙은 `BLOCK_DEVICE.md`에
정의되어 있다.

## 키보드 장치

`devices/keyboard/keyboard_device.c`는 `input.keyboard` Host Service를 256-entry HID event queue, MMIO와 IRQ로 노출한다.

```powershell
.\build.ps1 -e keyboard -d keyboard
.\build\main.exe -r 4096 -l .\build\examples\keyboard_demo.bin `
    -display window
```

게스트는 VIO Hub에서 input class 장치를 검색해야 한다.
`examples/keyboard/keyboard_demo.asm`은 동적으로 BAR와 IRQ를 찾고 이벤트를
UART로 출력한다. 이벤트 형식과 드라이버 처리 순서는 `KEYBOARD.md`에
정의되어 있다.
