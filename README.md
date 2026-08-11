# Computer VM

독자 ISA를 실행하는 가상 CPU/컴퓨터 프로젝트다. 64비트 정수, SIMD와
부동소수점, MMU·예외·멀티코어, 동적 VIO 장치와 GPT/FAT32 부팅을
지원한다.

## 소스 구조

```text
computer/
├─ src/                         VM 본체
├─ include/                     공용 ABI와 프로토콜 헤더
├─ kernel/                      reference kernel
├─ examples/
│  ├─ boot/                     ROM·loader·disk 부팅 예제
│  ├─ benchmarks/               ALU·branch·memory·MMU 벤치마크
│  ├─ block/                    block device 예제
│  ├─ calculation/              raw 계산 바이너리 예제
│  ├─ compiler/                 폐기된 자체 컴파일러 회귀 자료
│  ├─ counter/                  반복문 예제
│  ├─ display/                  framebuffer 예제
│  ├─ hello/                    UART 출력 예제
│  ├─ keyboard/                 keyboard 예제
│  ├─ linker/                   다중 오브젝트 링크 예제
│  ├─ llvm_ir/                  LLVM IR→CVM 변환 예제
│  └─ syscall/                  system call 예제
├─ devices/
│  ├─ block/
│  ├─ display/
│  ├─ keyboard/
│  └─ sample_counter/
├─ tools/
│  ├─ assembler/
│  ├─ archive/
│  ├─ linker/
│  ├─ compiler/                 폐기된 자체 컴파일러 회귀 자료
│  ├─ ir_translator/
│  ├─ kernel_image/
│  ├─ disk_image/
│  └─ object/
├─ tests/
├─ docs/
│  ├─ boot/
│  ├─ devices/
│  ├─ examples/
│  ├─ kernel/
│  ├─ system/
│  ├─ tests/
│  └─ tools/
└─ build.ps1                   유일한 빌드 진입점
```

예제와 장치는 프로젝트 이름별 하위 폴더가 하나의 독립 빌드 단위다. 하위
폴더에는 별도 빌드 스크립트를 두지 않는다.

## 빌드

옵션 없이 실행하면 기존 `build` 폴더를 완전히 초기화하고 VM, 모든 도구,
커널, 장치, 예제와 테스트 러너를 빌드한다.

```powershell
.\build.ps1
```

개별 프로젝트는 폴더 이름으로 선택한다. 선택 빌드도 기본적으로 `build`를
먼저 초기화한다.

```powershell
# counter 예제와 실행에 필요한 VM·도구
.\build.ps1 -e counter

# block 장치 하나
.\build.ps1 -d block

# boot 예제와 block 장치를 함께 빌드
.\build.ps1 -e boot -d block

# 기존 산출물을 유지하면서 선택 대상을 갱신
.\build.ps1 -e keyboard -d keyboard -nc
```

사용 가능한 예제 이름은 `benchmarks`, `block`, `boot`, `calculation`,
`counter`, `display`, `hello`, `keyboard`, `linker`, `llvm_ir`,
`syscall`이다. 장치
이름은 `block`, `display`, `keyboard`, `sample_counter`다. `-e all`과
`-d all`도 지원한다.

## 빌드 출력 구조

`build` 루트에는 `main.exe`와 유형별 폴더만 생성된다.

```text
build/
├─ main.exe
├─ tools/
│  ├─ vmasm.exe
│  ├─ cvmlink.exe
│  ├─ cvmar.exe
│  ├─ cvmir.exe
│  ├─ cvmclang.ps1
│  ├─ vmkimg.exe
│  ├─ vmkdisk.exe
│  ├─ test_runner.exe
│  ├─ lst/
│  └─ sym/
├─ kernel/
├─ devices/                     모든 장치 DLL을 직접 배치
│  ├─ lst/
│  └─ sym/
├─ examples/                    모든 예제 결과를 직접 배치
│  ├─ lst/                      assembler listing
│  └─ sym/                      symbol/map 파일
├─ modules/                     장치 DLL과 설정 자동 복사 위치
└─ sysroot/                     CVM headers, crt0.o, libcvm.a
```

## 실행 예시

```powershell
.\build\main.exe -r 4k `
    -l .\build\examples\counter.bin

.\build\tools\test_runner.exe

.\examples\boot\run_boot_demo.ps1
```

`-r/--ram`은 양의 정수 뒤에 `b`, `k`, `m`, `g` 단위를 붙일 수 있다.
단위는 대소문자를 구분하지 않으며, 생략하거나 `b`를 쓰면 byte이고
`k`, `m`, `g`는 각각 KiB, MiB, GiB다. 예를 들어 `-r 1m`은
1,048,576 byte를 할당한다.

부팅 예제는 ROM에서 시작해 VIO block device의 GPT/FAT32 파티션에서
`BOOT.CVM`과 `KERNEL.CVM`을 읽는다. reference kernel은 BootInfo, PMM,
MMU, VBR, W^X와 page-fault 복구를 검사하고 성공하면 UART에
`KERNEL: READY`를 출력한다.

장치를 빌드하면 DLL이 `build/devices`와 `build/modules`에 동시에
생성된다. 설정이 필수인 block 장치는 `.conf`도 자동 생성되며, boot 예제를
함께 빌드하면 생성된 `system.img`를 쓰기 가능으로 연결하도록 갱신된다.
reference kernel의 FAT32 자체 검사는 `/BOOT/KTEST.TXT`를 생성·교체한다.

세부 규격은 `docs/system`, `docs/boot`, `docs/devices`, `docs/tools`,
`docs/examples`, `docs/kernel`, `docs/tests` 아래에서 분류별로 확인할 수 있다.
