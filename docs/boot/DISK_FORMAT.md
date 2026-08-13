# RISC-MV 부팅 디스크 규격 v1

부팅 디스크는 512바이트 LBA를 사용하는 raw GPT 디스크다. 모든 정수는
little-endian이며 GPT GUID 필드만 GPT가 정의한 mixed-endian byte order를
따른다.

## 전체 배치

```text
LBA 0                  Protective MBR
LBA 1                  Primary GPT header
LBA 2..33              Primary partition entries (128 * 128 bytes)
LBA 34..2047           Alignment gap
LBA 2048..83967        RISC-MV FAT32 boot partition (40 MiB)
LBA 83968..last-33     RISC-MV RMFS system partition
last-32..last-1        Backup partition entries
last LBA               Backup GPT header
```

GPT의 first usable LBA는 34지만 부트 파티션은 1 MiB 정렬을 위해 LBA
2048에서 시작한다. boot partition은 81920 sector이고, 그 다음 LBA 83968부터
backup GPT 직전의 last usable LBA까지 system partition으로 사용한다.

## 부트 파티션

Partition Type GUID:

```text
9f7c3a21-6d52-4bc8-a3e1-43564d424f4f
```

기존 disk ABI 호환을 위해 partition type GUID와
FAT volume label `CVM BOOT`는 레거시 값을 유지한다. 기본 생성은 운영체제
난수원으로 디스크 GUID와 partition unique GUID를 만든다. 재현 가능한 빌드가 필요할 때만
`vmkdisk create --reproducible`을 사용하며, 이 경우 kernel 이미지와 디스크
크기로부터 GUID를 결정적으로 생성한다.

## FAT32

- sector size: 512
- reserved sectors: 32
- FAT count: 2
- root cluster: 2
- FSInfo sector: 1, backup: 7
- backup VBR: 6
- media descriptor: `0xF8`
- volume label: `CVM BOOT`
- 260 MiB 미만(532,480 sectors 미만) volume은 1 sector/cluster
- 260 MiB 이상 volume은 8 sectors/cluster

디렉터리 배치는 고정된 8.3 이름을 사용한다.

```text
root cluster 2
└─ BOOT/          cluster 3
   ├─ BOOT.EXF    cluster 4부터 연속 할당
   └─ KERNEL.EXF  BOOT.EXF 다음 cluster부터 연속 할당
```

ROM은 GPT에서 RISC-MV GUID를 찾고 `BOOT/BOOT.EXF`를 실행한다. 2차 부트로더는
[FIRMWARE_ABI.md](FIRMWARE_ABI.md)의 파일 서비스를 사용해
`BOOT/KERNEL.EXF`를 읽는다. 두 파일 모두 현재
[BOOT_FORMAT.md](BOOT_FORMAT.md)의 검증된 segment image container를 쓴다.

## 무결성 검사

- GPT header CRC32
- GPT partition entry array CRC32
- primary/backup header와 entry array 일치
- 두 FAT 복사본 일치
- FAT cluster 범위와 chain 종료 검사
- `BOOT.EXF`와 `KERNEL.EXF` header 및 payload CRC32

CRC32는 손상 검출용이다. 악의적인 이미지에 대한 보안 부팅은 추후 서명
규격으로 별도 추가한다.

## 시스템 파티션

Partition Type GUID:

```text
3a92d7e4-1d2b-4c68-9a6f-524953434d56
```

system partition은 [RMFS.md](../system/RMFS.md) v1로 포맷한다. reference
kernel은 GPT에서 이 GUID를 찾아 RMFS를 `/`로 마운트하고
`/BIN/INIT.EXF`를 PID 1로 실행한다. 따라서 FAT32는 firmware와 bootloader의
읽기 전용 부팅 계약만 담당하고, 운영체제 파일 쓰기는 RMFS에만 일어난다.

RMFS 검사는 primary/backup superblock과 CRC32, geometry, allocation bitmap,
root/BIN/init inode, directory entry CRC와 `INIT.EXF` 자체 CRC까지 포함한다.
