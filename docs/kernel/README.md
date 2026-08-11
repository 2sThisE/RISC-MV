# RISC-VM reference kernel

이 폴더는 펌웨어와 부트로더 다음에 실행되는 RISC-VM 기준 커널의 시작점이다.
현재 커널은 다음 초기화 경로를 실제로 수행한다.

1. C로 작성된 초기 bootstrap이 `CvmBootInfo` handoff와 CRC32를 검증하고
   early UART를 설정한다.
2. `CVM_MEMORY_USABLE` 범위의 완전한 4KiB 페이지로 free-list 물리
   페이지 할당자를 구성한다.
3. `0x100000000 + physical` direct-map으로 물리 page free-list를 접근하고
   할당과 반환을 자체 검사한다.
4. 부트로더의 MMU-on handoff를 검증한 뒤 자체 3단계 페이지 테이블을 만든다.
   커널 text는 RX, rodata는 R, data와 stack은 RW, direct-map은 RW/NX다.
5. 새 PTBR로 교체하고 부트로더가 만든 임시 page-table arena를 회수한다.
6. PMM에서 물리 페이지를 할당해 77-entry VBR 테이블을 설치하고 동기
   예외 1~11을 kernel panic에 연결한다.
7. 의도적인 `LOAD_PAGE_FAULT`에서 모든 GPR을 보존하고 누락 페이지를
   매핑한 뒤 `IRET`으로 실패한 LOAD를 재실행한다.
8. NULL 읽기, rodata 쓰기, data 페이지 실행을 실제로 시도해 각각
   page-not-present, store permission, instruction permission fault인지 확인한다.
9. direct-map 뒤의 동적 가상주소에 물리 페이지를 매핑해 16바이트 정렬 kernel
   heap을 구성하고 allocation 분할·병합·재사용과 다중 페이지 할당을 검사한다.
10. intrusive list, 동기화 byte queue, bitmap과 spinlock 런타임을 초기화하고
    자체 검사를 수행한다.
11. supervisor kernel mapping을 유지하는 독립 PTBR을 만들고 RISC-VM `.exf`의 CRC,
    주소 범위, segment 중첩과 W^X를 검증해 user RX/R/RW page와 64KiB stack을
    적재한다. guard page와 supervisor mapping의 user 접근 차단도 검사한다.
12. VIO hub에서 block/키보드/display 장치를 찾고 supervisor MMIO alias로
    매핑한다. block DMA bounce page로 GPT의 FAT32 boot partition을 마운트해
    8.3 경로 파일 읽기, 생성, 교체와 flush를 검사한다.
13. syscall vector 76에 dispatcher를 연결하고 page별 user 권한을 먼저
    검증하는 `copy_from_user`/`copy_to_user`로 잘못된 포인터와 overflow를
    fault 없이 거부한다. 초기 ABI는 `exit`, `write`, `read`, `yield`, `getpid`다.
14. 두 RISC-VM user task를 독립 주소 공간에 올리고 timer IRQ 0에서 전체 GPR,
    PC, FLAGS, SP와 PTBR을 저장·교체한다. 두 task의 실제 syscall 출력과 종료,
    timer 기반 선점이 모두 관측되어야 `KERNEL: READY`를 출력한다.

빌드는 프로젝트 루트에서 실행한다.

```powershell
.\build.ps1 -e boot
```

전체 빌드 또는 `boot` 예제 빌드는 다음 소스를 각각 오브젝트로 만든 뒤
legacy 이름의 `cvmlink`로 `kernel.exf`와 `kernel.map`을 생성한다.

- `kernel_main.c`: BootInfo 검증, UART와 전체 부팅 순서
- `pmm.c`: 물리 페이지 free-list
- `mmu.c`: 3단계 페이지 테이블과 MMU 전환
- `heap.c`: 동적 가상주소 기반 kernel heap
- `runtime.c`: list, byte queue, bitmap과 spinlock
- `address_space.c`: 프로세스별 page table과 user page 관리
- `user_loader.c`: 고정 가상주소 RISC-VM EXF 사용자 이미지 loader
- `devices.c`: VIO 검색, block/키보드/display와 내장 MMIO 장치 연결
- `fat32.c`, `vfs.c`: FAT32 8.3 파일 읽기/쓰기와 단일 root VFS
- `syscall.c`: dispatcher, 표준 입출력 syscall과 user-copy 검증
- `scheduler.c`: user task, timer 선점과 trap-frame 문맥 교환
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
1 MiB 최소 RAM에서 image span, 임시 page table과 RAM 상단 staging이 함께
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
kernel_stack_bottom..kernel_stack_top (24KiB) supervisor RW kernel stack
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

사용자 실행 이미지는 `0x01000000..0x3E000000`에 배치하고 stack은
`0x3EFF0000..0x3F000000`을 사용한다. stack 바로 아래 page는 guard로
남긴다. 각 주소 공간의 root와 user leaf/table page는 독립 소유하지만 kernel
mapping leaf는 supervisor 전용으로 공유한다. timer IRQ와 syscall은 동일한
고정 trap-frame wrapper를 사용하고, scheduler가 frame과 PTBR을 교체한 뒤
`IRET`하면 선택된 task의 user PC/SP에서 실행이 이어진다.

현재 VFS는 boot partition 하나를 `/`로 취급하며 FAT32 short 8.3 이름을
지원한다. 파일 읽기와 같은 디렉터리 안의 파일 생성·교체는 가능하지만 LFN,
디렉터리 생성/삭제와 프로세스별 descriptor table은 P1.3 범위다. FAT 탐색은
최근 primary FAT sector를 cache하고, 새 cluster 할당은 FAT32 FSInfo의
`next_free` hint에서 시작한다. 파일 데이터는 물리적으로 연속된 cluster를
최대 4KiB(8 sector) 단위로 묶어 읽는다.

block 장치의 호스트 worker는 event 기반으로 잠들지만, boot 초기와 현재 kernel
driver의 완료 확인은 `STATUS` polling이다. block IRQ 기반 sleep/wakeup과 keyboard
IRQ 입력 queue는 후속 작업이다.
