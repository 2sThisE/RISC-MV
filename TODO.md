# TODO

현재 ISA, ABI v1.0, 재배치 오브젝트, 링커와 정적 라이브러리를 기준으로 한
후속 작업 목록이다. 완료된 기능을 반복해서 적지 않고 보강·추가할 항목만
우선순위순으로 관리한다.

## P0 — Clang/LLVM CVM 크로스 컴파일 경로 — 완료

- [x] 개발 호스트 Clang 22.1.8 탐지와 실제 생성 IR bootstrap 회귀 경로
- [x] MSYS2 UCRT64 LLVM/Clang 개발 도구를 22.1.8로 통일하고 PATH 확인
- [x] 재현 가능한 LLVM 개발 도구 설치 절차 문서 작성
- [x] `cvm64-unknown-none` target triple과 CVM LLVM DataLayout v1 확정
- [x] `cvmir`: LLVM 22 공식 IR parser/verifier와 진단·테스트 골격
- [x] `cvmir`: 정수 함수 인자, 산술, 반환을 CVM ABI 어셈블리로 변환
- [x] `cvmir`: i1/i8/i16/i32/i64 산술·비교·조건/무조건 분기
- [x] Clang 22 최적화 IR의 `select`와 `tail/musttail/notail call` 수용
- [x] `cvmir`: 8개 이하 정수 인자의 직접 함수 호출과 재배치 오브젝트
- [x] `cvmir`: 정수 `trunc`/`zext`/`sext` cast
- [x] `cvmir`: alloca/load/store, 포인터, GEP, PHI와 전역 데이터
- [x] scalar stack argument, 명시적 sret pointer와 variadic caller stack 규약
- [x] `cvmclang`: Clang→IR→`cvmir`→`vmasm`/`cvmlink` driver
- [x] Clang 22 호환 target profile: LP64, 정렬, macro, `long double=64`
- [x] P0 코드 생성 경로를 LLVM parser 기반 assembly bridge로 확정
- [x] CVM sysroot, `crt0.o`, 기본 stack, builtins와 `libcvm.a`
- [x] 실제 Clang 22 생성 i64 IR→CVM 기계어→VM 실행 bootstrap 검증
- [x] 정규화된 `cvm64` Clang IR, 메모리 커널과 기본 runtime 링크 회귀 테스트

P0의 완료 기준은 freestanding 정수·포인터 C 커널 기반이다. float/vector IR,
variadic callee의 `va_start/va_arg`, aggregate SSA return과 native 최적화 backend는
P2 고도화 대상으로 분리한다.

## P1 — 커널의 C 전환과 운영체제 기반

- [x] CPU 제어·상태 조회·원자 연산용 `<cvm/intrin.h>`와 `cvmir` 직접 lowering
- [x] C/assembly 혼합 링크, 전체 section 경계와 심볼 기반 bootstrap stack
- [x] 커널 entry에서 C 호출 경계를 세우고 BootInfo/CRC32/early UART를 C로 이전
- [x] kernel entry·예외 레지스터 저장·IRET·fault probe만 assembly 경계로 정리
- [x] UART, PMM, MMU, VBR와 page-fault/예외 정책을 C로 이전
- [ ] 커널 heap과 기본 자료구조 구현
- [ ] 사용자 주소 공간과 CVM 실행 이미지 loader 구현
- [ ] syscall dispatcher와 사용자 포인터 검증 구현
- [ ] 선점 가능한 스케줄러와 문맥 교환 구현
- [ ] VFS, FAT32 읽기/쓰기와 block-device 커널 드라이버 구현
- [ ] UART/키보드/디스플레이 드라이버를 커널 장치 계층에 연결

## P2 — 도구 체인 보강

- LLVM source tree에 native `cvm64` TargetInfo와 builtin 정의 추가
- CVM integer legalization 뒤 `-O1` 이상 최적화 IR 허용
- LLVM native backend/MC object writer와 LLD 포팅의 비용·성능 재평가
- float/vector IR, indirect call, switch와 memory intrinsic lowering
- variadic callee `va_start/va_arg`와 aggregate SSA return lowering
- `cvmar` archive 멤버 추가·교체·삭제 명령
- 개발 빌드용 thin archive
- `.text.*`, `.rodata.*` 같은 사용자 정의 섹션과 section garbage collection
- COMDAT/link-once 및 심볼 visibility
- ABS8/ABS16과 추가 PC-relative 재배치가 실제 코드 생성에 필요해질 때 확장
- PIC, GOT/PLT, TLS와 동적 로더는 사용자 프로세스 이후 설계
- line table, DWARF 또는 CVM 전용 디버그 정보 포맷
- 링크 map에 로컬 심볼, 타입, 크기와 archive 출처 표시
- 오브젝트/archive parser fuzzing과 손상 파일 table-overlap 검증 강화
- weak/common/overflow/archive 순서 규칙을 검증하는 독립 linker 통합 테스트

## ABI v2 후보 — v1과 섞지 않음

- C bit-field의 저장 단위와 배치 규칙
- `_Atomic` 타입과 C 메모리 순서를 ISA atomic/fence에 매핑
- callee-saved 레지스터를 도입할지 성능 측정 후 결정
- binary128 또는 별도 `long double` 포맷
- 벡터 aggregate와 homogeneous floating aggregate 전달 최적화
- stack unwinding과 예외 처리 메타데이터

ABI v1의 기존 바이너리와 호환되지 않는 변경은 문서만 조용히 수정하지 않고
ABI major version, 오브젝트 요구 버전과 도구 진단을 함께 올린다.

## 폐기된 방향 — 자체 C 컴파일러

- [x] 자체 C 컴파일러 `cvmcc`의 신규 기능 개발 중단
- `cvmcc`의 lexer, parser, AST와 코드 생성기는 완전한 C 구현으로 확장하지 않는다.
- 기존 소스와 테스트는 ABI 및 도구 체인 회귀 참고용으로만 보존한다.
- C 컴파일은 Clang 프런트엔드와 LLVM IR을 사용하고, CVM 쪽에서는 IR 변환기,
  정식 backend, ABI, sysroot와 런타임 지원에 집중한다.
- 장기 셀프 호스팅은 독자 C 컴파일러가 아니라 Clang/LLVM 자체를 CVM으로
  빌드할 수 있는 커널·사용자 공간·표준 라이브러리 기반을 갖추는 것을 목표로 한다.
