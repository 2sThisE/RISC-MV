# RISC-VM 아키텍처 명칭과 호환성 정책

이 프로젝트의 공식 CPU 아키텍처 명칭은 `RISC-VM`이다. LLVM target triple은
`riscvm64-unknown-none`이고 실행파일 포맷은 RISC-VM Executable File v1,
확장자는 `.exf`다. RISC-VM은 독자 ISA이며 RISC-V 바이너리 호환 아키텍처가
아니다.

## 실행파일 식별자

| 항목 | 값 |
|---|---|
| 확장자 | `.exf` |
| 8-byte magic | `RVMEXF01` |
| ISA ID | little-endian `RVM1` (`0x314D5652`) |
| format version | 1.0 |
| ISA version | 1 |
| address width | 64 bit |
| byte order | little-endian |

header와 segment의 전체 바이트 규격은 [BOOT_FORMAT.md](../boot/BOOT_FORMAT.md)에
정의한다. `BOOT.EXF`, `KERNEL.EXF`와 Clang으로 링크한 사용자 프로그램은 모두
같은 EXF container를 사용하고 segment 권한과 진입점만 용도에 맞게 설정한다.

## 파일 확장자

| 확장자 | 용도 |
|---|---|
| `.s`, `.asm` | RISC-VM assembly source |
| `.o` | legacy CVM-named relocatable object format |
| `.a` | legacy CVM-named static archive format |
| `.exf` | 정식 RISC-VM 실행파일 |
| `.bin` | header 없는 raw 기계어 또는 장치 데이터 |
| `.img` | raw disk image |

## 레거시 명칭

기존 `Cvm*` C 타입, `CVM_*` 상수, `cvmclang`, `cvmir`, `cvmlink`, `cvmar`와
`libcvm.a` 이름은 소스 호환성과 불필요한 대규모 rename을 피하기 위해
유지한다. 새 public 상수와 문서는 `RISC_VM_*`, `RiscVm*`, RISC-VM 명칭을
사용한다. `boot_format.h`는 기존 source 이름을 새 EXF 값의 alias로 제공한다.

자체 C 컴파일러 `cvmcc`는 이 호환 정책의 지원 대상이 아니다. 실행파일을
기본 빌드에 포함하지 않으며 기능 추가, 버그 수정 또는 ABI 변경 추적 계획이
없다. 기존 `tools/compiler`, `examples/compiler`와 관련 테스트 소스는 개발
이력과 회귀 참고를 위해 동결된 상태로만 보존한다.

이 source-level alias는 기존 실행파일 호환을 뜻하지 않는다. `CVMKERN1` magic,
`CVM1` ISA ID 또는 `.cvm` 확장자를 가진 실행파일은 linker, image tool,
disk tool, Clang driver와 loader에서 지원하지 않는다. 기존 disk image도 새
`BOOT.EXF`와 `KERNEL.EXF`를 포함하도록 다시 생성해야 한다.
