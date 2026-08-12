# RMFS v1

RMFS(RISC-MV File System)는 RISC-MV system partition의 전용 파일시스템이다.
부팅 호환용 FAT32와 분리되어 있으며 reference kernel은 RMFS를 `/`로 마운트한다.
모든 정수는 little-endian이고, 디스크 sector는 512바이트, RMFS block은
4096바이트다.

## 기본 구조

```text
block 0                 primary superblock
inode bitmap            필요한 만큼의 연속 block
block bitmap            필요한 만큼의 연속 block
inode table             256-byte inode array
data blocks             directory entry와 file data
last block              backup superblock
```

superblock magic은 `RMVFS001`, 현재 format version은 1.0이다. primary와
backup은 동일해야 하며 각 superblock, inode, directory entry는 CRC32를 가진다.
volume UUID는 GPT system partition unique GUID와 동일한 16바이트 값이다.

block bitmap은 전체 volume block마다 한 bit, inode bitmap은 inode마다 한 bit를
사용한다. 현재 생성기는 volume block 1024개당 inode 하나를 두되 최소 1024개,
최대 262144개 범위에서 16개 단위로 정렬한다. 이 수치는 포맷 시점 정책이며
디스크 구조의 고정 상한은 아니다.

## inode와 extent

inode 크기는 256바이트다. mode/type, uid/gid, link count, byte size, 네 종류의
timestamp 자리, generation, parent inode와 최대 6개의 inline extent를 가진다.
extent 하나는 logical block, physical block, block count, flags로 구성된다.

현재 kernel writer는 bitmap의 빈 run을 최대 6개 inline extent로 조합한다.
전체 파일 교체는 새 파일 공간 전체를 마련한 뒤 inode를 교체하는 COW이고,
FD 범위 쓰기는 변경되는 logical block 구간만 새로 할당한다. 변경되지 않은
앞뒤 구간은 기존 extent를 잘라 재사용하며 물리적으로 인접한 구간은 다시
병합한다. inode 교체가 끝난 뒤 교체된 이전 block만 반환한다.

파일 크기의 별도 16 MiB 정책 제한은 없으며 실제 한계는 volume 여유 공간,
한 extent의 32비트 block count와 inode당 6개 extent다. 부분 교체 결과가 inline
extent 6개를 넘는 고조각화 파일은 외부 extent tree가 추가되기 전까지 쓰기를
거부한다.

mode의 파일 종류 값은 Unix 계열과 같은 `0040000`(directory),
`0100000`(regular)을 사용하며 하위 9비트는 권한 자리다. v1 kernel은 이 값을
기록하고 형식을 검증하지만 uid/gid 기반 접근 제어 정책은 아직 적용하지 않는다.

## 디렉터리

directory entry는 256바이트 고정 크기다.

- inode number: 64비트
- mode/type: 16비트
- UTF-8 name length: 16비트
- name: 최대 239바이트
- CRC32: 32비트

이름 비교는 현재 byte 단위 case-sensitive 비교다. `.`과 `..` entry는 저장하지
않고 inode의 parent 번호로 부모를 표현한다. 초기 system volume은 다음과 같다.

```text
inode 1  /
└─ inode 2  BIN/
   └─ inode 3  INIT.EXF
```

kernel은 기존 디렉터리의 빈 entry에 regular file을 생성한다. 빈 entry가 없으면
data block을 추가하고 마지막 extent와 인접하면 병합하며, 아니면 남은 inline
extent를 사용한다. 읽기, 전체 파일 교체, 4KiB scratch 기반 block 단위 COW
부분 FD write, append와 flush를 지원한다. EOF를 넘어 쓰면 중간 hole을 0으로
채우며, 기존 EOF가 block 중간이면 새로 노출되는 같은 block의 꼬리도 0으로
초기화한다. 현재 system call `open`에는
create flag가 없으므로 user program은 이미 존재하는 파일을 열며, kernel VFS
API의 전체 파일 쓰기는 새 파일을 만들 수 있다.

## 쓰기 순서와 동시성

RMFS writer는 여러 logical write를 하나의 transaction group으로 묶어 다음
순서를 사용한다.

1. 새 data block과 필요하면 새 inode를 bitmap에서 예약한다.
2. group의 첫 변경에서 primary/backup superblock을 `DIRTY`로 기록하고 flush해
   변경 시작을 먼저 확정한다.
3. 새 data block은 즉시 기록하고 bitmap, inode와 directory metadata 변경은
   메모리 cache에 누적한다.
4. inode가 더 이상 참조하지 않는 교체 대상 block을 cache에서 반환하고
   free count와 sequence를 갱신한다.
5. `fsync` 또는 32개 logical write 누적 시 dirty metadata block을 한 번씩
   writeback하고 primary/backup superblock을 `CLEAN`으로 기록한 뒤 flush한다.

파일시스템 전역 spinlock이 metadata와 data 작업을 직렬화하고 block driver의
별도 lock이 하나뿐인 DMA bounce page를 보호한다. 새 data를 먼저 쓰므로 정상적인
오류 반환에서는 기존 파일을 먼저 해제하지 않는다.

kernel은 mount 때 inode/block bitmap을 한 번 읽어 heap에 유지하고 inode table과
directory용 64-entry 4KiB metadata block cache를 둔다. bitmap 전체를 다시
할당하거나 읽지 않으며, group 안에서 같은 inode/directory/bitmap block이 여러
번 바뀌어도 commit 때 최종 block을 한 번만 기록한다. dirty cache가 가득 차면
DIRTY 상태에서 가장 오래된 entry를 writeback하고 이후 CLEAN commit에 포함한다.
checksum은 mount 때 생성한 256-entry CRC32 table을 사용한다.

`close`는 동기화를 의미하지 않는다. user ABI의 `fsync(fd)`와 kernel의 volume
sync가 group을 확정하며, 32개 변경 누적 시에도 자동 commit해 미확정 구간을
제한한다. DIRTY 기록 전에 실패하면 cache의 임시 allocation을 rollback하고,
DIRTY 이후 metadata I/O 오류가 발생하면 부분 commit 가능성이 있으므로 해당
volume을 즉시 unmount한다.

현재 group commit은 기존 COW와 DIRTY/CLEAN 상태를 이용하며 별도의 온디스크
journal 영역을 추가하지 않는다. 따라서 v1은 journal replay와 fsck를 아직
제공하지 않는다. 전원 차단처럼 group commit 도중 machine 전체가 사라지면
`DIRTY` state가 남을 수 있으며, 복구 도구가 추가되기 전에는 손상 가능성이
있다. `mkdir`, `unlink`, `rename`, 외부 extent tree, 권한 강제와 timestamp
갱신도 후속 호환 기능이다.

공용 byte offset과 상수의 기준은 `include/rmfs_format.h`, host formatter와
inspector는 `src/rmfs_image.c`, kernel driver는 `kernel/rmfs.c`다.
