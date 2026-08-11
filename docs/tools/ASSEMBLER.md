# RISC-MV 어셈블러 (`vmasm`)

`vmasm`은 이 프로젝트의 독자 ISA를 위한 C11 2-pass 어셈블러다. 첫 번째 패스에서 레이블 주소를 정하고 두 번째 패스에서 바이트를 생성하므로, 아직 선언되지 않은 앞쪽 레이블도 분기·호출 피연산자로 사용할 수 있다.

## 빌드와 실행

```powershell
.\build.ps1 -e counter
.\build\tools\vmasm.exe .\examples\counter\counter.asm `
    -o .\build\examples\counter.bin
.\build\main.exe -r 4096 -l .\build\examples\counter.bin
```

기본 기준 주소는 `1`이다. 이는 현재 `build/main.exe -l`이 raw 바이너리를 RAM 1번지에 적재하고 PC도 1로 시작하는 규칙과 맞는다.

Boot ROM 이미지는 고정 물리 주소에 맞게 별도 기준 주소를 사용한다.

```powershell
.\build\tools\vmasm.exe .\examples\boot\boot_rom.asm `
    -o .\build\examples\boot_rom.bin `
    --base 0x7FFFF00000
```

```text
vmasm INPUT.s -c -o OUTPUT.o
vmasm INPUT.asm -o OUTPUT.bin [--base ADDRESS]
      [--symbols FILE] [--listing FILE]
```

일반적인 네이티브 도구 관례에 맞춰 재배치 가능한 소스는 `.s`, 오브젝트는
`.o`를 사용한다. 기존 `.asm -> .bin`은 고정 주소 raw 바이너리 호환
경로로 유지한다. 여러 오브젝트를 부팅 가능한 이미지로 링크하려면 다음처럼
실행한다.

```powershell
.\build\tools\vmasm.exe source.s -c -o source.o
.\build\tools\cvmlink.exe source.o support.o -o KERNEL.EXF `
    --base 0x10000 --entry kernel_entry --map kernel.map
```

`.exf`는 RISC-MV 플랫폼의 정식 실행/부팅 이미지
확장자다. 정적 라이브러리는 통상 관례대로 `.a`를 사용하며 `cvmar`로 만든다.
자세한 사용법은 [STATIC_LIBRARY.md](STATIC_LIBRARY.md)를 참고한다.

- `--base`: 코드가 적재될 가상/물리 시작 주소. 10진수 또는 `0x` 16진수로 지정한다.
- `--symbols`: `주소 레이블` 형식의 심볼 맵을 만든다.
- `--listing`: 주소와 생성 바이트를 16바이트 단위로 기록한다.

출력 `.bin`은 헤더 없는 raw 기계어다. 따라서 `.entry` 값은 어셈블 결과와 콘솔에 표시되지만 파일 안에는 저장되지 않는다. 현재 로더로 바로 실행하려면 시작 코드를 기준 주소에 두거나 그 위치에 시작 레이블로 가는 분기를 둬야 한다.

## 문법

명령어, 레지스터, 조건 코드는 대소문자를 구분하지 않는다. 레이블은 대소문자를 구분한다. 주석은 `;`부터 줄 끝까지다.

```asm
.entry start

start:
    MOVI32U SP, 4096
    MOVI32U R0, 5

loop:
    ADDI32 R0, -1
    CMPI32 R0, 0
    BRCC NE, loop
    CALLREL finish
    HALT

finish:
    MOVI32U R1, 42
    RET
```

범용 레지스터는 `R0`부터 `R15`까지이며 `SP`는 `R15`의 별칭이다. SIMD·부동소수점용 128비트 벡터 레지스터는 `V0`부터 `V15`까지다. 정수는 10진수, `0x` 16진수, `0b` 2진수를 지원하고 숫자 사이 `_`는 무시한다. 표현식은 숫자, 레이블, 현재 주소 `$`, 괄호와 `+ - ~ * / % << >> & ^ |`를 C와 같은 우선순위로 지원한다.

## 재배치 오브젝트와 소스 전처리

오브젝트 소스는 `.section .text/.rodata/.data/.bss`, `.global`/`.globl`,
`.extern`, `.weak`, `.type`, `.size`, `.comm`, `.entry`를 지원한다.
`cvmlink`는 여러 `.o`의 전역 심볼을 해석하고 로컬
심볼을 오브젝트별로 격리한 뒤 실제 바이너리 재배치를 적용하고, 각 섹션을
4KiB 정렬된 RX/R/RW/RW `RMVEXF01` 세그먼트로 만든다. `.o`에는 소스가
아니라 기계어 바이트, 심볼 값, 재배치 표가 저장된다. `.bss`는 메모리 크기만
가지며 파일에는 제로 바이트를 저장하지 않는다. 중복 전역, 미해결 외부 심볼,
재배치 overflow, 실행 불가능한 엔트리는 오류다. 세부 포맷은
[OBJECT_FORMAT.md](OBJECT_FORMAT.md)를 참고한다.

`.include "relative/path.inc"`는 현재 파일 기준으로 포함하며 순환 포함과
16단계 초과를 거부한다. 매크로는 다음 문법을 사용한다. `\@`는 매 확장마다
다른 정수이므로 매크로 내부 로컬 레이블을 만들 때 쓸 수 있다.

```asm
.macro LOAD_PAIR left, right
    MOVI32U R0, \left
    MOVI32U R1, \right
.endm

LOAD_PAIR 20, 22
```

`BRCC` 조건은 `ALWAYS`, `EQ`, `NE`, `LT`, `LE`, `GT`, `GE`, `LTU`, `LEU`, `GTU`, `GEU`, `ORD`, `UNO`다. `ORD/UNO`는 `FCMP32/64`의 NaN 비교 결과를 검사한다. `JUMPREL`, `CALLREL`, `BRCC`에는 목표 레이블을 적으면 어셈블러가 다음 명령 주소 기준의 signed 32-bit 변위를 계산한다.

```asm
    VLOAD128 V0, R0
    VLOAD128 V1, R1
    VADD32 V2, V0, V1       ; uint32 lane 4개
    VFADD32 V3, V0, V1      ; float lane 4개
    FADD64 V4, V0, V1       ; 최하위 double lane 하나
    VSTORE128 R2, V3
```

실수 리터럴 문법은 아직 없으므로 상수는 `.dword`/`.qword`에 IEEE-754 비트 패턴으로 넣거나 `I32TOF32`, `I64TOF64` 변환 명령으로 만든다.

시스템 콜용 `SYSCALL`, 커널 스택 설정용 `SETKSP reg`, 조회용 `GETKSP reg`도 지원한다. `SETKSP`와 `GETKSP`는 CPU에서 supervisor 전용으로 검사된다.

## 데이터 지시어

| 지시어 | 동작 |
|---|---|
| `.byte values...` | 각 값을 1바이트 little-endian으로 기록 |
| `.word values...` | 각 값을 2바이트로 기록 |
| `.dword values...` | 각 값을 4바이트로 기록 |
| `.qword values...` | 각 값을 8바이트로 기록 |
| `.ascii "text"` | 문자열 바이트 기록 |
| `.asciz "text"` | 문자열과 끝의 NUL 기록 |
| `.space count[, fill]` | 지정 바이트 수만큼 채움, 기본값 0 |
| `.zero count` | 지정 바이트 수만큼 0으로 채움 |
| `.align n[, fill]` | 현재 주소를 `n`의 배수로 전진시켜 채움 |
| `.org address` | 현재 주소를 앞쪽 절대 주소로 이동 |
| `.entry expression` | 진입점 메타데이터 지정 |
| `.equ name, expression` | 변경 불가능한 정수 심볼 정의 |
| `.set name, expression` | 현재는 `.equ`와 같은 정수 심볼 정의 |

다음 지시어는 재배치 오브젝트 모드(`-c`)의 심볼 메타데이터다.

| 지시어 | 동작 |
|---|---|
| `.global name...`, `.globl name...` | 다른 오브젝트에 공개할 strong 심볼 |
| `.extern name...` | 다른 오브젝트가 제공해야 하는 심볼 |
| `.weak name...` | weak 정의 또는 미정의 weak 참조 |
| `.type name, function` | 함수 심볼 표시 |
| `.type name, object` | 데이터 객체 심볼 표시 |
| `.size name, expression` | 심볼 크기 기록. `$ - name` 사용 가능 |
| `.comm name, size[, alignment]` | zero-fill tentative 전역 객체. 기본 정렬 8 |

`.type`은 이 어셈블러의 정규 문법인 `function`/`object`를 사용하며 GNU
assembler의 `@function` 표기는 사용하지 않는다. 해석되지 않은 weak 참조는
링크 시 주소 0이 된다. 같은 common 심볼은 링커가 가장 큰 크기와 정렬로
병합하고 strong 정의가 common보다 우선한다.

문자열과 문자 리터럴은 `\0`, `\n`, `\r`, `\t`, `\\`, `\'`, `\"`, `\xNN` 이스케이프를 지원한다. `.org`는 뒤로 이동할 수 없고 전체 출력은 안전을 위해 256 MiB로 제한된다. `.org`는 재배치 가능한 `.o`에서는 허용되지 않는다.
