# RISC-VM ABI v1.0

이 문서는 독립적으로 컴파일·어셈블된 코드가 함께 링크되고 호출될 수 있도록
RISC-VM의 C 데이터 모델, 메모리 배치와 함수 호출 규칙을 고정한다. 기계가
명령을 실행하는 규칙은 ISA 문서가, 소프트웨어가 레지스터와 스택을 사용하는
규칙은 이 문서가 담당한다. 기계 판독용 상수는 레거시 이름의
`include/cvm_abi.h`에 있다.

이 문서는 ABI 전체 규격을 정의한다. 현재 `cvmir` bridge가 구현한 C 범위는
정수·포인터 중심의 부분집합이며 floating/vector IR, aggregate SSA 반환 등은
아직 지원하지 않는다. ABI에 규칙이 있다는 사실이 현재 컴파일러 지원을
뜻하지는 않는다. 구현 범위는
[LLVM_IR_TRANSLATOR.md](../tools/LLVM_IR_TRANSLATOR.md)를 따른다.

## 데이터 모델

RISC-VM ABI v1은 64비트 little-endian LP64 모델을 사용한다.

| C 타입 | 크기 | 정렬 |
|---|---:|---:|
| `_Bool`, `char`, `signed char`, `unsigned char` | 1 | 1 |
| `short`, `unsigned short` | 2 | 2 |
| `int`, `unsigned int`, 기본 `enum` | 4 | 4 |
| `long`, `unsigned long`, `long long`, `unsigned long long` | 8 | 8 |
| 포인터, `size_t`, `ptrdiff_t` | 8 | 8 |
| `float` | 4 | 4 |
| `double`, `long double` | 8 | 8 |
| 128비트 SIMD 값 | 16 | 16 |

`char`의 기본 signedness는 signed다. `wchar_t`는 signed 32비트다.
`long double`은 v1에서 `double`과 같은 IEEE-754 binary64이며 확장 정밀도를
제공하지 않는다.

스칼라는 자연 정렬을 사용하고 일반 스칼라 정렬의 최댓값은 8이다. 배열의
정렬은 원소 정렬과 같다. 구조체는 각 멤버를 그 타입의 정렬에 맞춰 배치하며,
구조체 정렬은 가장 큰 멤버 정렬이고 전체 크기는 그 정렬의 배수로 올린다.
union의 크기는 가장 큰 멤버 크기를 union 정렬로 올린 값이다. SIMD 멤버가
있으면 aggregate 정렬은 최대 16까지 올라간다. packed 구조체와 C bit-field
배치는 v1 ABI에 포함하지 않는다.

## 레지스터 규약

- 정수·포인터 인자 1~8은 `R0`~`R7`에 순서대로 전달한다.
- scalar `float`/`double` 및 SIMD 인자 1~8은 `V0`~`V7`에 전달한다.
- 정수·포인터 반환은 `R0`, 두 번째 64비트 반환 슬롯은 `R1`을 사용한다.
- 부동소수점·SIMD 반환은 `V0`, 두 번째 슬롯이 필요하면 `V1`을 사용한다.
- `R15`는 `SP`다. 함수는 반환할 때 호출 전 호출자의 `SP`를 복원해야 한다.
- ABI v1에서는 `R0`~`R14`, `V0`~`V15`, FLAGS가 모두 caller-saved다.
  callee-saved 범용·벡터 레지스터는 없다.

8/16/32비트 정수 인자는 signed 타입이면 64비트 sign-extension, unsigned
타입이면 zero-extension한 뒤 GPR에 둔다. 반환값도 같은 canonical 확장을
사용한다. 함수 포인터는 다른 포인터와 같은 64비트 코드 주소다.

모든 레지스터를 caller-saved로 정한 것은 초기 컴파일러와 어셈블리 경계를
단순하게 만들기 위한 v1의 의도적인 선택이다. 컴파일러는 호출 뒤에도 필요한
값을 스택에 spill해야 한다.

## 스택과 호출 프레임

스택은 낮은 주소 방향으로 자라며 red zone은 없다. 호출자는 `CALL`, `CALLR`,
`CALLREL` 실행 직전 `SP`를 16바이트 경계에 맞춰야 한다. 명령이 8바이트
반환 주소를 push하므로 피호출자 진입 직후에는 `SP % 16 == 8`이다.

피호출자가 다시 함수를 호출하려면 프롤로그와 로컬 프레임을 포함해 호출 직전
`SP % 16 == 0`으로 만들어야 한다. `RET`은 `[SP]`의 반환 주소를 PC로 읽고
`SP`를 8 증가시킨다. frame pointer는 필수가 아니며 필요하면 일반 레지스터를
사용한다. v1은 고정 frame-pointer 레지스터를 예약하지 않는다.

GPR/V 레지스터에 들어가지 않는 인자는 호출자가 오른쪽에서 왼쪽 순서로
스택에 배치한다. 각 일반 스택 인자 슬롯은 최소 8바이트이며 16바이트 타입은
16바이트 정렬한다. CALL이 반환 주소를 push한 뒤 피호출자의 첫 스택 인자는
`[SP+8]`에 있다. 호출자가 스택 인자와 정렬 padding을 회수한다.

## aggregate 전달과 반환

- 크기 1~8바이트 aggregate는 한 GPR 슬롯에 little-endian으로 전달·반환한다.
- 크기 9~16바이트 aggregate는 연속된 두 GPR 슬롯에 낮은 주소 부분부터
  전달하며 `R0:R1`로 반환한다.
- 16바이트를 초과하거나 정렬이 16보다 큰 aggregate는 메모리로 전달한다.
- 16바이트 초과 반환값은 호출자가 결과 공간을 만들고 숨은 첫 인자로 그
  포인터를 `R0`에 전달한다. 명시적 정수 인자는 `R1`부터 시작하며 피호출자는
  같은 결과 포인터를 `R0`으로 반환한다.
- aggregate를 레지스터에 넣을 때 사용하지 않는 상위 바이트는 0으로 만든다.

## 가변 인자

고정 매개변수는 일반 규칙을 따른다. `...`에 해당하는 모든 unnamed 인자는
레지스터가 남아 있어도 스택으로 전달한다. default argument promotion을 먼저
적용하고 각 값은 최소 8바이트 슬롯을 사용한다. 16바이트 값은 16바이트
정렬한다. 따라서 `va_list`는 다음 unnamed 스택 인자를 가리키는 64비트
포인터 하나로 표현할 수 있다. 가변 함수 호출에서도 CALL 직전 16바이트 스택
정렬 규칙은 유지한다.

## 전역 심볼과 링크

함수는 `.type name, function`, 객체는 `.type name, object`로 표시하며
`.size name, expression`으로 크기를 기록한다. `.comm`은 tentative 전역
객체다. 같은 이름의 common이 여러 개면 링커가 가장 큰 크기와 정렬을
선택하고, strong 정의가 있으면 strong 정의가 common을 대체한다.

strong 정의가 weak 정의보다 우선하며 여러 strong 정의는 링크 오류다.
여러 weak 정의만 있으면 링크 입력 순서상 첫 정의를 사용한다. 해석되지 않은
weak 심볼 주소는 0이다. ABI v1 실행 이미지는 정적 절대 주소 모델이며
PIC/GOT/PLT/TLS는 정의하지 않는다.

## syscall, 예외와 부팅 경계

권장 syscall ABI는 `R0=번호/반환값`, `R1`~`R6`=인자다. `SYSCALL` 명령은
레지스터 의미를 해석하지 않으며 실제 서비스와 추가 보존 규칙은 게스트
운영체제가 구현한다. 기본 규약에서는 syscall 뒤 `R0`~`R7`과 FLAGS가 바뀔
수 있다.

IRQ·동기 예외 핸들러는 일반 함수 ABI가 아니다. CPU 예외 프레임은 ISA
문서를 따르며 `IRET` 대상의 레지스터가 필요하면 핸들러가 직접 보존한다.

reference boot ABI는 커널 진입 시 `R0=CvmBootInfo 주소`,
`R1=CVM_BOOTINFO_HANDOFF_MAGIC`을 전달한다. fixed physical 이미지는 MMU OFF,
`CVM_KERNEL_FLAG_RELOCATABLE_PHYSICAL` 이미지는 BootInfo의 초기 PTBR로 MMU가
켜진 상태에서 시작한다. 두 경우 모두 supervisor이고 SP는 16바이트 정렬이다.
