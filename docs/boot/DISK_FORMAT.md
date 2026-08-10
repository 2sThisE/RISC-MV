# CVM 부팅 디스크 규격 v1

부팅 디스크는 512바이트 LBA를 사용하는 raw GPT 디스크다. 모든 정수는
little-endian이며 GPT GUID 필드만 GPT가 정의한 mixed-endian byte order를
따른다.

## 전체 배치

```text
LBA 0                  Protective MBR
LBA 1                  Primary GPT header
LBA 2..33              Primary partition entries (128 * 128 bytes)
LBA 34..2047           Alignment gap
LBA 2048..last-33      CVM FAT32 boot partition
last-32..last-1        Backup partition entries
last LBA               Backup GPT header
```

GPT의 first usable LBA는 34지만 부트 파티션은 1 MiB 정렬을 위해 LBA
2048에서 시작한다. 파티션은 backup GPT 직전의 last usable LBA까지다.

## CVM 부트 파티션

Partition Type GUID:

```text
9f7c3a21-6d52-4bc8-a3e1-43564d424f4f
```

GPT entry name은 `CVM Boot`다. 기본 생성은 운영체제 난수원으로 디스크
GUID와 partition unique GUID를 만든다. 재현 가능한 빌드가 필요할 때만
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
- 64 MiB급 volume은 1 sector/cluster
- 큰 v1 volume은 8 sectors/cluster

디렉터리 배치는 고정된 8.3 이름을 사용한다.

```text
root cluster 2
└─ BOOT/          cluster 3
   ├─ BOOT.CVM    cluster 4부터 연속 할당
   └─ KERNEL.CVM  BOOT.CVM 다음 cluster부터 연속 할당
```

ROM은 GPT에서 CVM GUID를 찾고 `BOOT/BOOT.CVM`을 실행한다. 2차 부트로더는
[FIRMWARE_ABI.md](FIRMWARE_ABI.md)의 파일 서비스를 사용해
`BOOT/KERNEL.CVM`을 읽는다. 두 파일 모두 현재
[BOOT_FORMAT.md](BOOT_FORMAT.md)의 검증된 segment image container를 쓴다.

## 무결성 검사

- GPT header CRC32
- GPT partition entry array CRC32
- primary/backup header와 entry array 일치
- 두 FAT 복사본 일치
- FAT cluster 범위와 chain 종료 검사
- `BOOT.CVM`과 `KERNEL.CVM` header 및 payload CRC32

CRC32는 손상 검출용이다. 악의적인 이미지에 대한 보안 부팅은 추후 서명
규격으로 별도 추가한다.
