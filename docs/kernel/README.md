# CVM reference kernel

이 폴더는 펌웨어와 부트로더 다음에 실행되는 CVM용 기준 커널의 시작점이다.
현재 커널은 다음 초기화 경로를 실제로 수행한다.

1. `CvmBootInfo` handoff와 CRC32를 검증한다.
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

전체 빌드 또는 `boot` 예제 빌드 과정에서 `kernel.s`는 먼저
`build/kernel/kernel.o`로 어셈블되고 `cvmlink`가
`kernel.cvm`과 `kernel.map`을 만든다. 부팅 디스크 예제는 이
`kernel.cvm`을 `/BOOT/KERNEL.CVM`으로 넣는다. 이미지 자체도 text=RX,
rodata=R, data/BSS=RW인 다중 세그먼트이므로 loader 단계부터 W^X가
표현된다.

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
0x1F000..0x1FFFF    supervisor RW kernel stack
other RAM pages     supervisor RW, non-executable
0x3FFFF000          supervisor RW UART alias
```
