# Boot ROM과 reset 흐름

## 물리 주소 영역

VM은 RAM, Boot ROM, MMIO를 하나의 64비트 물리 주소 공간에서 겹치지 않는 영역으로 구분한다.

| 영역 | 데이터 읽기 | 데이터 쓰기 | 명령어 fetch |
|---|---:|---:|---:|
| RAM | 허용 | 허용 | 허용 |
| Boot ROM | 허용 | 거부 | 허용 |
| MMIO | 장치에 따라 허용 | 장치에 따라 허용 | 항상 거부 |
| 미할당 주소 | 거부 | 거부 | 거부 |

RAM은 `0`부터 실행 인자 `-r`로 지정한 크기까지다. Boot ROM 시작 주소는 `0x7FFFF00000`이며 이미지 최대 크기는 1 MiB다. 실제로는 파일에 들어 있는 바이트 수만 ROM 영역으로 매핑한다. 기존 MMIO는 `0xFFFFFFFF...` 상위 주소 영역을 사용하므로 일반적인 RAM 및 ROM과 겹치지 않는다. 모든 매핑 함수는 영역 중첩을 거부한다.

ROM은 호스트의 별도 바이트 저장소를 사용하며 `Bus`에 읽기 전용으로 등록된다. MMIO는 같은 크기의 호스트 메모리를 할당하지 않고 주소 접근을 장치 callback으로 전달한다.

## 실행 모드

기존 직접 적재 방식은 그대로 사용할 수 있다.

```powershell
.\build\main.exe -r 4k -l .\build\examples\counter.bin
```

동일한 명령을 긴 옵션으로도 작성할 수 있다.

```powershell
.\build\main.exe --ram 4K --load .\build\examples\counter.bin
```

이때 바이너리는 RAM 1번지에 적재되고 boot hardware thread의 PC도 1이 된다.

Boot ROM 방식은 다음과 같다.

```powershell
.\build.ps1 -e boot
.\build\main.exe -r 1m -rom .\build\examples\boot_rom.bin
```

`-rom`의 긴 이름은 `--rom`이다. 전체 실행 옵션은 `main.exe --help`로 확인한다.
RAM 크기의 `b`, `k`, `m`, `g` 접미사는 대소문자를 구분하지 않으며 각각
byte, KiB, MiB, GiB를 뜻한다. 접미사를 생략하면 byte로 해석한다.

이때 RAM에 프로그램을 자동 적재하지 않고 boot PC를 `0x7FFFF00000`으로 설정한다. `-rom`과 `-l`을 동시에 사용할 수도 있다. 이 경우 PC는 ROM에서 시작하고 `-l` 파일은 RAM 1번지에 미리 적재되므로 ROM 코드가 이를 검사하거나 그 위치로 분기할 수 있다.

ROM은 `--base 0x7FFFF00000`으로 어셈블해야 절대 레이블과 주소가 실제 매핑 위치에 맞는다. ROM의 첫 바이트가 reset 진입점이며 별도 파일 헤더는 없다.

System Control의 `WARM_RESET` 명령은 장치·CPU·인터럽트 상태를 초기화하고 동일한 reset 진입점에서 다시 실행한다. RAM과 ROM 매핑은 보존되며 `RESET_CAUSE`는 software로, `RESET_COUNT`는 1 증가한다. 구체적인 레지스터는 `SYSTEM.md`에 정의되어 있다.

## fetch 흐름

```text
PC
 └─ MMU OFF: PC를 물리 주소로 사용
    MMU ON : PTE의 EXEC 권한 검사 후 물리 주소로 변환
             ↓
       물리 영역 디코드
       ├─ RAM → fetch
       ├─ ROM → fetch
       ├─ MMIO → INSTRUCTION_ACCESS
       └─ 미할당 → INSTRUCTION_ACCESS
```

PTE에 EXEC가 있더라도 변환 결과가 MMIO이면 실행할 수 없다. 반대로 ROM을 MMU가 켜진 상태에서 실행하려면 leaf PTE에 READ/EXEC를 주고 ROM의 물리 페이지를 매핑해야 한다. 페이지 테이블 자체는 계속 물리 RAM에 있어야 한다.

## 실제 디스크 부팅

`examples/boot/boot_rom.asm`과 `examples/boot/bootloader.asm`은 다음 부팅 단계를
나누어 실행한다.

1. VIO Hub에서 block 장치를 찾는다.
2. GPT header와 partition-entry CRC32를 검사하고 레거시 CVM Boot GUID를 찾는다.
3. FAT32의 `BOOT/BOOT.EXF`를 검증·적재하고 Firmware Table을 전달한다.
4. BOOT.EXF는 Firmware FileSize로 `BOOT/KERNEL.EXF` 크기를 얻고, 4KiB로
   올림 정렬한 staging buffer를 RAM 상단에 동적으로 배치해 파일을 읽는다.
5. BOOT.EXF가 kernel image를 검증하고 펌웨어 USABLE map에서 정렬된 물리
   first-fit 영역을 골라 LOAD/BSS를 배치한다.
6. 임시 페이지 테이블에 loader identity map, 고정 kernel VA, RAM direct-map,
   UART alias를 만들고 BootInfo 확장에 PTBR과 주소 geometry를 기록한다.
7. 최신 memory-map key로 `ExitBootServices`를 호출한다.
8. BOOT.EXF가 MMU를 켜고 kernel 가상 entry에 분기한다.

Boot ROM의 FAT32 reader는 `0x18000..0x181FF`에 최근 FAT sector 하나를
cache한다. 파일의 연속된 cluster run은 block 장치 한도인 최대 128 sector까지
한 번의 DMA 요청으로 읽으며, 조각난 chain이나 마지막 부분 sector는 기존
안전한 경로로 처리한다.

IRQ를 사용하는 펌웨어나 커널은 VBR과 핸들러를 먼저 설치하고 IRQ Controller에서 해당 장치 IRQ의 route와 enable 비트를 설정한 뒤 `EI`해야 한다. reset 직후 외부 IRQ는 모두 masked 상태다.

이 과정에는 새로운 부팅 전용 opcode가 필요하지 않다. 기존 LOAD/STORE, 비교·분기와 간접 jump만 사용한다.

커널 파일과 handoff 구조는 [BOOT_FORMAT.md](BOOT_FORMAT.md)의 RISC-MV 부팅
ABI v1을 따른다. `vmkimg`로 raw 어셈블 결과를 `/boot/kernel.exf`에 넣을
수 있는 형식으로 포장한다. 현재 reference kernel은 다음 명령으로 생성한다.

```powershell
.\build.ps1 -e boot
.\build\tools\vmkimg.exe inspect .\build\kernel\kernel.exf
```

`examples/boot/kernel_stub.asm`은 BootInfo CRC만 확인하고 종료하는 더 작은
부트 ABI 예제로 남겨 두며 `build.ps1 -e boot`가 `kernel_stub.exf`로 포장한다.

GPT/FAT32 부팅 디스크까지 만들려면 다음 예제를 실행한다.

```powershell
.\build.ps1 -e boot
.\build\tools\vmkdisk.exe inspect .\build\examples\system.img
```

전체 예제는 다음 한 줄로 빌드하고 실행한다. 현재 v1 ROM은 고정 작업
영역과 staging buffer 때문에 최소 1 MiB RAM이 필요하다.

```powershell
.\examples\boot\run_boot_demo.ps1
```

최소 RAM profile의 물리 배치는 다음과 같다. `KERNEL.EXF` 파일이 커지면
staging 시작은 아래로 이동하고, 커널 메모리가 커지면 kernel end는 위로
이동한다. 부트로더는 두 범위가 겹치면 적재를 거부한다.

```text
0x00000..0x1FFFF  firmware/BootInfo 작업 영역
0x20000..0x2FFFF  2차 부트로더 코드와 downward stack 슬롯
kernel_phys_base..kernel_end  동적으로 배치된 kernel LOAD/BSS/stack
kernel_end..initial_pt_end  임시 page table (handoff 뒤 reclaimable)
initial_pt_end..staging_start  usable RAM
staging_start..RAM_END  page-rounded KERNEL.EXF staging
```

정상 부팅에서는 다음 주요 성공 마커가 순서대로 출력된다. 3개 process의 4개
user thread syscall 출력이 중간에 추가되며 실제 출력 순서는 선점 시점에 따라
달라질 수 있다.

```text
RISC-MV ROM: start
RISC-MV ROM: bootloader
RISC-MV LOADER: start
RISC-MV LOADER: kernel
KERNEL: BootInfo OK
KERNEL: PMM OK
KERNEL: MMU ON
KERNEL: HEAP OK
KERNEL: STRUCTURES OK
KERNEL: DEVICES OK
KERNEL: VFS FAT32 RW OK
KERNEL: USER ADDRESS SPACE OK
KERNEL: VBR OK
KERNEL: SYSCALL DISPATCH OK
KERNEL: NULL PAGE BLOCKED
KERNEL: WRITE PROTECT OK
KERNEL: NX PROTECT OK
KERNEL: MEMORY PROTECTION OK
KERNEL: PAGE FAULT RECOVERED
KERNEL: USER TASKS START
KERNEL: USER FAULT ISOLATED
KERNEL: PROCESS REAP OK
KERNEL: PROCESS THREAD OK
KERNEL: USER SYSCALL OK
KERNEL: PREEMPTIVE SCHEDULER OK
KERNEL: READY
```

Firmware Table과 서비스 규격은 [FIRMWARE_ABI.md](FIRMWARE_ABI.md)에 있다.
`kernel` 폴더의 C reference kernel은 `0x40000000` 고정 가상주소에서 이미
MMU가 켜진 상태로 시작한다. BootInfo의 USABLE 메모리와
`0x100000000 + physical` direct-map으로 free-list PMM을 만들고, 자체 최종
페이지 테이블로 교체한 뒤 초기 PT 페이지를 회수한다. 커널 text=RX,
rodata=R, data/stack=RW이고 direct-map은 RW/NX다.
이후 물리 RAM에 77-entry VBR 테이블을 만들고 동기 예외를
공통 kernel panic에 연결한다. 부팅 자체 검사는 일부러 미매핑 가상주소를
읽어 `LOAD_PAGE_FAULT`를 발생시키며, 전용 핸들러가 페이지를 매핑하고
`IRET`으로 LOAD를 재실행해야 성공한다.
추가 보호 검사는 NULL load, rodata store, data-page indirect call이 각각
올바른 page fault를 발생시키는지 확인하고 저장된 예외 PC를 해당 probe의
다음 명령으로 조정한 뒤 `IRET`한다.
