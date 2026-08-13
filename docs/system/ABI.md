# RArchM64 ABI v1.0

이 문서는 독립적으로 컴파일·어셈블된 코드가 함께 링크되고 호출될 수 있도록
RArchM64의 C 데이터 모델, 메모리 배치와 함수 호출 규칙을 고정한다. 기계가
명령을 실행하는 규칙은 ISA 문서가, 소프트웨어가 레지스터와 스택을 사용하는
규칙은 이 문서가 담당한다. 기계 판독용 상수는 레거시 이름의
`include/cvm_abi.h`에 있다.

이 문서는 ABI 전체 규격을 정의한다. 현재 `cvmir` bridge가 구현한 C 범위는
정수·포인터 중심의 부분집합이며 floating/vector IR, aggregate SSA 반환 등은
아직 지원하지 않는다. ABI에 규칙이 있다는 사실이 현재 컴파일러 지원을
뜻하지는 않는다. 구현 범위는
[LLVM_IR_TRANSLATOR.md](../tools/LLVM_IR_TRANSLATOR.md)를 따른다.

## 데이터 모델

RArchM64 ABI v1은 64비트 little-endian LP64 모델을 사용한다.

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

reference kernel의 첫 user thread 진입 ABI는 `R0=argc`, `R1=argv`,
`R2=envp`다. 초기 SP는 16바이트 정렬이며 `[SP]`부터 64비트 `argc`, 연속된
`argv` 포인터, NULL, 연속된 `envp` 포인터, NULL 순서로 놓인다. 문자열과
포인터가 가리키는 저장소도 같은 초기 user stack 안에 있다. 추가 thread는
동일한 startup table을 자동 상속하지 않으며 생성자가 별도로 진입 문맥을
지정한다.

권장 syscall ABI는 `R0=번호/반환값`, `R1`~`R6`=인자다. `SYSCALL` 명령은
레지스터 의미를 해석하지 않으며 실제 서비스와 추가 보존 규칙은 게스트
운영체제가 구현한다. 기본 규약에서는 syscall 뒤 `R0`~`R7`과 FLAGS가 바뀔
수 있다.

reference kernel이 현재 사용하는 초기 syscall 번호는 다음과 같다. 성공 시
`R0`에는 0 또는 양의 결과가, 실패 시에는 음수 errno가 반환된다.
공개 상수의 단일 기준은 `include/rarchm64_syscall.h`이며 ABI version은 1이다.

| 번호 | 이름 | 인자 |
|---:|---|---|
| 0 | `exit` | `R1=status` |
| 1 | `write` | `R1=fd`, `R2=buffer`, `R3=size` |
| 2 | `read` | `R1=fd`, `R2=buffer`, `R3=size` |
| 3 | `yield` | 없음 |
| 4 | `getpid` | 없음 |
| 5 | `wait` | `R1=status_address` |
| 6 | `waitpid` | `R1=pid`, `R2=status_address` |
| 7 | `join` | `R1=tid`, `R2=status_address` |
| 8 | `open` | `R1=path`, `R2=flags` |
| 9 | `close` | `R1=fd` |
| 10 | `seek` | `R1=fd`, `R2=signed_offset`, `R3=whence` |
| 11 | `fsync` | `R1=fd` |
| 12 | `exec` | `R1=path`, `R2=argv`, `R3=envp` |
| 13 | `sleep` | `R1=milliseconds` |
| 14 | `display_mode` | `R1=width`, `R2=height`, `R3=display_info_address` |
| 15 | `display_present` | 없음 |

`display_mode`는 XRGB8888 back buffer를 현재 process에 배타적으로 할당하고
48바이트 `RArchM64DisplayInfo`에 user VA, 실제 byte 수, width, height, stride와
format을 차례로 반환한다. width/height는 1 이상 1920x1080 이하이고 stride는
`width * 4`로 kernel이 계산한다. 해상도를 키울 때는 새 연속 물리 buffer와
user mapping을 먼저 준비하며 성공한 뒤에만 이전 buffer를 반환한다. 다른
process가 display를 소유하면 `-EBUSY`, RAM이 부족하면 `-ENOMEM`이다.

user는 반환된 writable back buffer를 채운 뒤 `display_present`를 호출한다.
호출 thread는 장치가 staging front buffer로 복사하고 present-completion IRQ를
보낼 때까지 CPU를 점유하지 않고 대기한다. process 종료 또는 성공한 `exec`은
mapping과 물리 buffer를 반환한다. host window 크기는 이 guest mode와 독립이며
frontend가 guest frame을 host window에 맞춰 표시한다.

`open`의 flags bit 0은 read, bit 1은 write, bit 2는 append이며 최소 read 또는
write 하나가 필요하다. append는 write와 함께 써야 하고 각 write 직전에 현재
파일 끝으로 offset을 옮긴다. 서로 별도로 연 파일은 offset을 공유하지 않는다.
read-only block 장치에서는 write-open을 거부한다. 현재는 존재하는 RMFS
regular file만 열고 새 파일 생성 flag는 아직 없다. kernel 내부 전체 파일
write API는 기존 디렉터리에 파일을 만들 수 있다. `seek`의
whence 0/1/2는 각각 시작/현재 위치/파일 끝 기준이다. 경로는 NUL을 포함해
최대 128바이트 안에서 끝나야 한다. 각 process에는 keyboard FD 0, UART FD 1/2가
기본 설치되며 regular file은 가장 낮은 빈 FD 3 이상을 받는다.

regular file의 `write`는 RMFS transaction group에 변경을 누적하며 `close`는
암묵적으로 동기화하지 않는다. `fsync(fd)`가 성공하면 해당 시점까지 volume에
누적된 data와 metadata가 block 장치에 기록되고 RMFS superblock이 CLEAN으로
확정된다. 현재 구현은 파일별 journal이 아니라 volume 단위 group commit이다.

`read`/`write`는 `buffer + size`의 uintptr overflow와 signed 반환 범위 초과를
먼저 거부하고, 접근하는 모든 user page의 read 또는 write 권한을 검사한다.
경로 복사도 매 바이트 주소 덧셈과 page 권한을 검사하며 NUL 없는 128바이트
경로는 거부한다.

FD 0의 keyboard `read`는 입력이 없으면 호출 thread를 `BLOCKED`로 전환하고
keyboard IRQ가 도착할 때까지 반환하지 않는다. 반환 데이터는 아직 문자나
UTF-8이 아니라 각 event의 8-bit HID usage이며 한 번에 최대 64바이트를
전달한다. regular-file `read`/`write`/`fsync`는 block IRQ를 기다리는 동안
kernel continuation을 보존하고 다른 runnable thread를 실행한다. 장치 오류와
timeout은 `-EIO`로 전달된다.

`exec`의 `argv`와 `envp`는 각각 NULL로 끝나는 user pointer 배열이며 배열
자체를 0으로 넘기면 빈 목록으로 취급한다. 각 목록은 최대 32개, 두 목록의
문자열 저장소 합계는 최대 4096바이트다. kernel은 경로, 포인터 배열과 문자열을
모두 기존 address space에서 먼저 복사하고 새 EXF, address space, user stack을
완전히 준비한 뒤 한 번에 교체한다.

성공하면 syscall 호출 지점으로 돌아오지 않고 새 EXF entry로 진입하며
`R0/R1/R2=argc/argv/envp`와 초기 stack table을 다시 구성한다. PID, 현재 TID,
parent/child 관계와 열린 FD table 및 open-file offset은 유지하고 같은 process의
다른 software thread는 종료한다. 실패하면 기존 image, 실행 문맥, 다른 thread와
FD table을 변경하지 않고 음수 errno를 반환한다. 현재 close-on-exec FD flag는
정의하지 않는다.

안정된 errno 번호는 다음과 같다. syscall은 아래 양수 값을 음수로 바꿔
반환한다.

| 번호 | 이름 | 의미 |
|---:|---|---|
| 2 | `ENOENT` | 경로 없음 |
| 5 | `EIO` | 장치/파일 I/O 실패 |
| 8 | `ENOEXEC` | 실행 파일 형식/적재 검증 실패 |
| 9 | `EBADF` | 잘못된 FD |
| 10 | `ECHILD` | 기다릴 자식 없음 |
| 11 | `EAGAIN` | 지금 완료할 수 없음 |
| 12 | `ENOMEM` | 메모리 부족 |
| 13 | `EACCES` | 접근 mode 위반 |
| 14 | `EFAULT` | 잘못된 user 주소 |
| 16 | `EBUSY` | 이미 사용/대기 중 |
| 22 | `EINVAL` | 잘못된 인자 |
| 24 | `EMFILE` | process FD table 가득 참 |
| 27 | `EFBIG` | 지원 파일/전송 크기 초과 |
| 35 | `EDEADLK` | self-join 등 deadlock |
| 38 | `ENOSYS` | 미구현 syscall |

`wait`는 임의의 직접 자식을 기다리며 `waitpid`는 양의 PID의 직접 자식을
기다린다. 성공하면 자식 PID를 반환하고, `status_address`가 0이 아니면 그곳에
signed 64비트 exit status를 기록한다. 주소 0은 status를 버린다는 뜻이다.
살아 있는 자식은 호출 thread를 `BLOCKED`로 전환하고 종료 시 깨운다. 현재
옵션 인자는 없으며 `WNOHANG`과 process group 대기는 정의하지 않는다. 자식이
없으면 `-ECHILD`, 잘못된 user 주소면 `-EFAULT`를 반환한다.

`join`은 호출자와 같은 process의 다른 software thread가 종료할 때까지 기다린다.
성공 시 대상 TID를 반환하고 선택적인 `status_address`에 signed 64비트 thread
exit status를 기록한다. 대상은 한 번만 join할 수 있다. self-join은 `-EDEADLK`,
이미 waiter가 있는 대상은 `-EBUSY`, 잘못되었거나 이미 수거된 TID는 `-EINVAL`을
반환한다.

`sleep`은 호출 thread를 지정한 밀리초 이상 `BLOCKED`로 만들고 timer IRQ에서
다시 `RUNNABLE`로 전환한다. 0은 즉시 성공한다. 양수 시간은 현재 2ms timer
quantum의 다음 tick으로 올림하며, 대기 중 실행 가능한 thread가 없으면 CPU는
hardware `WAIT` 상태에 들어간다. 반환값은 성공 시 0이다.

IRQ·동기 예외 핸들러는 일반 함수 ABI가 아니다. CPU 예외 프레임은 ISA
문서를 따르며 `IRET` 대상의 레지스터가 필요하면 핸들러가 직접 보존한다.

reference boot ABI는 커널 진입 시 `R0=CvmBootInfo 주소`,
`R1=CVM_BOOTINFO_HANDOFF_MAGIC`을 전달한다. fixed physical 이미지는 MMU OFF,
`CVM_KERNEL_FLAG_RELOCATABLE_PHYSICAL` 이미지는 BootInfo의 초기 PTBR로 MMU가
켜진 상태에서 시작한다. 두 경우 모두 supervisor이고 SP는 16바이트 정렬이다.
