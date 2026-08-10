# CVM reference kernel

이 폴더는 펌웨어와 부트로더 다음에 실행되는 CVM용 기준 커널의 시작점이다.
현재 커널은 다음 초기화 경로를 실제로 수행한다.

1. C로 작성된 초기 bootstrap이 `CvmBootInfo` handoff와 CRC32를 검증하고
   early UART를 설정한다.
2. `CVM_MEMORY_USABLE` 범위의 완전한 4KiB 페이지로 free-list 물리
   페이지 할당자를 구성한다.
3. 할당과 반환, 페이지 읽기/쓰기를 MMU 전후에 자체 검사한다.
4. NULL 페이지를 제외한 RAM을 supervisor RW identity mapping하고 UART를
   낮은 가상주소 `0x3FFFF000`에 매핑하는 3단계 페이지 테이블을 만든다.
   커널 text는 RX, rodata는 R, data와 stack은 RW로 다시 제한한다.
5. `SETPTBR`, `MMUON` 후 UART MMIO와 물리 페이지 할당자가 계속
   동작하는지 확인한다.
6. PMM에서 물리 페이지를 할당해 77-entry VBR 테이블을 설치하고 동기
   예외 1~11을 kernel panic에 연결한다.
7. 의도적인 `LOAD_PAGE_FAULT`에서 모든 GPR을 보존하고 누락 페이지를
   매핑한 뒤 `IRET`으로 실패한 LOAD를 재실행한다.
8. NULL 읽기, rodata 쓰기, data 페이지 실행을 실제로 시도해 각각
   page-not-present, store permission, instruction permission fault인지 확인한다.

빌드는 프로젝트 루트에서 실행한다.

```powershell
.\build.ps1 -e boot
```

전체 빌드 또는 `boot` 예제 빌드는 다음 소스를 각각 오브젝트로 만든 뒤
`cvmlink`로 `kernel.cvm`과 `kernel.map`을 생성한다.

- `kernel_main.c`: BootInfo 검증, UART와 전체 부팅 순서
- `pmm.c`: 물리 페이지 free-list
- `mmu.c`: 3단계 페이지 테이블과 MMU 전환
- `exception.c`: VBR 구성, page-fault 정책, panic과 보호 자체 검사
- `kernel.s`: entry, 예외 GPR 보존/복원, `IRET`, 의도적 fault probe
- `layout_start.s`, `layout_end.s`: 전체 section 경계와 bootstrap stack

즉 커널 정책과 일반 로직은 C에 있고 CPU가 만든 예외 프레임을 직접 다루는
부분만 짧은 assembly wrapper로 유지한다.
부팅 디스크 예제는 이
`kernel.cvm`을 `/BOOT/KERNEL.CVM`으로 넣는다. 이미지 자체도 text=RX,
rodata=R, data/BSS=RW인 다중 세그먼트이므로 loader 단계부터 W^X가
표현된다.

layout 오브젝트는 여러 C/assembly 오브젝트를 링크해도 전체 `.text`,
`.rodata`, `.data`, `.bss`의 시작과 끝 심볼이 올바르게 잡히게 한다. BSS 끝은
부트 프로토콜이 정한 커널 상한 `0x20000`을 넘을 수 없으며 빌드 스크립트가
링크 map을 검사해 초과를 즉시 오류로 처리한다.

복구할 수 없는 예외는 UART에 `ECAUSE`, `EPC`, `BADADDR`, `EINFO`를 64비트
16진수로 출력한 뒤 해당 boot thread를 `HALT`한다. 현재 demand paging은
부팅 자체 검사용 주소 하나에만 허용하며, 임의의 page fault를 자동으로
복구하는 정책은 아직 적용하지 않는다.

현재 런타임 배치는 다음과 같다.

```text
0x00000             unmapped NULL guard
0x10000..text_end   supervisor RX
next 4KiB section   supervisor R rodata
next 4KiB section   supervisor RW data
kernel_stack_bottom..kernel_stack_top (24KiB) supervisor RW kernel stack
other RAM pages     supervisor RW, non-executable
0x3FFFF000          supervisor RW UART alias
```
