# 외부 VIO 장치 예제

`sample_counter.c`는 VM 본체와 링크되지 않는 독립 장치 모듈이다. 공용 `device_abi.h`만 포함하고 `vm_device_query`를 export한다.

```powershell
.\devices\build_sample.ps1
Copy-Item .\build\devices\sample_counter.dll .\build\modules\
.\build\main.exe -r 1024 -l .\examples\calculation.bin
```

장치는 Device Manager에서 BAR 0과 IRQ 하나를 할당받는다. BAR 0의 64비트 레지스터는 `VALUE=0x00`, `STEP=0x08`, `COMPARE=0x10`, `CONTROL=0x18`, `STATUS=0x20`이다. `CONTROL` bit 0은 enable, bit 1은 IRQ enable이다.

외부 모듈은 host가 제공하는 검증된 DMA, IRQ, 로그 함수만 사용할 수 있으며 VM의 RAM·CPU·Bus 구조체 포인터를 직접 받지 않는다.

## framebuffer display

`display_device.c`는 guest RAM의 XRGB8888 framebuffer를 host display service로 넘기는 독립 모듈이다.

```powershell
.\devices\build_display.ps1
Copy-Item .\build\devices\display_device.dll .\build\modules\
.\build\main.exe -r 4096 -l .\guest.bin
```

현재 기본 frontend는 마지막 완성 프레임을 메모리에 보관하는 headless 방식이다. 프레임이 제출되면 종료 시 크기, frame number, byte 수가 출력된다. 레지스터와 제출 절차는 `..\docs\DISPLAY.md`를 참고한다.

## HID 키보드

`keyboard_device.c`는 `input.keyboard` Host Service에서 정규화된 USB HID key event를 받아 256-entry MMIO 큐와 IRQ로 게스트에 제공한다.

```powershell
.\devices\build_keyboard.ps1
.\examples\build_keyboard_demo.ps1
Copy-Item .\build\devices\keyboard_device.dll .\build\modules\
.\build\main.exe -r 4096 -l .\build\examples\keyboard_demo.bin `
    -display window
```

키보드 배열과 문자 변환은 게스트의 책임이다. 전체 MMIO와 이벤트 비트 규격은 `..\docs\KEYBOARD.md`를 참고한다.
