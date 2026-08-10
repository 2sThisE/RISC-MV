# CVM 부팅 ABI v1

이 문서는 Boot ROM, 커널 이미지 생성기와 커널 사이의 바이트 단위 규약을
정의한다. 다중 바이트 정수는 모두 little-endian이다. 디스크 및 RAM에
C 구조체를 그대로 쓰지 않고 `boot_format.h`의 encode/decode 함수를
사용한다.

## 커널 이미지

기본 경로는 GPT 부트 파티션의 `/boot/kernel.cvm`이다.

```text
CvmKernelHeader (128 bytes)
CvmKernelSegment[segment_count] (64 bytes each)
segment payloads
```

### CvmKernelHeader

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | 8 | magic = `CVMKERN1` |
| `0x08` | 2 | format major = 1 |
| `0x0A` | 2 | format minor = 0 |
| `0x0C` | 4 | header size = 128 |
| `0x10` | 8 | flags, v1에서는 0 |
| `0x18` | 4 | ISA ID = little-endian `CVM1` |
| `0x1C` | 4 | ISA version = 1 |
| `0x20` | 1 | address bits = 64 |
| `0x21` | 1 | byte order = 1 (little-endian) |
| `0x22` | 2 | segment count, 1..64 |
| `0x24` | 4 | segment entry size = 64 |
| `0x28` | 8 | segment table file offset |
| `0x30` | 8 | physical entry address |
| `0x38` | 8 | complete image file size |
| `0x40` | 8 | required CPU feature mask |
| `0x48` | 16 | build ID |
| `0x58` | 4 | header CRC32 |
| `0x5C` | 4 | payload CRC32 |
| `0x60` | 32 | reserved, zero |

Header CRC32는 `header_crc32` 필드를 0으로 보고 128바이트에 계산한다.
Payload CRC32는 offset 128부터 파일 끝까지 계산하므로 세그먼트 테이블도
보호한다. CRC32는 손상 검출용이며 보안 서명이 아니다.

### CvmKernelSegment

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | 4 | type = LOAD(1) |
| `0x04` | 4 | READ(1), WRITE(2), EXECUTE(4) |
| `0x08` | 8 | file offset |
| `0x10` | 8 | physical load address |
| `0x18` | 8 | future virtual address |
| `0x20` | 8 | bytes stored in file |
| `0x28` | 8 | bytes occupied in RAM |
| `0x30` | 8 | power-of-two alignment |
| `0x38` | 8 | reserved, zero |

부트로더는 `file_size`만큼 복사하고 이어지는
`memory_size - file_size` 바이트를 0으로 만든다. 진입점은 EXECUTE가
설정된 LOAD 세그먼트 안에 있어야 한다. 부트로더는 덧셈 overflow,
파일 범위, RAM 범위와 세그먼트 중첩을 모두 검사해야 한다.

## 커널 handoff

Boot ROM은 다음 상태에서 커널로 분기한다.

| 상태 | 값 |
|---|---|
| `PC` | kernel physical entry |
| `R0` | `CvmBootInfo` 물리 주소 |
| `R1` | `CVM_BOOTINFO_HANDOFF_MAGIC` |
| `R2` | boot hardware-thread ID |
| `R15/SP` | 16바이트 정렬 임시 스택 top |
| privilege | supervisor |
| MMU | off (v1 기본) |
| IRQ | disabled |
| 다른 GPR | zero |

## CvmBootInfo

고정 헤더는 256바이트다. `memory_map_offset`과
`command_line_offset`은 `CvmBootInfo` 시작 주소를 기준으로 한 상대
offset이다. `total_size`는 고정 헤더와 모든 부속 데이터를 포함한다.

주요 필드는 RAM 범위, kernel/initrd 범위, VIO Hub와 시스템 장치 MMIO,
부팅 디스크 슬롯, GPT 파티션 위치, 주소 폭과 4 KiB 페이지 크기다. 정확한
offset과 C 선언은 `include/boot_format.h`에 있다.

Checksum은 `checksum` 필드를 0으로 보고 `total_size` 전체에 계산한
CRC32다. 메모리 맵은 base 오름차순이며 서로 겹치지 않아야 한다.

메모리 타입은 USABLE, RESERVED, KERNEL, BOOTLOADER_RECLAIMABLE,
BOOT_INFO, INITRD, FIRMWARE, MMIO를 정의한다. 커널은 USABLE만 즉시
할당하며 BootInfo를 필요한 곳으로 복사한 뒤 BOOTLOADER_RECLAIMABLE을
회수할 수 있다.

전체 장치 목록은 BootInfo에 복제하지 않는다. 커널은 `vio_hub_base`와
`system_info_base`를 root로 사용해 장치를 검색한다.

## 현재 구현 경계

- `src/boot_format.c`: kernel 이미지와 BootInfo 직렬화, CRC32와 검증
- `tools/kernel_image`: raw 바이너리를 단일 세그먼트 kernel 이미지로 포장
- Boot ROM의 GPT/FAT32 탐색 및 DMA 적재: 다음 부트로더 단계
- 다중 세그먼트 생성: 향후 링커 단계
