# Clang/LLVM RArchM64 크로스 컴파일 경로

레거시 이름의 `cvmir`는 LLVM 22의 공식 IR parser와 verifier를 사용하고
RArchM64 ABI v1
어셈블리 또는 `CVMOBJ2` 오브젝트를 생성하는 assembly bridge다. LLVM 문법을
프로젝트가 다시 해석하지 않으며, RArchM64 instruction selection과 stack frame,
데이터 배치만 담당한다.

`cvmclang.ps1`은 Clang 호출, RArchM64 target 정규화, legacy 이름의 `cvmir`,
`vmasm`, `cvmlink`,
`crt0.o`와 `libcvm.a` 연결을 묶는 freestanding C driver다.

## 개발 도구 설치

Windows에서는 MSYS2 UCRT64 패키지를 기준 환경으로 사용한다.

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-clang \
  mingw-w64-ucrt-x86_64-llvm \
  mingw-w64-ucrt-x86_64-llvm-libs \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja
```

PowerShell에서 UCRT64 도구를 PATH 앞에 둔다.

```powershell
$env:LLVM_ROOT = 'C:\msys64\ucrt64'
$env:Path = "$env:LLVM_ROOT\bin;$env:Path"

where.exe clang
clang --version
llvm-config --version
```

현재 검증 기준은 Clang/LLVM 22.1.8이다. `build.ps1`은 `llvm-config`가 없거나
22.x가 아니거나 개발 헤더가 없으면 진단 후 중단한다.

## RArchM64 target 규격

```text
target triple = "rarchm64-unknown-none"
target datalayout = "e-p:64:64-i8:8-i16:16-i32:32-i64:64-f32:32-f64:64-v128:128-a:0:64-n8:16:32:64-S128"
```

설치된 Clang에는 아직 upstream RArchM64 TargetInfo가 없으므로 driver는
`x86_64-unknown-none-elf`의 LP64 frontend 규칙과 `-mlong-double-64`를 사용해
IR을 만든다. 그 뒤 triple과 DataLayout을 위 값으로 정규화한다. 이 호환
profile은 RArchM64 ABI v1의 정수·포인터 크기, 자연 정렬, signed `char`, 32비트
`wchar_t`, 64비트 `long double`과 일치한다. packed 구조체와 bit-field는 ABI
v1 범위 밖이다.

도구가 사용하는 target 값은 다음 명령으로 확인한다.

```powershell
.\build\tools\cvmir.exe --print-target
```

## 사용법

C 소스에서 오브젝트를 만든다.

```powershell
.\build\tools\cvmclang.ps1 kernel.c -c -o kernel.o
```

기본 `_start`, 64KiB stack, `libcvm.a`를 사용해 실행 이미지를 만든다. C
소스에는 `main`이 있어야 한다.

```powershell
.\build\tools\cvmclang.ps1 program.c -o program.exf
```

커널처럼 시작 코드와 entry를 직접 지정할 수도 있다.

```powershell
.\build\tools\cvmclang.ps1 kernel.c -o kernel.exf `
    -Startup kernel_start.s -Entry kernel_entry -Base 0x10000
```

LLVM IR을 직접 변환하는 저수준 명령은 다음과 같다. `cvmir.exe`는 비-RArchM64 target triple의 IR을 허용하기 위한 `--allow-foreign-triple` 옵션과 도움말 표시를 위한 `-h`/`--help` 옵션을 추가로 지원한다. `cvmclang.ps1`은 추가 인클루드 경로 지정을 위해 `-IncludeDirectory` 파라미터를 지원한다.

```powershell
.\build\tools\cvmir.exe -S input.ll -o input.s [--allow-foreign-triple]
.\build\tools\cvmir.exe -c input.ll -o input.o
.\build\tools\cvmclang.ps1 input.c -o input.exf -IncludeDirectory .\include
```

## 지원 범위

- `i1`, `i8`, `i16`, `i32`, `i64`와 64비트 pointer
- 정수 산술·논리·shift, `icmp`, `select`, 정수 cast
- basic block, `phi`, 조건·무조건 분기
- `alloca`, scalar `load/store`, volatile MMIO 접근
- array/structure `getelementptr`와 전역 `.rodata/.data/.bss`
- 직접 함수 호출, scalar register/stack argument, void/scalar return
- variadic 호출에서 unnamed argument의 stack 전달
- Clang의 `sret` hidden pointer처럼 IR에 명시된 pointer parameter
- weak/common/external symbol과 `CVMOBJ2` relocation
- `<cvm/intrin.h>`로 노출되는 CPU 제어·상태 조회·64비트 원자 명령

각 SSA 값은 정확성 우선으로 8바이트 stack slot에 spill한다. R14를 frame
pointer로 사용하고 CALL 직전 SP를 16바이트로 정렬한다. `cvmclang`은 현재
`-O0`을 사용한다. 최적화 IR은 i65 같은 RISC-MV 비정규 정수 폭을 만들 수 있으므로
legalization pass가 생기기 전에는 기본 경로로 사용하지 않는다.

아직 지원하지 않는 범위는 floating/vector IR, `switch`, indirect call,
LLVM memory intrinsic, aggregate SSA return, 동적 `alloca`, variadic 함수 내부의
`va_start/va_arg`다. 이 기능을 만나면 잘못된 코드를 생성하지 않고 오류로
중단한다.

## CPU intrinsic

`<cvm/intrin.h>`는 일반 함수처럼 호출하지만 `cvmir`가 외부 심볼 호출이 아닌
단일 RISC-MV 명령으로 직접 내린다. 따라서 별도 런타임 구현이나 링크 심볼이
필요하지 않다.

- 실행 제어: `cvm_halt`, `cvm_nop`, `cvm_wait`, `cvm_fence`
- 인터럽트: `cvm_enable_interrupts`, `cvm_disable_interrupts`
- 실행 위치: `cvm_core_id`, `cvm_thread_id`
- 예외 상태: `cvm_exception_cause`, `cvm_exception_address`,
  `cvm_exception_pc`, `cvm_exception_info`
- 권한·예외 벡터: `cvm_set_vbr`, `cvm_get_vbr`, `cvm_get_mode`,
  `cvm_enter_user`
- MMU: `cvm_set_ptbr`, `cvm_get_ptbr`, `cvm_mmu_on`, `cvm_mmu_off`,
  `cvm_get_mmu`
- 커널 stack pointer: `cvm_set_ksp`, `cvm_get_ksp`
- 원자 연산: `cvm_cas64`, `cvm_xchg64`, `cvm_atomic_add64`

원자 연산의 반환값은 연산 전 메모리 값이다. `cvm_cas64`는 그 값이
`expected`와 같을 때만 `desired`를 저장한다. 이 API는 C11 `_Atomic`과 메모리
순서를 완전히 구현한 것이 아니므로 공유 자료구조는 필요한 위치에
`cvm_fence()`를 명시해야 한다.

`IRET`은 일반 C 함수 호출 문맥에서 compiler epilogue와 충돌하므로 intrinsic으로
노출하지 않는다. 예외 진입과 복귀는 레지스터 저장·복원 및 `IRET`을 수행하는
짧은 assembly wrapper에 두고, 실제 처리 함수만 C로 호출한다.

## sysroot와 런타임

빌드 결과는 `build/sysroot`에 생성된다.

```text
build/sysroot/
├─ include/            stddef/stdint/stdbool/limits/string, cvm/mmio, cvm/intrin
└─ lib/
   ├─ crt0.o           _start, 기본 64KiB stack, main 호출
   ├─ builtins.o
   └─ libcvm.a         memcpy/memmove/memset/memcmp/strlen
```

현재 libc는 최소 freestanding runtime이며 파일, heap, 표준 입출력은 제공하지
않는다. 커널에서는 `cvm/mmio.h`의 volatile load/store helper로 MMIO 장치를
접근할 수 있다.

## 현재 backend 선택

현재 정식 경로는 LLVM IR에서 레거시 이름의 RISC-MV
assembler/object/linker로 이어지는 bridge다. LLVM MC/object writer, LLD와
native RArchM64 backend 포팅은 장기 검토 항목이다. 즉 현재 경로는 임시
hand-written IR parser가 아니지만 LLVM 자체의 native code generator도 아니다.
