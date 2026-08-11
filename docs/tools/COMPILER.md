# 폐기된 자체 C 컴파일러 (`cvmcc`)

`cvmcc`는 더 이상 지원하거나 유지보수하지 않는다. 기능 추가, 버그 수정,
RArchM64 ABI·ISA 변경 추적과 배포 계획도 없다. 기존 구현과 관련 테스트는
개발 이력과 과거 회귀 참고를 위해 현재 상태 그대로 보존한다.

기본 빌드는 `cvmcc.exe`를 생성하지 않고 `compiler` 예제도 빌드 대상으로
노출하지 않는다. 따라서 아래 내용은 사용할 도구의 설명이 아니라 동결된
구현의 역사적 기능 기록이다. 새 C 코드는
[LLVM IR 변환 경로](LLVM_IR_TRANSLATOR.md)의 `cvmclang.ps1`을 사용한다.

## 동결 당시 구현 범위

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

## 구현되지 않은 C 기능

포인터·배열, 구조체·union·enum, 문자열, `for`/`switch`, `break`/`continue`,
증감·복합 대입, stack argument, variadic, 함수 포인터, 부동소수점, 전처리기,
최적화와 디버그 정보는 구현되지 않았다. 이 목록을 확장할 계획은 없으며,
현재 소스는 완전한 ISO C 구현이나 지원되는 RISC-MV 컴파일러로 간주하지
않는다. `test_runner`의 `compiler` suite는 보존된 구현의 기존 동작을
기록하는 회귀 검사일 뿐 지원 또는 유지보수 약속이 아니다.
