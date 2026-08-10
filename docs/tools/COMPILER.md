# cvmcc 부트스트랩 C 컴파일러

> 이 도구의 신규 개발은 중단됐다. 소스와 테스트는 과거 ABI 회귀 자료로만
> 보존하며 기본 빌드에는 포함하지 않는다. 새 C 코드는 `cvmclang.ps1`과
> LLVM 기반 경로를 사용한다.

`cvmcc`는 CVM ABI v1용 C 컴파일러의 첫 부트스트랩 단계다. C 소스를 CVM
어셈블리로 내보내거나 어셈블러를 내부 호출해 `CVMOBJ2` 재배치 오브젝트를
직접 만든다.

```powershell
.\build\tools\cvmcc.exe -S input.c -o input.s
.\build\tools\cvmcc.exe -c input.c -o input.o
.\build\tools\cvmlink.exe start.o input.o -o program.cvm `
    --base 0x10000 --entry program_entry
```

매크로나 `#include`가 필요하면 현 단계에서는 호스트 GCC로 freestanding
전처리한 `.i`를 입력한다.

```powershell
gcc -E -ffreestanding -nostdinc input.c -o input.i
.\build\tools\cvmcc.exe -c input.i -o input.o
```

## 현재 지원 범위

- `void`, `char`, `short`, `int`, `long`, `long long`과 signed/unsigned 정수
- 정수 상수, 지역·전역 스칼라 변수, 상수 전역 초기화와 tentative definition
- 단항 `+ - ! ~`, 산술·비트·shift·비교·논리 연산과 단순 대입
- 블록, 선언, 식 문장, `if/else`, `while`, `return`
- 함수 정의·호출과 레지스터 인자 최대 8개
- ABI v1의 `R0`~`R7` 인자, `R0` 반환, `R15` SP, 16바이트 call 정렬
- `.global`, `.type`, `.size`, `.comm` 메타데이터와 재배치 호출

모든 지역은 정확성 우선으로 8바이트 stack slot을 배정하되 타입 폭에 맞는
LOAD/STORE 및 sign/zero extension을 사용한다. `R14`는 함수 내부 frame
pointer로 사용하며 ABI v1에서 caller-saved이므로 외부 규약과 충돌하지 않는다.

## 아직 지원하지 않는 C 기능

포인터·배열, 구조체·union·enum, 문자열, `for`/`switch`, `break`/`continue`,
증감·복합 대입, stack argument, variadic, 함수 포인터, 부동소수점, 전처리기,
최적화와 디버그 정보는 후속 단계다. 현재 컴파일러는 완전한 ISO C 구현이
아니며 지원하지 않는 문법을 진단하고 중단한다.
