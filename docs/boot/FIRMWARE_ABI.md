# RISC-VM Firmware ABI v1

RISC-VM Firmware ABI는 Boot ROM과 `BOOT.EXF` 2차 부트로더 사이의 최소 공통
규격이다. UEFI의 System Table과 Boot Services 분리를 참고하지만, RISC-VM 독자
ISA와 VIO 장치를 위한 별도 규격이며 UEFI 호환을 주장하지 않는다.

## 부팅 경로

```text
reset -> Boot ROM -> /BOOT/BOOT.EXF -> /BOOT/KERNEL.EXF
      -> ExitBootServices -> kernel entry
```

Boot ROM은 GPT/FAT32와 VIO block 장치를 소유한다. 2차 부트로더는 디스크
구조를 직접 해석하지 않고 Firmware Table의 파일 서비스를 사용한다.

## BOOT.EXF handoff

| 상태 | 값 |
|---|---|
| `PC` | BOOT.EXF physical entry |
| `R0` | `CvmFirmwareTable` 물리 주소, v1은 `0x6000` |
| `R1` | `CVM_FIRMWARE_HANDOFF_MAGIC` |
| `R2` | boot VIO slot |
| `SP` | 16바이트 정렬 임시 stack |
| privilege | supervisor |
| MMU | off |
| IRQ | disabled |

`BOOT.EXF`는 kernel image와 같은 검증된 `RVMEXF01` segment container를
사용한다. 이미지의 의미는 디스크 경로와 handoff ABI로 구분한다.

## 호출 규약

- `R0`~`R5`: argument와 return value
- `R6`~`R9`: caller-saved
- `R10`~`R14`: callee-saved
- `R15`: stack pointer
- stack은 호출 경계에서 16바이트 정렬
- status는 `R0=0` 성공, `R0=1` 실패

서비스 주소는 guest physical address이며 `CALLR`로 호출한다. 모든 서비스는
supervisor/MMU-off 부팅 환경에서 실행한다.

## CvmFirmwareTable

정확한 C layout은 `include/firmware.h`에 있으며 전체 크기는 256바이트다.
Checksum은 offset `0x18`을 0으로 보고 전체 256바이트에 계산한 CRC32다.

| Offset | Field |
|---:|---|
| `0x00` | magic = `CVMFW001` |
| `0x08` | ABI major/minor |
| `0x0C` | header size |
| `0x10` | total size |
| `0x18` | checksum |
| `0x20` | RAM size |
| `0x28` | CPU feature mask |
| `0x30` | page size |
| `0x38`, `0x40` | physical/virtual address bits |
| `0x48`~`0x78` | VIO와 built-in system device bases |
| `0x80`, `0x84` | boot slot과 GPT partition index |
| `0x88`, `0x90` | boot partition LBA와 sector count |
| `0x98` | current memory-map key |
| `0xA0` | ConsoleWrite |
| `0xA8` | GetMemoryMap |
| `0xB0` | GetFileSize |
| `0xB8` | ReadFile |
| `0xC0` | ExitBootServices |
| `0xC8` | ResetSystem |

## 서비스

### ConsoleWrite

`R0=byte address`, `R1=length`. Boot UART에 정확히 length만큼 출력한다.

### GetMemoryMap

`R0=CvmMemoryMapEntry buffer`, `R1=entry capacity`. 성공 시 `R1=entry count`,
`R2=map key`를 반환한다. v1 기본 map은 low firmware RAM과 나머지 usable
RAM 두 구간이다. 부트로더는 kernel, BootInfo와 자신의 임시 영역을 반영해
최종 kernel memory map을 만든다.

### GetFileSize

`R0=11-byte FAT 8.3 short name`. 성공 시 `R1=file size`다. 이름은 NUL 종료
문자열이 아니라 `KERNEL  EXF`처럼 정확히 11바이트다.

### ReadFile

`R0=11-byte name`, `R1=destination`, `R2=capacity`. Boot directory의 FAT
chain을 따라 exact file bytes를 복사한다. 성공 시 `R1=file size`다.

### ExitBootServices

`R0=마지막 GetMemoryMap의 map key`. key가 최신이면 파일, console, memory-map
서비스를 비활성화하고 성공한다. 이후 장치와 RAM의 소유권은 kernel로
넘어가며 ResetSystem만 호출할 수 있다.

### ResetSystem

`R0=1`은 shutdown, `R0=2`는 warm reset이다.

## v1 제한

- FAT short name과 Boot directory 파일만 지원
- 동적 memory allocation service 없음
- Runtime Service는 ResetSystem만 제공
- BOOT.EXF는 단일 fixed segment이고 reference KERNEL.EXF는 링커가 만든
  다중 relocatable-physical segment를 사용
- Secure Boot와 image signature 없음
