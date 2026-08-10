# Computer VM

독자 ISA를 실행하는 가상 CPU/컴퓨터 프로젝트다. 64비트 범용 연산, 128비트 정수 SIMD, IEEE-754 스칼라·벡터 부동소수점, MMU·예외·멀티코어와 외부 장치를 지원하며 구현과 개발 도구를 분리한다.

## 폴더 구조

```text
computer/
├─ src/                 VM 본체와 실행 프로그램 C 소스
├─ include/             VM, ISA, 장치 ABI와 프로토콜 헤더
├─ devices/             외부 VIO 장치 모듈 소스와 빌드 스크립트
├─ tools/assembler/     독자 ISA용 2-pass 어셈블러
├─ examples/            게스트 어셈블리와 예제 생성기 소스
├─ tests/               통합 테스트 러너와 테스트 suite
├─ docs/                ISA 및 장치 규격 문서
├─ build/               실행 파일, DLL, 생성된 예제 바이너리
│  └─ modules/          main이 시작할 때 자동 연결하는 장치 모듈
└─ build.ps1            VM 본체 빌드 진입점
```

`src`는 `include`의 헤더를 사용하며, 장치 모듈도 내부 구현 대신 `include/device_abi.h`와 각 프로토콜 헤더에만 의존한다. 컴파일 결과는 소스 폴더에 섞이지 않고 `build` 아래에 생성된다.

## 대표 명령

프로젝트 루트에서 실행한다.

```powershell
# VM 본체
.\build.ps1

# 어셈블러
.\tools\assembler\build.ps1

# kernel.cvm 패키저
.\tools\kernel_image\build.ps1

# 전체 테스트
.\tests\build_runner.ps1
.\build\test_runner.exe

# 외부 장치
.\devices\build_sample.ps1
.\devices\build_display.ps1
.\devices\build_block.ps1
.\devices\build_keyboard.ps1
```

실행 옵션 전체와 예시는 내장 도움말에서 확인할 수 있다.

```powershell
.\build\main.exe --help
```

짧은 옵션과 긴 옵션을 모두 지원한다. 예를 들어 `-r`, `-l`, `-rom`, `-c`, `-t`는 각각 `--ram`, `--load`, `--rom`, `--cores`, `--threads`로도 쓸 수 있다. 인자 오류는 `main.exe: error: ...` 형식으로 표준 오류에 출력되고 종료 코드 2를 반환한다. 도움말과 정상 실행은 0, VM 초기화·실행 실패는 1을 반환한다.

Boot ROM에서 시작하려면 ROM 이미지를 `0x7FFFF00000` 기준으로 어셈블하고 `-rom`으로 실행한다. 이 모드에서는 `-l`이 선택 사항이며, 둘 다 주면 ROM에서 시작하되 `-l` 바이너리도 RAM 1번지에 미리 적재된다.

```powershell
.\tools\assembler\build.ps1
.\examples\build_boot_rom.ps1
.\build\main.exe -r 4096 -rom .\build\examples\boot_rom.bin
```

예제 ROM은 UART로 `ROM`을 출력하고 종료한다. ROM은 읽기·실행만 가능하고, MMIO는 데이터 LOAD/STORE만 가능하며 명령어 fetch는 거부된다.

빌드된 외부 장치는 `build/devices`에 보관된다. 별도 `-d` 인자 없이 항상 연결할 장치는 `main.exe` 옆의 `modules` 폴더에 복사한다.

```powershell
Copy-Item .\build\devices\sample_counter.dll .\build\modules\
.\build\main.exe -r 1024 -l .\examples\calculation.bin
```

설정이 필요한 모듈은 DLL과 같은 이름의 `.conf` 파일을 함께 둔다. 예를 들어 `block_device.dll`은 `block_device.conf`의 `path=.\disk.img;create=67108864` 문자열을 `create()` 설정으로 받는다. `-d/-dc`도 일회성 또는 명시적 연결용으로 계속 지원한다.

예제 어셈블:

```powershell
.\build\vmasm.exe .\examples\counter.asm `
    -o .\build\examples\counter.bin `
    --symbols .\build\examples\counter.sym `
    --listing .\build\examples\counter.lst

.\build\main.exe -r 4096 -l .\build\examples\counter.bin
```

키보드 게스트 데모:

```powershell
.\examples\build_keyboard_demo.ps1
Copy-Item .\build\devices\keyboard_device.dll .\build\modules\
.\build\main.exe -r 4096 -l .\build\examples\keyboard_demo.bin `
    -display window
```

키 이벤트는 UART를 통해 터미널에 출력되며 Escape를 누르면 VM이 종료된다.

세부 규격은 `docs/ISA.md`, `docs/BOOT.md`, `docs/BOOT_FORMAT.md`, `docs/SYSTEM.md`, `docs/IRQ_CONTROLLER.md`, `docs/DEVICE_ABI.md`, `docs/DISPLAY.md`, `docs/KEYBOARD.md`, `docs/BLOCK_DEVICE.md`를 참고한다.
