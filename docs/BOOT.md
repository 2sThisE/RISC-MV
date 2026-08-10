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
.\build\main.exe -r 4096 -l .\build\examples\counter.bin
```

동일한 명령을 긴 옵션으로도 작성할 수 있다.

```powershell
.\build\main.exe --ram 4096 --load .\build\examples\counter.bin
```

이때 바이너리는 RAM 1번지에 적재되고 boot hardware thread의 PC도 1이 된다.

Boot ROM 방식은 다음과 같다.

```powershell
.\examples\build_boot_rom.ps1
.\build\main.exe -r 4096 -rom .\build\examples\boot_rom.bin
```

`-rom`의 긴 이름은 `--rom`이다. 전체 실행 옵션은 `main.exe --help`로 확인한다.

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

## 실제 디스크 부팅으로 확장

현재 `examples/boot_rom.asm`은 ROM fetch와 UART MMIO를 검증하기 위해 `ROM`을 출력하고 HALT한다. 실제 디스크 부트로더는 같은 ROM 영역에서 다음 작업을 수행하면 된다.

1. VIO Hub에서 block 장치를 찾는다.
2. block 장치 MMIO로 커널 또는 다음 단계 로더를 읽는다.
3. 읽은 바이트를 RAM에 배치한다.
4. 필요하면 형식과 크기, 진입점을 검증한다.
5. SP와 초기 CPU 상태를 준비한다.
6. RAM의 커널 진입점으로 분기한다.
7. 커널이 페이지 테이블을 만든 후 MMU를 켠다.

IRQ를 사용하는 펌웨어나 커널은 VBR과 핸들러를 먼저 설치하고 IRQ Controller에서 해당 장치 IRQ의 route와 enable 비트를 설정한 뒤 `EI`해야 한다. reset 직후 외부 IRQ는 모두 masked 상태다.

이 과정에는 새로운 부팅 전용 opcode가 필요하지 않다. 기존 LOAD/STORE, 비교·분기와 간접 jump만 사용한다.

커널 파일과 handoff 구조는 [BOOT_FORMAT.md](BOOT_FORMAT.md)의 CVM 부팅
ABI v1을 따른다. `vmkimg`로 raw 어셈블 결과를 `/boot/kernel.cvm`에 넣을
수 있는 형식으로 포장한다. 현재 예시는 다음 명령으로 생성한다.

```powershell
.\examples\build_kernel_stub.ps1
.\build\vmkimg.exe inspect .\build\examples\kernel_stub.cvm
```
