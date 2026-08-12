# RISC-MV 부팅 디스크 도구 (`vmkdisk`)

`vmkdisk`는 검증된 RISC-MV 실행 이미지를 포함하는 GPT raw 디스크 이미지를
호스트에서 생성한다. 출력 이미지는 기존 block device 모듈의 backing
파일로 바로 사용할 수 있다.

## 빌드

```powershell
.\build.ps1 -e boot
```

## 생성

```powershell
.\build\tools\vmkdisk.exe create `
    -o .\custom-system.img `
    --size 64M `
    --bootloader .\build\examples\bootloader.exf `
    --kernel .\build\kernel\kernel.exf `
    --init .\build\examples\init.exf
```

크기는 512바이트 배수여야 하며 현재 v1은 64 MiB부터 64 GiB까지 받는다.
`K`, `M`, `G` 접미사는 1024 단위다. 이미 존재하는 출력은 덮어쓰지 않고
오류를 반환한다.

기본값은 운영체제 난수원으로 매번 고유한 GPT disk/partition GUID를 만든다.
바이트 단위 재현 빌드가 필요한 경우 `--reproducible`을 추가한다.

생성기는 다음을 기록한 뒤 디스크를 다시 읽어 전체 검증한다.

- Protective MBR
- Primary/Backup GPT header 및 128개 partition entry와 CRC32
- 40 MiB FAT32 boot partition과 RISC-MV system partition GPT entry
- FAT32 VBR, backup VBR, FSInfo와 두 개의 동일한 FAT
- FAT32의 `/BOOT/BOOT.EXF`, `/BOOT/KERNEL.EXF`
- RMFS v1 superblock/backup, inode·block bitmap, inode table
- RMFS의 `/BIN/INIT.EXF`
- 세 실행 이미지의 header/payload CRC32와 세그먼트 규격

## 검사

```powershell
.\build\tools\vmkdisk.exe inspect .\build\examples\system.img
```

손상된 MBR/GPT CRC, primary/backup 불일치, FAT geometry·cluster chain,
RMFS superblock/inode/directory CRC, 누락되거나 손상된 `BOOT.EXF`,
`KERNEL.EXF`, `INIT.EXF`를 거부한다. 상세 RMFS 규격은
[RMFS.md](../system/RMFS.md)에 있다.
