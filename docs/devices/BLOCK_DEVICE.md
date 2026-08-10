# Raw 블록 장치

`devices/block/block_device.c`는 호스트 파일을 512바이트 섹터 배열로 노출하는 VIO storage 장치다. 게스트에는 파일이나 디렉터리가 아니라 raw LBA 블록만 보인다. 파일시스템 해석은 게스트 운영체제의 책임이다.

## 빌드와 연결

```powershell
.\build.ps1 -e block -d block

.\build\main.exe -r 4096 -l .\build\examples\block_demo.bin `
    -d .\build\devices\block_device.dll `
    -dc "path=.\disk.img;create=67108864"
```

`-dc`는 바로 앞의 `-d` 모듈에 configuration 문자열을 전달한다. 여러 모듈을 연결할 때는 필요한 `-d` 바로 뒤에 `-dc`를 둔다.

지원 설정:

| 키 | 의미 |
|---|---|
| `path` | backing raw 이미지 경로, 필수 |
| `create` | 파일이 없을 때 생성할 바이트 크기 |
| `readonly` | `true`/`false` 또는 `1`/`0` |

경로만 전달하는 축약형도 가능하다.

```powershell
-dc ".\disk.img"
```

이미지 크기는 512의 양의 배수여야 한다. `create`는 기존 파일을 자르거나 확장하지 않고 파일이 없을 때만 사용된다. 읽기 전용과 생성 옵션은 함께 사용할 수 없다.

## MMIO 레지스터

모든 레지스터 접근 폭은 8바이트다. 실제 BAR 주소와 IRQ는 VIO Hub에서 검색한다.

| 오프셋 | 이름 | 접근 | 의미 |
|---:|---|---|---|
| `0x00` | `MAGIC` | R | `0x314B4C42564D` |
| `0x08` | `VERSION` | R | 규격 버전 1 |
| `0x10` | `FEATURES` | R | DMA, FLUSH, READ_ONLY |
| `0x18` | `CAPACITY` | R | 전체 512바이트 섹터 수 |
| `0x20` | `SECTOR_SIZE` | R | 512 |
| `0x28` | `MAX_TRANSFER` | R | 한 요청 최대 128섹터(64KiB) |
| `0x30` | `LBA` | R/W | 시작 섹터 |
| `0x38` | `DMA_ADDRESS` | R/W | 게스트 물리 RAM 주소 |
| `0x40` | `SECTOR_COUNT` | R/W | 처리할 섹터 수 |
| `0x48` | `COMMAND` | W | READ=1, WRITE=2, FLUSH=3 |
| `0x50` | `STATUS` | R/W1C | READY, BUSY, DONE, ERROR, READ_ONLY |
| `0x58` | `ERROR` | R | 마지막 오류 코드 |
| `0x60` | `CONTROL` | R/W | bit 0 IRQ_ENABLE |
| `0x68` | `IRQ_STATUS` | R | COMPLETE, ERROR |
| `0x70` | `IRQ_ACK` | W1C | 완료 원인 해제 |

### FEATURES

| 비트 | 이름 |
|---:|---|
| 0 | `DMA` |
| 1 | `FLUSH` |
| 2 | `READ_ONLY` |

### STATUS

| 비트 | 이름 | 의미 |
|---:|---|---|
| 0 | `READY` | 새 요청을 받을 수 있음 |
| 1 | `BUSY` | worker가 요청 처리 중 |
| 2 | `DONE` | 마지막 요청 성공 |
| 3 | `ERROR` | 마지막 요청 실패 |
| 4 | `READ_ONLY` | backing 파일이 읽기 전용 |

`DONE`과 `ERROR`는 해당 비트를 `STATUS`에 쓰면 해제된다. 새 정상 요청을 제출하면 이전 완료 상태는 `BUSY`로 교체된다.

### 오류 코드

| 값 | 이름 |
|---:|---|
| 0 | `NONE` |
| 1 | `BUSY` |
| 2 | `COMMAND` |
| 3 | `RANGE` |
| 4 | `DMA` |
| 5 | `IO` |
| 6 | `READ_ONLY` |
| 7 | `ALLOCATION` |

## 요청 처리

READ와 WRITE는 `LBA`, `DMA_ADDRESS`, `SECTOR_COUNT`를 먼저 설정한 뒤 `COMMAND`를 쓴다. 장치는 값을 요청 구조체에 snapshot하고 module 전용 worker에서 비동기로 처리한다.

```text
WRITE: guest RAM --dma_read--> staging --fwrite--> disk.img
READ : disk.img --fread--> staging --dma_write--> guest RAM
```

완료되면 `STATUS`가 `DONE` 또는 `ERROR`가 되고, IRQ가 활성화됐다면 할당된 IRQ를 발생시킨다. 현재 queue depth는 1이다. `BUSY` 중 새 명령을 제출하면 진행 중 요청은 유지되고 `BUSY` 오류 IRQ가 기록된다.

LBA 범위는 `count <= capacity - lba` 형태로 검사하여 덧셈 overflow를 피한다. DMA는 Device Manager가 일반 물리 RAM 범위를 다시 검사하며 MMIO 주소에는 DMA할 수 없다.

`FLUSH`는 C stream을 비우고 Windows에서는 `_commit`, POSIX에서는 `fsync`까지 호출한다. 일반 WRITE 완료는 호스트 캐시에 기록됐음을 의미하며 영구 저장 경계가 필요할 때 게스트가 FLUSH를 명시해야 한다.

## 현재 제한

- 한 장치당 요청 하나만 처리
- scatter/gather 및 descriptor queue 없음
- hot-unplug 없음
- 취소 가능한 진행 중 요청 없음
- raw 이미지 자체의 snapshot/COW 없음
- 장치는 파티션과 파일시스템을 해석하지 않음. 호스트의 `vmkdisk`가
  GPT/FAT32 이미지를 만들고 `examples/boot/boot_rom.asm`이 guest에서 이를 해석함

`examples/block/block_demo.asm`은 첫 외부 BAR 주소를 사용해 LBA 1에 한 섹터를 쓰고 다시 읽은 뒤 UART로 `B`를 출력한다. 범용 게스트 드라이버는 이 주소를 하드코딩하지 않고 VIO Hub에서 storage class 장치를 검색해야 한다.
