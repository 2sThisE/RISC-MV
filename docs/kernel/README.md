# RISC-MV reference kernel

이 폴더는 펌웨어와 부트로더 다음에 실행되는 RISC-MV 기준 커널의 시작점이다.
현재 커널은 다음 초기화 경로를 실제로 수행한다.

1. C로 작성된 초기 bootstrap이 `CvmBootInfo` handoff와 CRC32를 검증하고
   early UART를 설정한다.
2. `CVM_MEMORY_USABLE` 항목을 범위 커서로 등록한다. 초기화할 때 RAM 전체를
   건드리지 않고, 새 페이지는 범위에서 지연 할당하며 반환된 페이지는 별도
   재사용 free-list에 넣는다.
3. `0x100000000 + physical` direct-map으로 실제 할당된 4KiB 페이지만 0으로
   초기화하고 할당과 반환을 자체 검사한다.
4. 부트로더의 MMU-on handoff를 검증한 뒤 자체 3단계 페이지 테이블을 만든다.
   커널 text는 RX, rodata는 R, data와 전체 BSS/stack은 RW,
   direct-map은 RW/NX다.
   direct-map은 정렬과 남은 길이가 허용하는 가장 큰 1GiB/2MiB/4KiB leaf를
   순서대로 선택하며 나머지 커널 매핑은 기본적으로 4KiB leaf를 사용한다.
5. 새 PTBR로 교체하고 부트로더가 만든 임시 page-table arena를 회수한다.
6. PMM에서 물리 페이지를 할당해 77-entry VBR 테이블을 설치한다. 동기 예외
   1~11은 공통 trap-frame handler로 들어가며 supervisor의 복구 불가능한
   예외는 panic, user 예외는 현재 process 종료로 분리한다.
7. 의도적인 `LOAD_PAGE_FAULT`에서 모든 GPR을 보존하고 누락 페이지를
   매핑한 뒤 `IRET`으로 실패한 LOAD를 재실행한다.
8. NULL 읽기, rodata 쓰기, data 페이지 실행을 실제로 시도해 각각
   page-not-present, store permission, instruction permission fault인지 확인한다.
9. System Information/Core Control을 supervisor alias로 매핑한다. 논리 CPU가
   둘 이상이면 `0x1E000`의 MMU-off trampoline으로 설정된 모든 secondary를
   시작한다. CPU별 16KiB stack, PTBR, VBR을 설치해 MMU-on C 진입점과 hardware
   `WAIT`까지 도달시키고, 전용 IPI 48로 실제 wake/handler/복귀를 확인한다.
   secondary는 커널 종료 전까지 idle online 상태를 유지하고 종료 경로가 모두
   `STOP`/`HALTED`를 확인한 뒤 stack을 회수한다.
10. direct-map 뒤의 동적 가상주소에 물리 페이지를 매핑해 16바이트 정렬 kernel
   heap을 구성하고 allocation 분할·병합·재사용과 다중 페이지 할당을 검사한다.
11. intrusive list, 동기화 byte queue, bitmap과 spinlock 런타임을 초기화하고
    자체 검사를 수행한다.
12. supervisor kernel mapping을 유지하는 독립 PTBR을 만들고 RISC-MV `.exf`의 CRC,
    주소 범위, segment 중첩과 W^X를 검증해 user RX/R/RW page와 64KiB stack을
    적재한다. guard page와 supervisor mapping의 user 접근 차단도 검사한다.
13. VIO hub에서 block/키보드/display 장치를 찾고 supervisor MMIO alias로
    매핑한다. GPT의 RMFS system partition을 `/`로 마운트해 긴 이름 경로,
    파일 읽기, 생성, 교체, append와 flush를 검사한다. FAT32 boot partition은
    firmware/bootloader 전용으로 유지한다.
14. syscall vector 76에 dispatcher를 연결하고 page별 user 권한을 먼저
    검증하는 `copy_from_user`/`copy_to_user`로 잘못된 포인터와 overflow를
    fault 없이 거부한다. 초기 ABI는 `exit`, FD 기반 `write`/`read`, `yield`,
    `getpid`, `wait`, `waitpid`, `join`, `open`, `close`, `seek`, `fsync`, `exec`,
    timer 기반 `sleep`, `display_mode`와 `display_present`다.
15. `/BIN/INIT.EXF`를 읽어 PID 1 process로 만들고, 동적
    `KernelProcess`/`KernelThread` 객체로 총 2개 process와 3개 thread를 만든다.
    init의 두 thread는 PTBR을 공유하되 각자 64KiB user stack과 64KiB
    kernel stack을 사용한다. timer IRQ 0에서 전체 GPR, PC, FLAGS와 SP를
    저장·교체하며, 주소 공간이 바뀔 때만 PTBR을 교체한다. 모든 syscall 출력,
    종료와 timer 선점이 관측되어야 한다. 한 process는 의도적으로 NULL user
    load page fault를 일으키며, 해당 process만 종료되고 나머지가 계속 실행해야
    한다. fault process는 init의 자식으로 구성한다. init의 `waitpid`는
    자식이 종료할 때까지 `BLOCKED`가 되고, 자식의 fault exit status를 user
    공간에 복사한 뒤 `RUNNABLE`로 깨어나 자식을 수거한다. 부모 없는 zombie는
    reap queue에 들어간 뒤 다음 thread의 trap에서 address space, user page와
    user/kernel stack을 해제한다. 같은 process의 worker thread는 `join`으로
    기다리고, 종료 뒤 user stack page와 kernel stack을 deferred reap한다.
    최초 thread의 stack에는 16바이트 정렬된 argc/argv/envp 테이블을 만들고
    R0/R1/R2에도 같은 진입 인자를 전달한다. 별도 수명 검사는 parent/child
    process와 여러 thread를 8회 생성·파괴하며
    PMM free-page 수가 기준값으로 정확히 돌아오는지 확인한다. 이 검사가 모두
    성공해야 `KERNEL: READY`를 출력한다.

`exec`는 새 EXF와 argc/argv/envp stack을 별도 address space에 먼저 완성한 뒤
현재 process image를 교체한다. 성공 시 PID/TID와 FD table을 보존하고 다른
software thread를 정리하며, 실패 시 기존 image/thread/FD 상태로 계속 실행한다.
init 회귀는 존재하지 않는 파일, 비-EXF 파일과 잘못된 user pointer 실패 뒤에도
계속 실행되는지 검사하고, self-exec 뒤 전달된 argv/envp와 기존 FD 3의 offset 및
읽기를 확인한다.

init 통합 검사가 실패하면 더 이상 모든 원인을 `ARGS ERROR`로 합치지 않고
`INIT: ERROR X`를 출력한다. `A/B`는 argc/argv, `C/D`는 waitpid 결과/status,
`E/F`는 join 결과/status, `G`~`Q`는 파일 syscall, `R`~`V`는 exec 실패
rollback, `W`~`Z`는 새 image의 argv/envp와 FD 유지 단계다. 이 코드는 반복
부팅에서 최초 실패 지점을 보존하기 위한 회귀 진단 ABI이며 정상 출력은
`INIT: EXEC/FD OK`다. `a`~`h`는 32x32 mode/present, 64x64 buffer 교체와
present, 최소 RAM에서 1920x1080 할당 실패, 잘못된 mode 거부 뒤 기존 buffer
재사용을 검사하는 display 단계다.

빌드는 프로젝트 루트에서 실행한다.

```powershell
.\build.ps1 -e boot
```

전체 빌드 또는 `boot` 예제 빌드는 다음 소스를 각각 오브젝트로 만든 뒤
legacy 이름의 `cvmlink`로 `kernel.exf`와 `kernel.map`을 생성한다.

- `kernel_main.c`: BootInfo 검증, UART와 전체 부팅 순서
- `pmm.c`: 범위 기반 지연 물리 페이지 할당과 반환 페이지 재사용 free-list
- `mmu.c`: 4KiB/2MiB/1GiB leaf를 지원하는 3단계 페이지 테이블과 MMU 전환
- `heap.c`: 동적 가상주소 기반 kernel heap
- `runtime.c`: list, byte queue, bitmap과 spinlock
- `address_space.c`: 프로세스별 page table, user page 매핑과 범위 unmap
- `user_loader.c`: 고정 가상주소 RISC-MV EXF 사용자 이미지 loader
- `process.c`: 동적 PID/TID, parent/child Process, Thread stack과 수명 검사
- `smp.c`: System Info/Core Control 탐색, 88-byte per-CPU 상태, secondary
  MMU-off trampoline, 전용 stack, BKL과 IPI wake/idle/stop 수명주기
- `devices.c`: VIO 검색, block/키보드/display와 내장 MMIO 장치 연결
- `fat32.c`: firmware 호환 FAT32 구현 자료
- `rmfs.c`, `vfs.c`: RMFS extent/bitmap 파일 읽기·쓰기와 단일 root VFS
- `syscall.c`: dispatcher, 표준 입출력·wait syscall과 user-copy 검증
- `scheduler.c`: runnable/reap/timeout queue, timer 선점, 일반 wait queue,
  wait/join/sleep wakeup, idle과 문맥 교환
- `exception.c`: VBR 구성, page-fault 정책, panic과 보호 자체 검사
- `kernel.s`: entry, 예외/trap GPR 보존, user 진입, `IRET`, fault probe
- `layout_start.s`, `layout_end.s`: 전체 section 경계와 bootstrap stack

즉 커널 정책과 일반 로직은 C에 있고 CPU가 만든 예외 프레임을 직접 다루는
부분만 짧은 assembly wrapper로 유지한다.
부팅 디스크 예제는 이
`kernel.exf`를 `/BOOT/KERNEL.EXF`로 넣는다. 이미지 자체도 text=RX,
rodata=R, data/BSS=RW인 다중 세그먼트이므로 loader 단계부터 W^X가
표현된다.

layout 오브젝트는 여러 C/assembly 오브젝트를 링크해도 전체 `.text`,
`.rodata`, `.data`, `.bss`의 시작과 끝 심볼이 올바르게 잡히게 한다. 커널은
`0x40000000` 고정 가상주소로 링크되지만 물리주소는 이미지에 고정하지 않는다.
부트로더가 펌웨어 USABLE map에서 first-fit 물리 base를 선택하며 빌드 스크립트는
2 MiB 최소 boot RAM에서 image span, 임시 page table과 RAM 상단 staging이 함께
들어갈 수 있는지 검사한다.

복구할 수 없는 예외는 UART에 `ECAUSE`, `EPC`, `BADADDR`, `EINFO`를 64비트
16진수로 출력한 뒤 해당 boot thread를 `HALT`한다. 현재 demand paging은
부팅 자체 검사용 주소 하나에만 허용하며, 임의의 page fault를 자동으로
복구하는 정책은 아직 적용하지 않는다.

현재 런타임 배치는 다음과 같다.

```text
0x00000000          unmapped NULL guard
0x40000000..text_end supervisor RX kernel text
next 4KiB section   supervisor R kernel rodata
next 4KiB section   supervisor RW kernel data
kernel_bss_start..kernel_bss_end supervisor RW globals and 24KiB bootstrap stack
0x3FFFF000          supervisor RW UART alias
0x3F000000          supervisor RW VIO hub alias
0x3F001000          supervisor RW IRQ controller alias
0x3F002000          supervisor RW timer alias
0x3F100000+slot*64K supervisor RW external device BAR alias
0x100000000+PA      supervisor RW/NX physical RAM direct-map
align_up(direct-map end + 2MiB, 4KiB)..+64MiB supervisor RW/NX kernel heap
```

heap은 물리적으로 연속되지 않은 PMM 페이지를 연속된 가상주소에 매핑한다.
현재 `free`는 block을 병합해 다시 사용하지만 heap 끝의 빈 page를 즉시 PMM으로
반환하지는 않는다. 이 정책은 초기 커널에서 주소 안정성을 유지하고 이후 page
trim 정책을 별도로 추가할 수 있게 한다.

사용자 실행 이미지는 `0x01000000..0x3E000000`에 배치한다. 첫 user stack은
`0x3EFF0000..0x3F000000`이고 같은 process에 thread를 추가할 때마다 4KiB guard를
사이에 두고 낮은 주소 방향으로 64KiB stack을 하나씩 추가한다. 각 thread는
heap에서 별도 16KiB kernel stack도 받는다. 한 process의 thread는 같은 address
space root를 공유하고, 서로 다른 process의 root와 user leaf/table page는 독립
소유한다. kernel mapping leaf는 supervisor 전용으로 공유한다. timer IRQ와
syscall은 동일한 trap-frame wrapper를 사용하고 scheduler가 선택된 thread의
frame과 kernel SP를 적용한 뒤, process가 달라질 때만 PTBR을 바꾼다.

현재 상태 전이는 생성 시 `NEW`, runnable queue 등록 시 `RUNNABLE`, dequeue 시
`RUNNING`, 실행 중인 자식을 기다릴 때 `BLOCKED`, 자식 종료 통지를 받으면 다시
`RUNNABLE`, syscall exit 또는 user fault 시 `ZOMBIE`, deferred reap 시 `DEAD`다.
마지막 thread가 종료되면 process도 `ZOMBIE`가 된다. 부모가 있는 zombie는
부모의 child list에 남아 `wait`/`waitpid`로 한 번만 수거되며, 부모 없는
process는 현재 kernel stack에서 빠져나온 다음 trap에서 자동 수거한다. 부모가
먼저 종료하면 남은 자식은 살아 있는 PID 1 init으로 재부모화한다. init 자체가
종료한 뒤 부모가 없어진 zombie는 자동 수거한다.

현재 `waitpid`는 정확한 양의 PID를, `wait`는 임의의 자식을 기다린다. exit
status는 signed 64비트 값으로 user 주소에 기록하며 null status 포인터는 값을
버리는 의미다. `join`은 같은 process의 다른 TID만 대상으로 하며, 한 target에
한 waiter만 등록한다. target이 끝나면 status를 전달하고 target user stack을
unmap한 뒤 현재 kernel stack에서 벗어난 trap에서 객체와 kernel stack을
수거한다. self-join은 `-EDEADLK`, 중복 join은 `-EBUSY`로 거부한다.

일반 `KernelWaitQueue`는 FIFO waiter 목록과 선택적인 timer deadline을 함께
관리하며 wake-one/wake-all을 제공한다. timeout tick을 0으로 지정하면 IRQ가
직접 깨울 때까지 무기한 대기하고, 양수면 timeout queue에도 동시에 등록한다.
wake와 timeout 중 먼저 처리된 경로가 두 큐에서 thread를 원자적으로 분리하므로
한 thread가 중복으로 runnable queue에 들어가지 않는다. SMP 커널 trap은 먼저
Big Kernel Lock을 얻은 뒤 조건 재검사와 waiter 등록을 수행한다. blocking kernel
continuation이나 hardware idle로 전환할 때는 BKL을 놓고, 다시 kernel C 문맥으로
복귀할 때 재획득하므로 IRQ-before-sleep lost-wakeup과 lock을 든 채 수면하는
교착을 피한다.

IRQ handler는 heap allocation, filesystem 진입, spin 대기나 수면을 하지 않는다.
block handler는 장치 status/error를 snapshot하고 요청 wait queue 하나를 깨우며,
keyboard handler는 MMIO queue를 고정 256-byte kernel ring으로 drain한 뒤 FIFO
waiter에게 직접 복사하거나 ring에 보관한다. completion과 timeout은 동일한
detach 경로를 사용하므로 늦게 도착한 두 번째 wake는 no-op이다.

실행 가능한 thread가 없고 `BLOCKED` thread만 남으면 scheduler는 `EI; WAIT`로
hardware idle에 들어간다. `EI` 직후 `WAIT` 실행 전에 timer IRQ가 도착한 경우
handler가 저장된 PC를 idle 복귀 label로 옮겨 이미 처리한 IRQ 뒤 다시 잠드는
경쟁을 막는다. 하드웨어 timer는 1GHz 나노초 tick을 사용하고 2,000,000 tick마다
스케줄러 IRQ를 발생시킨다. `sleep(milliseconds)`는 2ms timer quantum 단위로 올림해 이
timeout 경로를 사용한다. `waitpid`/`join`도 마지막 runnable thread를 block할 수
있으며 `WNOHANG` 옵션은 아직 없다.

설정된 secondary 논리 CPU는 각자의 stack, `current_thread`, kernel-stack owner를
가지며 global runnable queue에서 user thread를 받아 실제로 실행한다. user mode는
BKL 밖에서 병렬로 실행하고 syscall/exception/IPI로 kernel에 진입할 때만 공유
커널 상태를 직렬화한다. IPI 48은 programmable IRQ controller를 거치지 않고 대상
CPU의 VBR handler로 들어가 WAIT 복귀 PC를 보정한다. 종료나 user fault가 발생한
secondary는 `IRET`로 architecture exception 상태를 정리한 뒤 영구 idle stack으로
돌아간다.

같은 process의 서로 다른 thread도 여러 논리 CPU에서 동시에 실행한다. 각 thread는
첫 dispatch의 논리 CPU에 고정된다. timer가 thread를 runnable queue에 되돌린 뒤에도
원래 CPU만 다시 선택하므로, 그 CPU가 trap wrapper에서 아직 `IRET` 준비 중인 동안
다른 CPU가 같은 kernel stack을 사용하는 경쟁이 없다. `exec`와 process fault는
process stop flag를 세우고 모든 CPU에 IPI를 보낸다. sibling은 다음 trap에서
supervisor idle 진입점으로 복귀하고 영구 per-CPU stack으로 전환한 뒤에만 active
소유권을 지운다. 요청 CPU는 이 rendezvous가 끝난 뒤 old address space와 sibling
kernel stack을 회수한다.

BKL이 scheduler/process/wait queue의 최상위 lock이며 address-space, heap, RMFS,
FD/vnode와 device lock은 BKL 안쪽에서만 얻는다. 수면, user 실행과 보존된 kernel
continuation 사이의 전환에는 BKL을 들고 가지 않는다. affinity가 정해진 runnable
wake는 그 논리 CPU에 직접 IPI를 보내고, 미지정 thread는 idle CPU 하나를 깨운다.
process stop은 다른 CPU 전부에 reschedule IPI를 보낸다.
현재 MMU는 TLB cache 없이 매 접근마다 page table을 읽으므로 별도 TLB shootdown은
필요하지 않다. 향후 TLB를 넣을 때 ASID/generation과 shootdown acknowledgment가
함께 필요하다. timer tick과 외부 device IRQ는 Core 0/Thread 0이 담당하고 각 논리
CPU는 자기 user trap과 IPI를 처리한다. 같은 core의 hardware thread는 host의
core-local round-robin 실행 모델을, 다른 core는 host thread 병렬 실행 모델을 따른다.

현재 VFS는 RMFS system partition을 `/`로 마운트한다. RMFS는 최대 239바이트
case-sensitive 이름, 256바이트 inode, inline extent 6개, inode/block bitmap과
주/백업 CRC32 superblock을 사용한다. 파일 읽기와 기존 디렉터리 안의 regular
file 생성·전체 교체·4KiB block 단위 COW 부분 쓰기·append, 디렉터리 data block
자동 확장과 최대 6개 조각 extent 할당이 가능하다. 부분 쓰기는 변경 block만
새로 할당하고 기존 앞뒤 extent를 분할·재사용·병합하며 EOF 이후 hole은 0으로
채운다. 디렉터리 생성, 삭제/rename과 journal
replay는 후속 범위다. 각 process는 32-slot FD table을 소유하며,
FD slot은 참조 횟수가 있는 open-file handle을 가리키고 handle은 다시 vnode를
소유한다. FD 조회는 임시 참조를 얻고 close/process 종료는 slot 참조를 놓는다.
마지막 handle 참조가 사라지면 vnode와 경로 저장소도 함께 해제된다. 새 process는
FD 0에 read-only keyboard pseudo-vnode, FD 1과 2에 write-only UART
pseudo-vnode를 기본 설치한다. RMFS metadata/data 접근은 filesystem lock으로,
공용 block DMA bounce page는 block lock으로 직렬화한다. 4KiB scratch block은
전역 lock 아래 재사용해 64KiB per-thread kernel stack을 소모하지 않는다.
RMFS bitmap은 mount 동안 cache하고 inode/directory는 64-entry metadata block
cache로 유지한다. 변경은 volume transaction group에 누적되어 같은 metadata
block을 한 번만 기록하며 `fsync` 또는 32개 write에서 CLEAN commit과 flush를
수행한다. `close`는 암묵적 sync가 아니다. metadata CRC32는 lookup table로
계산하고 DIRTY 이후 I/O 실패는 volume을 unmount해 불완전한 transaction 위에서
쓰기를 계속하지 않는다. 온디스크 journal replay는 아직 후속 범위다.
세부 온디스크 규격은 [RMFS.md](../system/RMFS.md)에 있다.

block 장치의 호스트 worker는 event 기반으로 잠든다. scheduler 시작 전
GPT/RMFS mount와 self-test는 interrupt 문맥이 없으므로 bounded `STATUS` polling을
사용한다. scheduler 시작 뒤에는 block/keyboard vector와 controller mask를 켠다.
각 block 요청은 현재 kernel C continuation의 SP와 전체 GPR을 per-thread kernel
stack에 보존하고 wait queue에서 잠들며, 다른 runnable thread 또는 hardware
`WAIT`가 실행된다. 완료 IRQ가 원래 continuation을 복구한다. filesystem syscall은
sleep 가능한 gate로 직렬화하므로 다른 thread가 잠든 syscall이 소유한 filesystem
spinlock에 진입해 CPU를 소모하지 않는다.
init 회귀는 worker가 runnable인 동안 RMFS read를 수행하고 실제 continuation
전환 및 block 요청/완료 IRQ 횟수 일치를 검사한다.

display syscall은 host window와 독립적인 1x1~1920x1080 XRGB8888 guest mode를
제공한다. kernel은 `width * height * 4`와 4KiB 올림을 overflow 없이 검사하고
PMM에서 연속 물리 buffer를 할당해 현재 process의 `0x3E000000`부터 RW/NX로
매핑한다. 크기가 달라지면 새 buffer와 mapping이 모두 성공한 뒤 이전 buffer를
반환한다. 이 user mapping이 back buffer이고 display 장치가 present 때 DMA로
복사한 staging frame이 front buffer다. 호출 thread는 완료 IRQ까지 wait queue에서
잠들며 process 종료와 exec는 mapping 및 물리 page를 반환한다.
