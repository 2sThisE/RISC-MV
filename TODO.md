# RISC-MV 개발 로드맵

이 문서는 현재 구현 상태에서 실제로 남은 작업을 선행 관계와 위험도 순으로
관리한다. 완료된 세부 작업의 전체 이력보다는 현재 기준점, 다음 구현 단계와
각 단계의 완료 조건을 기록한다.

우선순위 표기는 다음과 같다.

- `P0`: 빌드, 부팅 또는 데이터 안전성을 막는 즉시 수정 항목
- `P1`: 현재 주력 단계. 사용자 프로그램을 실행하는 운영체제 기반
- `P2`: 도구체인, 성능, 개발 편의성 고도화
- `P3`: 장기 운영체제 기능과 셀프호스팅

## 현재 기준점 — 완료

### RISC-MV와 RArchM64 가상 하드웨어

- [x] RISC-MV ISA 계열과 64-bit little-endian `RArchM64` 아키텍처
- [x] `RArchM64`의 256개 opcode 공간
- [x] 정수, 부호 연산, 분기, call/return, atomic/fence, float와 SIMD 기초
- [x] privilege mode, 4KiB/2MiB/1GiB MMU leaf, page table, 예외, VBR,
  syscall과 interrupt return
- [x] timer, IRQ controller, IPI, core-control과 가상 multicore/hardware-thread
- [x] RAM/ROM/MMIO bus, VIO hub와 동적 장치 module ABI
- [x] UART, block, keyboard와 XRGB8888 framebuffer display 장치

### 부팅과 실행 포맷

- [x] Boot ROM -> `BOOT.EXF` -> `KERNEL.EXF` 2단계 부팅
- [x] GPT의 40 MiB FAT32 boot partition + RMFS system partition과
  firmware service/BootInfo handoff
- [x] 고정 kernel VA, 동적 물리 배치, RAM direct-map과 MMU-on 진입
- [x] 임시 page table과 bootloader/staging 메모리의 kernel 회수
- [x] RISC-MV EXF v1: `.exf`, `RMVEXF01`, `RA64`
- [x] 기존 `CVMKERN1`, `CVM1`, `.cvm` 실행파일 비호환 처리
- [x] FAT cache, FSInfo next-free hint, 연속 sector batch I/O와 event 기반 host
  block worker

### 빌드와 도구체인

- [x] assembler, relocatable `.o`, linker, static `.a`와 archive 추출
- [x] 다중 `.text/.rodata/.data/.bss` segment와 W^X EXF 생성
- [x] RArchM64 ABI v1.0, LP64, 16-byte stack alignment와 scalar stack argument
- [x] Clang 22 -> LLVM IR -> `cvmir` -> object/linker cross-build 경로
- [x] `rarchm64-unknown-none` triple, sysroot, `crt0.o`, builtins와 `libcvm.a`
- [x] 전체 build script, 35-suite test runner와 실제 boot 회귀 경로

### reference kernel P1.2

- [x] C kernel entry, BootInfo 검증, 범위 기반 지연 PMM, MMU, heap과 기본 자료구조
- [x] 독립 user address space, EXF 검증/적재와 user/kernel 권한 분리
- [x] syscall dispatcher와 page-aware user pointer copy/검증
- [x] timer IRQ 기반 전체 문맥 교환과 단일 코어 선점 스케줄링
- [x] RMFS VFS 읽기/쓰기와 `/KTEST.TXT` 내용 검증
- [x] UART, block, keyboard와 display의 최소 kernel driver

현재 scheduler는 동적 `KernelProcess`/`KernelThread`와 intrusive runnable queue로
3개 process, 4개 thread를 선점 실행한다. user fault는 해당 process에 격리하고
부모 없는 zombie는 안전한 다음 trap에서 수거한다. parent/child와 process
`wait`/`waitpid`, thread `join`의 `BLOCKED -> RUNNABLE` wakeup은 구현됐다.
process별 file descriptor와 transactional `exec`도 구현됐으며 일반 wait
queue/idle과 SMP kernel scheduler는 아직 없다.

## P0 — 현재 차단 항목

현재 알려진 P0 차단 항목은 없다. 새 P0가 생기면 아래 P1 작업보다 먼저 해결하고
재현 테스트를 함께 추가한다.

## P1.3 — 프로세스와 사용자 공간

P1.3은 아래 순서를 따른다. 뒤 단계가 앞 단계의 임시 구조를 다시 뜯지 않도록
Process/Thread 소유권과 scheduler 상태 모델을 먼저 확정한다.

### P1.3-A — Process/Thread 기반과 동적 scheduler — 완료

- [x] 고정 `KernelTask[2]`를 `KernelProcess`와 `KernelThread`로 분리
- [x] Process가 PID, address space, user image와 thread 목록을 소유하고 파괴
- [x] Thread가 TID, GPR/PC/FLAGS/SP, kernel stack과 scheduling 상태를 소유
- [x] `NEW -> RUNNABLE -> RUNNING -> ZOMBIE` 전이와 부모 없는 process의
  deferred reap에서 thread `DEAD` 처리 및 객체 수거
- [x] 고정 배열 대신 intrusive runnable queue와 동적 PID/TID 할당기 사용
- [x] 같은 Process의 Thread가 PTBR을 공유하고 각자 user/kernel stack을 사용
- [x] 주소 공간 변경 시에만 PTBR을 교체하도록 문맥 교환 경계 정리
- [x] `thread_create`, `thread_exit`, `yield`에 해당하는 kernel 내부 경로 구현
- [x] 기존 user syscall/선점/종료 자체 검사를 새 구조에서 통과
- [x] 3개 process와 한 process의 2개 thread를 함께 선점하는 실제 부팅 테스트

완료 조건: 고정 task 개수 없이 단일 코어에서 process와 software thread를
동적으로 생성하고 선점할 수 있어야 한다.

parent/child와 wait 가능한 zombie는 P1.3-B, process별 FD table은 P1.3-D,
일반적인 `BLOCKED -> RUNNABLE` wait queue와 idle은 P1.3-E에서 확장한다.

### P1.3-B — 수명 관리와 user fault 격리 — 완료

- [x] parent/child reference와 wait 가능한 zombie의 소유권 규칙 정의
  (부모 없는 process는 deferred reap, 부모 있는 zombie는 wait까지 보존)
- [x] thread 종료와 마지막 thread 종료를 process zombie 전환으로 연결
- [x] parent/child 관계, signed 64-bit exit status, zombie와 `wait`/`waitpid` 구현
- [x] process wait 대상 대기를 위한 `BLOCKED -> RUNNABLE` 전이와 wakeup 구현
- [x] thread `join`, 이미 종료된 대상 수거와 현재 stack 이탈 뒤 deferred reap
- [x] 부모 종료 시 orphan 자동 수거와 중복 process wait의 `-ECHILD` wakeup
- [x] self/중복 thread join 방지와 잘못된 TID·user pointer 오류 처리
- [x] user page fault, illegal instruction과 privilege fault를 현재 process 종료로 전달
- [x] kernel mode fault와 손상된 kernel 상태는 기존 panic 유지
- [x] address space, page, EXF image와 user/kernel stack이 정확히 한 번
  회수되는지 8회 반복 생성/종료와 PMM free-page 기준값으로 검사
  (FD 수명 검사는 P1.3-D에서 추가)
- [x] 한 user process가 fault로 종료돼도 다른 process가 계속 실행되는 회귀 테스트

완료 조건: 잘못된 user EXF가 kernel이나 다른 process를 종료시키지 않고 모든
자원이 수거되어야 한다.

### P1.3-C — VFS 기반 EXF `exec`와 초기 userland

- [x] VFS 경로에서 크기 상한과 exact-size 할당으로 EXF 전체를 읽는 공통
  user-image/process loader API
- [x] `RMVEXF01`, `RA64`, version, CRC32, segment 범위·중첩과 W^X 재검증
- [x] 새 Process/address space와 초기 Thread 완성 뒤 검증된 상태만 원자적으로
  runnable 등록
- [x] loader/thread 생성 실패와 queue 등록 중간 실패에서 page, process,
  thread와 scheduler counter를 전부 rollback
- [x] 초기 user stack에 16-byte aligned `argc`, `argv[]`, NULL, `envp[]`, NULL
  테이블을 만들고 R0/R1/R2에도 `argc/argv/envp`를 전달하는 ABI 정의·검사
- [x] kernel이 `/BIN/INIT.EXF`를 찾아 PID 1 첫 user process로 시작하는 경로
- [x] 부모가 먼저 종료된 process를 살아 있는 PID 1 init으로 재부모화하고,
  init 종료 뒤 부모 없는 zombie는 자동 회수하는 정책
- [x] `vmkdisk --init INIT.exf` 입력으로 user EXF를 RMFS
  `/BIN/INIT.EXF`에 패키징하고 검사하는 input 규격
- [x] 디스크 init 정식화 뒤 내장 wait/join user image 생성기를 링크에서 제거하고
  loader/lifetime/fault용 최소 회귀 image만 유지
- [x] 손상 CRC, 잘못된 ISA, 실제 중첩 segment와 W^X 위반 EXF 거부 테스트

완료 조건: kernel을 다시 빌드하지 않고 가상디스크의 user EXF를 교체해 실행할
수 있어야 한다.

### P1.3-D — 프로세스별 file descriptor와 파일 syscall

- [x] VFS vnode/open-file handle과 process별 32-slot FD table의
  소유권/참조 횟수·process 종료 정리 정의
- [x] 새 process의 FD 0을 keyboard input, FD 1/2를 UART console
  pseudo-vnode에 연결
- [x] FD table 기반 `open`, `close`, `read`, `write`, `seek`, `fsync` syscall과
  `/BIN/INIT.EXF`와 `/KTEST.TXT`의 실제 RMFS 파일 왕복 회귀
- [x] open-file별 독립 offset, append-at-write, read/write flag와
  read-only block 장치의 write-open 거부 규칙
- [x] path 128-byte NUL bound, user buffer address+size overflow,
  signed 반환 범위와 page별 read/write 권한 검증
- [x] `include/rarchm64_syscall.h`를 syscall 번호/open flag/seek/fsync/errno의
  ABI v1 단일 기준으로 추가하고 ABI 문서와 동기화
- [x] process 종료 시 모든 FD slot 참조를 닫고 vnode/open-file을 최종 회수
- [x] in-place `exec`가 새 EXF와 argc/argv/envp stack을 먼저 완성한 뒤 image를
  교체하고, 성공 시 PID/TID/FD를 유지하며 실패 시 image/thread/FD를 원상 유지
- [x] 서로 다른 process의 FD table 격리와 중복 close/NULL user pointer를
  실제 init syscall로 거부하는 회귀 테스트

완료 조건: Clang으로 빌드한 user EXF가 syscall만으로 가상디스크 파일을 열고
읽고 쓰고 닫을 수 있어야 한다.

### P1.3-E — sleep/wakeup과 interrupt 기반 비동기 I/O

- [ ] scheduler wait queue, wake-one/wake-all과 timeout primitive
- [ ] timer 기반 `sleep`과 BLOCKED thread가 없을 때의 idle/WAIT 경로
- [ ] block driver의 early-boot polling과 scheduler 이후 IRQ mode 분리
- [ ] block 완료 IRQ에서 요청별 waiter를 깨우고 오류를 호출자에게 전달
- [ ] keyboard IRQ ring buffer와 blocking `read`
- [ ] interrupt context에서 할당/수면하지 않는 IRQ-safe queue와 lock 규칙
- [ ] IRQ-before-sleep, timeout-vs-completion과 wakeup 유실 경쟁 테스트
- [x] init 통합 실패를 `A`~`Z` 단계 코드로 분리하고 동일 persistent image
  반복 부팅으로 wait/join, 파일 syscall과 exec rollback/FD 유지 경로 확인

완료 조건: I/O를 기다리는 thread가 CPU를 polling하지 않고 다른 runnable
thread가 계속 실행되어야 한다.

### P1.3-F — display 사용자 API와 가변 framebuffer

- [ ] host window 크기와 guest framebuffer 해상도를 분리한 mode API
- [ ] checked `width * height * 4`, page roundup와 최대 1920x1080 검증
- [ ] PMM contiguous allocation, scatter/gather 또는 DMA bounce 중 정책 확정
- [ ] 해상도 변경 시 새 buffer 성공 후 이전 buffer를 교체·반환
- [ ] process별 framebuffer mapping 권한과 사용자 `present` syscall/API
- [ ] front/back buffer와 present-completion IRQ 기반 대기
- [ ] 잘못된 stride, 작은 RAM, 반복 mode 변경과 process 종료 회귀 테스트

완료 조건: user process가 선택한 유효 해상도로 화면을 출력하고 buffer를
누수 없이 교체할 수 있어야 한다.

### P1.3-G — RMFS/VFS 기반 — 완료

- [x] FAT32를 firmware/bootloader 전용으로 한정하고 별도 GPT system
  partition을 RMFS v1로 포맷
- [x] 4KiB block, 256-byte inode/directory entry, 239-byte 이름과 6개 inline
  extent 온디스크 규격
- [x] inode/block bitmap, primary/backup superblock과 metadata CRC32
- [x] `/`, `/BIN`, `/BIN/INIT.EXF` 초기 volume과 host formatter/inspector
- [x] kernel RMFS mount, 경로 탐색, range/whole read, regular file 생성·교체,
  FD 부분 write/append와 flush
- [x] volume 크기 기반 동적 inode 수, 64 GiB disk image와 16 MiB 파일 제한 제거
- [x] 최대 6개 조각 extent allocator, 디렉터리 data block 자동 확장과
  4KiB scratch 기반 부분 덮어쓰기
- [x] mount 수명 bitmap cache, 변경 bitmap block만 기록, table CRC32와
  DIRTY 이후 I/O 실패 시 강제 unmount
- [x] 새 data 선기록, DIRTY/CLEAN state, inode 교체 뒤 이전 extent 반환 순서
- [x] RMFS 전역 lock과 block DMA bounce-page lock
- [x] 4KiB scratch를 kernel stack 밖에서 재사용하고 전체 BSS를 MMU RW로 매핑
- [x] GPT/FAT/RMFS/EXF 손상 검사 unit test와 실제 init read/write boot 회귀
- [ ] `mkdir`, `truncate`, `unlink`, `rename`
- [ ] uid/gid/mode 권한 강제, timestamp와 link count 정책
- [ ] 외부 extent tree와 디스크 부족/고조각화 stress test
- [x] 변경된 file block만 교체하는 block-granular COW, extent split/merge와
  EOF 이후 hole zero-fill 회귀
- [x] 64-entry metadata block cache, explicit `fsync`와 DIRTY-state 기반
  transaction group commit(32-write 자동 commit 포함)
- [ ] 온디스크 metadata journal과 replay 기반 group commit 복구
- [ ] journal 또는 copy-on-write recovery와 DIRTY volume fsck/replay 도구

완료 조건: 기본 system volume이 FAT32 8.3 제약 없이 RMFS에서 user EXF와
regular file을 읽고 생성·교체할 수 있어야 한다. 일반 파일시스템 관리 명령과
전원 차단 복구는 위 후속 항목으로 확장한다.

### P1.3 전체 완료 조건

- [x] `/BIN/INIT.EXF`가 부팅 후 PID 1 첫 process로 실행
- [x] 여러 process와 process 내부 여러 thread가 선점 실행
- [x] user fault가 해당 process에만 격리
- [ ] file/keyboard/block I/O가 FD와 blocking syscall로 동작
- [x] process 종료 후 현재 PMM page, heap과 process/thread wait 상태 누수 없음
- [x] process 종료와 exec에서 FD 참조/offset 소유권 유지 및 정리
- [ ] 일반 wait queue 도입 뒤 waiter/timeout 자원 누수 없음
- [x] 현재 단계 전체 unit test와 실제 GPT/FAT32+RMFS boot regression 통과

## P1.4 — SMP kernel과 hardware-thread 활용

P1.3은 먼저 Core 0/Thread 0에서 완성한다. 그 뒤 VM에 이미 있는 가상
core/hardware-thread 기능을 kernel scheduler에 연결한다.

- [ ] Core Control MMIO로 secondary hardware thread를 명시적으로 online
- [ ] per-CPU current thread, kernel stack, scheduler와 interrupt 상태
- [ ] global queue 또는 per-CPU run queue 정책과 load balancing
- [ ] scheduler/PMM/heap/VFS/FD/device 상태의 SMP lock 계층과 순서 정의
- [ ] IPI reschedule, stop과 TLB shootdown protocol
- [ ] IRQ affinity/routing과 timer tick의 core별 책임
- [ ] 같은 core의 hardware thread와 다른 core의 병렬 실행 차이 문서화
- [ ] 2C1T, 2C2T, 최대 구성과 lock contention stress test

완료 조건: 둘 이상의 가상 core가 동일 RAM과 kernel을 안전하게 공유하며 서로
다른 runnable thread를 실제 host 병렬성으로 실행해야 한다.

## P1.5 — 최소 userland와 C runtime

- [ ] syscall number/header와 user용 wrapper library 분리
- [ ] `errno`, `string`, `ctype`, 기본 `malloc/free`와 startup 종료 경로
- [ ] FD 기반 최소 `stdio`와 line-buffered console
- [ ] `/BIN/INIT.EXF`, 간단한 shell과 `echo`, `cat`, `ls` 수준 도구
- [ ] user EXF와 일반 파일을 system image에 패키징하는 build manifest
- [ ] kernel/user ABI version 불일치 진단

이 단계는 ISO C/POSIX 전체 호환을 주장하지 않는다. 필요한 API를 작은 범위로
정의하고 회귀 테스트와 함께 확장한다.

## P2 — 도구체인과 개발 환경

### P2-A — Clang IR 변환 범위와 최적화

- [ ] indirect call, `switch`와 memory intrinsic lowering
- [ ] float/vector IR과 scalar/vector cast lowering
- [ ] variadic callee `va_start/va_arg`와 aggregate SSA return
- [ ] RArchM64 integer legalization 뒤 `-O1` 이상 IR 허용
- [ ] 최적화 단계별 differential execution test

### P2-B — object/linker/archive

- [ ] `.text.*`, `.rodata.*` 사용자 section과 section garbage collection
- [ ] COMDAT/link-once, symbol visibility와 weak/common 규칙 보강
- [ ] 필요가 확인될 때 ABS8/ABS16과 추가 PC-relative relocation 도입
- [ ] `cvmar` member 추가·교체·삭제와 개발용 thin archive
- [ ] link map에 local symbol, type, size와 archive 출처 표시
- [ ] PIC, GOT/PLT, TLS와 동적 loader는 user process 기반 이후 설계

### P2-C — debugger와 진단

- [ ] line table, DWARF subset 또는 RISC-MV 전용 debug 정보 결정
- [ ] register/memory/MMU/exception/device 상태를 읽는 debugger protocol
- [ ] breakpoint, single-step, watchpoint와 process/thread 선택
- [ ] EXF build ID와 symbol/map 자동 연결
- [ ] kernel panic, user fault와 scheduler trace 수집

### P2-D — native LLVM backend 재평가

- [ ] LLVM source tree의 native `rarchm64` TargetInfo와 builtin
- [ ] instruction selection, register info, MC object writer와 assembler
- [ ] LLD port 비용과 현재 IR bridge 대비 빌드/실행 성능 측정
- [ ] 유지보수 비용이 이점보다 작을 때만 기본 backend로 전환

### P2-E — 도구 hardening

- [ ] EXF/object/archive/disk parser fuzzing
- [ ] table overlap, integer overflow와 손상 파일 corpus
- [ ] weak/common/archive 순서와 relocation overflow 독립 통합 테스트
- [ ] reproducible build와 build ID 정책 보강

## P3 — 장기 운영체제와 셀프호스팅

- [ ] `fork` 또는 spawn 중심 process 생성 정책과 copy-on-write 재평가
- [ ] pipe, signal/event, shared memory와 process IPC
- [ ] mount table, 추가 filesystem과 block cache/writeback
- [ ] dynamic loader와 shared library가 필요해질 때 ABI/EXF 확장
- [ ] network device와 최소 network stack
- [ ] user 권한, credential와 파일 접근 제어
- [ ] 충분한 libc/userland 뒤 Clang/LLVM 자체를 RISC-MV용으로 cross-build
- [ ] native build가 가능해진 뒤 self-hosting 범위와 재현성 검증

## ABI v2 후보 — ABI v1과 섞지 않음

- [ ] C bit-field 저장 단위와 배치 규칙
- [ ] `_Atomic` 타입과 C memory order를 ISA atomic/fence에 매핑
- [ ] callee-saved register 도입 여부를 실제 workload로 측정
- [ ] binary128 또는 별도 `long double` 형식
- [ ] vector aggregate와 homogeneous floating aggregate 전달
- [ ] stack unwinding과 예외 처리 metadata

ABI v1과 호환되지 않는 변경은 문서만 수정하지 않는다. ABI major, EXF 요구
version, object metadata, tool 진단과 호환성 테스트를 함께 변경한다.

## 유지·폐기·보류한 방향

- 기존 `Cvm*`, `CVM_*`, `cvmclang`, `cvmir`, `cvmlink`, `cvmar`, `libcvm.a`는
  레거시 source/tool 이름으로 유지한다. 새 public 명칭은 RISC-MV/EXF를 사용한다.
- `.cvm` 실행파일, `CVMKERN1`과 `CVM1`은 다시 지원하지 않는다.
- 자체 C compiler `cvmcc`는 지원과 유지보수를 종료한다. 기능 추가, 버그 수정,
  ABI/ISA 변경 추적 또는 배포 계획은 없으며 기존 lexer/parser/example/test
  소스만 현재 상태로 동결 보존한다.
- RISC-V 호환 ISA port는 현재 RArchM64 ABI와 섞지 않고 별도 장기 project로
  평가한다.
- 무작위 kernel 물리 배치와 ASLR은 현재 필수 항목이 아니다. 보안 모델과 entropy,
  relocation 비용을 정의한 뒤 별도 제안으로 검토한다.

## 바로 시작할 작업

1. P1.3-E scheduler wait queue와 wake-one/wake-all primitive 구현
2. timer 기반 `sleep`과 runnable thread가 없을 때 idle/`WAIT` 경로 구현
3. block driver를 early-boot polling과 scheduler 이후 IRQ mode로 분리
4. keyboard IRQ ring buffer와 blocking `read` 구현
