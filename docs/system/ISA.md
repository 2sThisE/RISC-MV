# RISC-MV RArchM64 instruction set

RArchM64는 RISC-MV ISA 계열의 64비트 아키텍처다.

## 기본 규칙

- opcode는 1바이트다.
- 범용 레지스터는 `R0`부터 `R15`까지 16개이며 각각 64비트다. `R15`는 스택 포인터 `SP`의 별칭이다.
- 벡터 레지스터는 `V0`부터 `V15`까지 16개이며 각각 128비트다. 정수 SIMD와 스칼라·벡터 부동소수점 명령이 이를 공유한다.
- 각 하드웨어 스레드는 독립적인 레지스터, PC, SP, FLAGS를 가진다.
- 각 하드웨어 스레드는 범용 레지스터와 분리된 `USER` 또는 `SUPERVISOR` 권한 모드를 가진다. CPU reset 직후에는 `SUPERVISOR`다.
- 각 하드웨어 스레드는 범용 레지스터와 분리된 예외 상태 레지스터 `EPC`, `ECAUSE`, `BADADDR`, `EINFO`를 가진다.
- 각 하드웨어 스레드는 user→supervisor 진입에 쓰는 별도 64비트 커널 스택 포인터 `KSP`를 가진다.
- 레지스터 번호 오퍼랜드는 1바이트지만 유효 범위는 `0x00`부터 `0x0F`다.
- 즉시값과 RAM의 다중 바이트 값은 리틀 엔디언이다.
- 정수 부호 표현은 64비트 2의 보수다.
- 주소 오퍼랜드와 `PC`, `SP`는 64비트다.
- 명령어 표의 오퍼랜드는 opcode 다음에 나오는 순서다.

## 플래그

| 이름 | 비트 | 의미 |
|---|---:|---|
| `ZERO` | 0 | 결과가 0 |
| `NEGATIVE` | 1 | 결과의 최상위 비트가 1 |
| `CARRY` | 2 | `ADD` 자리올림 또는 `SUB/CMP`에서 borrow 없음 |
| `OVERFLOW` | 3 | 부호 있는 `ADD`, `SUB`, `CMP` overflow |
| `INTERRUPT_ENABLE` | 4 | 외부 인터럽트를 받을 수 있음 |
| `UNORDERED` | 5 | 마지막 `FCMP32/64` 피연산자 중 하나가 NaN |

`ADD`, `SUB`, `CMP`는 연산 플래그 네 개를 설정한다. 논리, shift, 곱셈과 나눗셈은 결과에 맞춰 `ZERO`와 `NEGATIVE`를 설정하며 나머지 연산 플래그는 초기화한다. 단, `MUL`은 부호 없는 곱셈의 상위 비트가 잘린 경우 `CARRY`도 설정한다. 모든 산술·논리 명령은 `INTERRUPT_ENABLE`을 보존한다.

## 명령어

| Opcode | 명령어 | 오퍼랜드 | 동작 |
|---:|---|---|---|
| `00` | `HALT` | 없음 | 현재 하드웨어 스레드를 영구 종료, supervisor 전용 |
| `01` | `MOVI64` | `dst, imm64` | 64비트 즉시값 저장 |
| `02` | `ADD` | `dst, src` | `dst = dst + src` |
| `03` | `LOAD8U` | `dst, addr_reg` | 1바이트 읽기, 0 확장 |
| `04` | `STORE8` | `addr_reg, src` | 하위 1바이트 저장 |
| `05` | `JUMP` | `target64` | 절대주소 분기 |
| `06` | `CALL` | `target64` | 반환 PC를 push하고 분기 |
| `07` | `RET` | 없음 | 반환 PC를 pop |
| `08` | `MOV` | `dst, src` | 레지스터 복사 |
| `09` | `LOAD16U` | `dst, addr_reg` | 2바이트 읽기, 0 확장 |
| `0A` | `LOAD32U` | `dst, addr_reg` | 4바이트 읽기, 0 확장 |
| `0B` | `LOAD64` | `dst, addr_reg` | 8바이트 읽기 |
| `0C` | `LOAD8S` | `dst, addr_reg` | 1바이트 읽기, 부호 확장 |
| `0D` | `LOAD16S` | `dst, addr_reg` | 2바이트 읽기, 부호 확장 |
| `0E` | `LOAD32S` | `dst, addr_reg` | 4바이트 읽기, 부호 확장 |
| `0F` | `STORE16` | `addr_reg, src` | 하위 2바이트 저장 |
| `10` | `STORE32` | `addr_reg, src` | 하위 4바이트 저장 |
| `11` | `STORE64` | `addr_reg, src` | 8바이트 저장 |
| `12` | `SUB` | `dst, src` | `dst = dst - src` |
| `13` | `MUL` | `dst, src` | 하위 64비트 곱셈 |
| `14` | `DIVU` | `dst, src` | 부호 없는 나눗셈 |
| `15` | `DIVS` | `dst, src` | 부호 있는 나눗셈 |
| `16` | `MODU` | `dst, src` | 부호 없는 나머지 |
| `17` | `MODS` | `dst, src` | 부호 있는 나머지 |
| `18` | `AND` | `dst, src` | 비트 AND |
| `19` | `OR` | `dst, src` | 비트 OR |
| `1A` | `XOR` | `dst, src` | 비트 XOR |
| `1B` | `NOT` | `dst` | 비트 반전 |
| `1C` | `SHL` | `dst, amount8` | 논리 왼쪽 shift |
| `1D` | `SHR` | `dst, amount8` | 논리 오른쪽 shift |
| `1E` | `SAR` | `dst, amount8` | 부호 유지 오른쪽 shift |
| `1F` | `CMP` | `lhs, rhs` | `lhs - rhs` 플래그만 설정 |
| `20` | `JZ` | `target64` | 같거나 결과가 0이면 분기 |
| `21` | `JNZ` | `target64` | 다르거나 결과가 0이 아니면 분기 |
| `22` | `JLT` | `target64` | signed `<` |
| `23` | `JLE` | `target64` | signed `<=` |
| `24` | `JGT` | `target64` | signed `>` |
| `25` | `JGE` | `target64` | signed `>=` |
| `26` | `JLTU` | `target64` | unsigned `<` |
| `27` | `JLEU` | `target64` | unsigned `<=` |
| `28` | `JGTU` | `target64` | unsigned `>` |
| `29` | `JGEU` | `target64` | unsigned `>=` |
| `2A` | `PUSH` | `src` | 64비트 레지스터를 스택에 저장 |
| `2B` | `POP` | `dst` | 스택에서 64비트 레지스터 복원 |
| `2C` | `NOP` | 없음 | 아무 동작 없이 다음 명령어로 이동 |
| `2D` | `IRET` | 없음 | 인터럽트 프레임의 PC, FLAGS, 권한 모드를 복원, supervisor 전용 |
| `2E` | `EI` | 없음 | 외부 인터럽트 허용, supervisor 전용 |
| `2F` | `DI` | 없음 | 외부 인터럽트 차단, supervisor 전용 |
| `30` | `CAS64` | `addr, expected, desired, result` | 정렬된 RAM을 비교 후 교환하고 이전 값을 `result`에 저장 |
| `31` | `XCHG64` | `addr, value` | 정렬된 RAM과 `value`를 원자적으로 교환 |
| `32` | `ATOMIC_ADD64` | `addr, value, result` | 정렬된 RAM에 `value`를 더하고 이전 값을 `result`에 저장 |
| `33` | `FENCE` | 없음 | 순차 일관성 메모리 fence |
| `34` | `COREID` | `dst` | 현재 가상 코어 번호 저장 |
| `35` | `THREADID` | `dst` | 현재 코어 안의 하드웨어 스레드 번호 저장 |
| `36` | `WAIT` | 없음 | 외부 인터럽트가 올 때까지 현재 하드웨어 스레드 대기 |
| `37` | `EXCAUSE` | `dst` | `ECAUSE`를 저장 |
| `38` | `EXADDR` | `dst` | `BADADDR`를 저장 |
| `39` | `SETVBR` | `src` | VBR을 설정하고 RAM 벡터 테이블 활성화, supervisor 전용 |
| `3A` | `GETVBR` | `dst` | 현재 VBR 값을 저장, supervisor 전용 |
| `3B` | `GETMODE` | `dst` | 현재 권한 모드(`USER=0`, `SUPERVISOR=1`)를 저장 |
| `3C` | `ENTERUSER` | 없음 | 현재 스레드를 user 모드로 낮춤, supervisor 전용 |
| `3D` | `SETPTBR` | `src` | 물리 RAM의 루트 페이지 테이블 주소를 설정, supervisor 전용 |
| `3E` | `GETPTBR` | `dst` | 현재 PTBR을 저장, supervisor 전용 |
| `3F` | `MMUON` | 없음 | 다음 명령부터 가상주소 변환 활성화, supervisor 전용 |
| `40` | `MMUOFF` | 없음 | 다음 명령부터 물리주소 직접 접근, supervisor 전용 |
| `41` | `GETMMU` | `dst` | MMU가 켜져 있으면 1, 꺼져 있으면 0을 저장 |
| `42` | `EXPC` | `dst` | `EPC`를 저장 |
| `43` | `EXINFO` | `dst` | `EINFO`를 저장 |
| `44` | `JUMPR` | `target` | 레지스터가 가리키는 주소로 분기 |
| `45` | `CALLR` | `target` | 반환 PC를 push하고 레지스터 주소로 분기 |
| `46` | `SHLV` | `dst, amount_reg` | 레지스터 하위 6비트만큼 왼쪽 shift |
| `47` | `SHRV` | `dst, amount_reg` | 레지스터 하위 6비트만큼 논리 오른쪽 shift |
| `48` | `SARV` | `dst, amount_reg` | 레지스터 하위 6비트만큼 부호 유지 오른쪽 shift |
| `49` | `MOVI32U` | `dst, imm32` | 32비트 즉시값을 0 확장하여 저장 |
| `4A` | `MOVI32S` | `dst, imm32` | 32비트 즉시값을 부호 확장하여 저장 |
| `4B` | `ADDI32` | `dst, imm32` | 부호 확장한 즉시값을 더하고 플래그 설정 |
| `4C` | `CMPI32` | `lhs, imm32` | 부호 확장한 즉시값과 비교하고 플래그만 설정 |
| `4D` | `ANDI32` | `dst, imm32` | 0 확장한 즉시값과 AND |
| `4E` | `ORI32` | `dst, imm32` | 0 확장한 즉시값과 OR |
| `4F` | `XORI32` | `dst, imm32` | 0 확장한 즉시값과 XOR |
| `50` | `TESTI32` | `lhs, imm32` | AND 결과의 플래그만 설정하고 레지스터는 보존 |
| `51` | `LEA` | `dst, base, disp32` | `base + signed disp32` 주소 계산, 플래그 보존 |
| `52` | `LOAD8UO` | `dst, base, disp32` | offset 주소에서 1바이트 읽고 0 확장 |
| `53` | `LOAD16UO` | `dst, base, disp32` | offset 주소에서 2바이트 읽고 0 확장 |
| `54` | `LOAD32UO` | `dst, base, disp32` | offset 주소에서 4바이트 읽고 0 확장 |
| `55` | `LOAD64O` | `dst, base, disp32` | offset 주소에서 8바이트 읽기 |
| `56` | `LOAD8SO` | `dst, base, disp32` | offset 주소에서 1바이트 읽고 부호 확장 |
| `57` | `LOAD16SO` | `dst, base, disp32` | offset 주소에서 2바이트 읽고 부호 확장 |
| `58` | `LOAD32SO` | `dst, base, disp32` | offset 주소에서 4바이트 읽고 부호 확장 |
| `59` | `STORE8O` | `base, src, disp32` | offset 주소에 하위 1바이트 저장 |
| `5A` | `STORE16O` | `base, src, disp32` | offset 주소에 하위 2바이트 저장 |
| `5B` | `STORE32O` | `base, src, disp32` | offset 주소에 하위 4바이트 저장 |
| `5C` | `STORE64O` | `base, src, disp32` | offset 주소에 8바이트 저장 |
| `5D` | `JUMPREL` | `disp32` | 다음 PC 기준 signed 상대 분기 |
| `5E` | `BRCC` | `condition, disp32` | 조건이 맞으면 다음 PC 기준 signed 상대 분기 |
| `5F` | `CALLREL` | `disp32` | 반환 PC를 push하고 다음 PC 기준 상대 호출 |
| `60` | `SEXT8` | `dst, src` | `src` 하위 8비트를 64비트로 부호 확장 |
| `61` | `SEXT16` | `dst, src` | `src` 하위 16비트를 64비트로 부호 확장 |
| `62` | `SEXT32` | `dst, src` | `src` 하위 32비트를 64비트로 부호 확장 |
| `63` | `ZEXT8` | `dst, src` | `src` 하위 8비트를 64비트로 0 확장 |
| `64` | `ZEXT16` | `dst, src` | `src` 하위 16비트를 64비트로 0 확장 |
| `65` | `ZEXT32` | `dst, src` | `src` 하위 32비트를 64비트로 0 확장 |
| `66` | `SYSCALL` | 없음 | syscall 벡터로 동기 진입하고 다음 PC로 복귀할 프레임 생성 |
| `67` | `SETKSP` | `src` | 현재 하드웨어 스레드의 KSP 설정, supervisor 전용 |
| `68` | `GETKSP` | `dst` | 현재 하드웨어 스레드의 KSP 읽기, supervisor 전용 |
| `69` | `VLOAD128` | `vdst, addr` | RAM에서 128비트를 읽어 벡터 레지스터에 저장 |
| `6A` | `VSTORE128` | `addr, vsrc` | 벡터 레지스터 128비트를 RAM에 저장 |
| `6B` | `VMOV` | `vdst, vsrc` | 128비트 벡터 복사 |
| `6C`~`6F` | `VADD8/16/32/64` | `vdst, va, vb` | 해당 폭의 unsigned lane별 modulo 덧셈 |
| `70`~`73` | `VSUB8/16/32/64` | `vdst, va, vb` | 해당 폭의 unsigned lane별 modulo 뺄셈 |
| `74`~`77` | `VMUL8/16/32/64` | `vdst, va, vb` | 해당 폭의 unsigned lane별 하위 비트 곱셈 |
| `78` | `VAND` | `vdst, va, vb` | 128비트 AND |
| `79` | `VOR` | `vdst, va, vb` | 128비트 OR |
| `7A` | `VXOR` | `vdst, va, vb` | 128비트 XOR |
| `7B` | `FLOAD32` | `vdst, addr` | IEEE-754 binary32를 최하위 lane에 읽고 상위 비트는 0으로 설정 |
| `7C` | `FLOAD64` | `vdst, addr` | IEEE-754 binary64를 최하위 lane에 읽고 상위 비트는 0으로 설정 |
| `7D` | `FSTORE32` | `addr, vsrc` | 최하위 binary32 lane 저장 |
| `7E` | `FSTORE64` | `addr, vsrc` | 최하위 binary64 lane 저장 |
| `7F`~`86` | `FADD/FSUB/FMUL/FDIV32/64` | `vdst, va, vb` | 최하위 float/double lane 스칼라 연산, 상위 비트는 0으로 설정 |
| `87`~`88` | `FCMP32/64` | `va, vb` | 최하위 실수 lane을 비교해 FLAGS 설정 |
| `89`~`8A` | `FNEG32/64` | `vdst, vsrc` | 최하위 실수 lane의 부호 비트 반전 |
| `8B`~`8C` | `FABS32/64` | `vdst, vsrc` | 최하위 실수 lane의 부호 비트 제거 |
| `8D` | `I32TOF32` | `vdst, src` | 범용 레지스터 하위 signed 32비트를 binary32로 변환 |
| `8E` | `I64TOF64` | `vdst, src` | signed 64비트를 binary64로 변환 |
| `8F` | `F32TOI32` | `dst, vsrc` | binary32를 0 방향으로 잘라 signed 32비트로 변환 후 부호 확장 |
| `90` | `F64TOI64` | `dst, vsrc` | binary64를 0 방향으로 잘라 signed 64비트로 변환 |
| `91`~`98` | `VFADD/VFSUB/VFMUL/VFDIV32/64` | `vdst, va, vb` | binary32 4개 또는 binary64 2개의 lane별 연산 |
| `99` | `F32TOF64` | `vdst, vsrc` | 최하위 binary32를 binary64로 변환 |
| `9A` | `F64TOF32` | `vdst, vsrc` | 최하위 binary64를 binary32로 변환 |
| `9B` | `GETFPSTATUS` | `dst` | 현재 스레드의 sticky 부동소수점 상태를 범용 레지스터로 읽기 |
| `9C` | `CLEARFPSTATUS` | 없음 | sticky 부동소수점 상태를 모두 초기화 |

## 확장 정수·주소 명령 규칙

`imm32`와 `disp32`는 리틀 엔디언 4바이트다. `MOVI32S`, `ADDI32`, `CMPI32`, `LEA`, offset 메모리 명령과 상대 분기는 이를 64비트로 부호 확장한다. 논리 즉시값 명령은 0 확장한다. 주소 덧셈은 64비트 modulo 연산이며 최종 LOAD·STORE·분기 대상은 기존 MMU와 Bus 검사를 그대로 받는다.

`LEA`, `MOVI32U/S`, LOAD·STORE와 `SEXT/ZEXT`는 FLAGS를 변경하지 않는다. `ADDI32`는 `ADD`, `CMPI32`는 `CMP`, 즉시 논리와 가변 shift는 기존 논리·shift 명령과 같은 플래그 규칙을 사용한다.

상대 분기의 기준은 opcode와 모든 operand를 읽은 뒤의 PC다. 따라서 `disp32=0`이면 바로 다음 명령을 가리킨다. `CALLREL`이 stack에 저장하는 반환 주소도 이 PC다. `JUMPR`와 `CALLR`은 대상 레지스터 값을 stack 변경 전에 읽고 실행 가능한 주소인지 검사한다.

`BRCC`의 condition 값은 다음과 같다.

| 값 | 이름 | 조건 |
|---:|---|---|
| `0` | `ALWAYS` | 항상 |
| `1` | `EQ` | 같음/zero |
| `2` | `NE` | 다름/non-zero |
| `3` | `LT` | signed `<` |
| `4` | `LE` | signed `<=` |
| `5` | `GT` | signed `>` |
| `6` | `GE` | signed `>=` |
| `7` | `LTU` | unsigned `<` |
| `8` | `LEU` | unsigned `<=` |
| `9` | `GTU` | unsigned `>` |
| `10` | `GEU` | unsigned `>=` |
| `11` | `ORD` | 마지막 실수 비교가 ordered, 즉 두 값 모두 NaN이 아님 |
| `12` | `UNO` | 마지막 실수 비교가 unordered, 즉 피연산자 중 하나가 NaN |

존재하지 않는 condition 값은 `ILLEGAL_INSTRUCTION` 예외를 발생시킨다. `SHLV`, `SHRV`, `SARV`는 shift 횟수 레지스터의 하위 6비트만 사용하므로 모든 입력이 `0~63` 범위로 정규화된다.

## SIMD와 부동소수점

128비트 벡터 레지스터에서 byte offset 0이 최하위 lane이며 `VLOAD128`이 읽는 가장 낮은 메모리 주소에 대응한다. 정수 SIMD 연산은 포화 연산이 아니라 각 lane 폭에서 잘리는 modulo 연산이다. SIMD 연산은 일반 정수 FLAGS를 변경하지 않는다. 목적 레지스터가 입력 레지스터와 같아도 모든 입력을 읽은 뒤 결과를 기록한다.

`VLOAD128/VSTORE128`은 비정렬 주소와 MMU 페이지 경계 통과를 지원하지만 일반 RAM 전용이다. 주소 전체의 변환과 RAM 범위를 먼저 검사하므로 실패한 load는 목적 벡터를 보존하고 실패한 store는 일부 바이트만 기록하지 않는다. MMIO는 `LOAD/STORE8/16/32/64` 또는 `FLOAD/FSTORE32/64`를 사용한다.

부동소수점 형식은 IEEE-754 binary32/binary64이며 호스트의 기본 round-to-nearest 동작을 사용한다. `FDIV`의 0 나누기는 정수 나눗셈 예외를 발생시키지 않고 IEEE-754 infinity 또는 NaN을 만든다. `FCMP`에서 NaN을 만나면 `UNORDERED`가 설정되고 `EQ/LT/LE/GT/GE`는 거짓, `NE`와 `UNO`는 참이다. `ORD`는 그 반대다.

`FPSTATUS`는 범용 레지스터 및 FLAGS와 분리된 하드웨어 스레드별 sticky 상태다. 비트는 `0=INVALID`, `1=DIVIDE_BY_ZERO`, `2=OVERFLOW`, `3=UNDERFLOW`다. 한번 설정된 비트는 `CLEARFPSTATUS` 전까지 유지된다. 현재 구현은 NaN 결과와 NaN 비교를 `INVALID`, 유한한 0이 아닌 값을 0으로 나눈 경우를 `DIVIDE_BY_ZERO`, 유한 입력의 무한대 결과를 `OVERFLOW`, 0이 아닌 subnormal 결과를 `UNDERFLOW`로 기록한다. 별도 부동소수점 트랩과 `INEXACT`, 반올림 모드 변경은 아직 지원하지 않는다. 범위를 벗어난 float→integer 변환은 최소·최대 정수로 포화하고 `INVALID`를 설정하며 NaN은 0으로 변환한다.

`CAS64`는 교환에 성공하면 `ZERO`를 설정하고 실패하면 해제한다. `XCHG64`는 입력 레지스터에 이전 메모리 값을 돌려준다. 원자적 64비트 명령은 8바이트로 정렬된 일반 RAM에서만 사용할 수 있으며 MMIO에는 사용할 수 없다.

## 권한 모드

권한 모드는 `R0`~`R15`나 FLAGS에 들어 있지 않은 CPU 전용 상태다. 모든 하드웨어 스레드는 reset 직후 `SUPERVISOR`로 시작한다. `ENTERUSER`는 권한을 `USER`로 낮추기만 하며 user 코드가 명령어로 supervisor 권한을 직접 얻는 방법은 없다.

`HALT`, `IRET`, `EI`, `DI`, `SETVBR`, `GETVBR`, `ENTERUSER`, `SETPTBR`, `GETPTBR`, `MMUON`, `MMUOFF`, `SETKSP`, `GETKSP`는 privileged 명령이다. user 모드에서 실행하면 명령의 부작용 없이 `PRIVILEGE_VIOLATION` 동기 예외가 발생한다. `SYSCALL`은 user 모드에서 supervisor로 들어가기 위해 의도적으로 허용된다. `GETMODE`, `GETMMU`, `EXPC`, `EXCAUSE`, `EXADDR`, `EXINFO`는 두 모드에서 모두 실행할 수 있지만 실제 CPU 상태를 변경하지 않고 숫자를 범용 레지스터로 복사하기만 한다.

user 모드에서 외부 IRQ·동기 예외·시스템 콜이 전달되면 CPU는 R15의 user SP를 보존하고 KSP로 전환한 뒤 자동으로 `SUPERVISOR`가 된다. `IRET`은 user SP와 이전 권한 모드를 복원한다. supervisor에서 중첩 진입할 때는 현재 R15를 그대로 사용한다. MMU가 켜져 있으면 user 모드는 `USER` PTE가 설정된 페이지에만 접근할 수 있다. MMU가 꺼져 있으면 두 모드 모두 물리 주소를 직접 사용하므로 privileged ISA 검사 외의 메모리 격리는 없다.

## 클럭과 외부 인터럽트

CPU는 명령어 하나를 완전히 실행한 다음 아래 순서로 외부 장치를 확인한다.

1. `ClockSource.poll`에서 지난 나노초 tick 수를 받는다.
2. `Bus`가 tick을 등록된 장치에 전달한다.
3. 타이머 같은 장치가 IRQ Controller에 외부 IRQ를 올린다.
4. controller가 mask와 route를 확인하여 대상 하드웨어 스레드에 전달한다.
5. 인터럽트가 허용되어 있으면 CPU가 전달된 pending IRQ를 하나 가져온다.
6. 현재 FLAGS, 권한 모드와 다음 PC를 스택 프레임에 저장하고 supervisor 모드의 핸들러로 분기한다.
7. 핸들러가 장치를 ACK하고 controller에 EOI한 뒤 `IRET`하면 이전 상태가 복구된다.

타이머 tick의 공식 단위는 나노초이며 주파수는 1GHz다. 단일 CPU용
`cpu_run()`에서 `InstructionClock`을 사용하면 명령어 하나가 끝날 때 설정된 수의
나노초 tick을 반환하므로 테스트를 실제 시간과 독립적으로 재현할 수 있다.
멀티코어 `vm_run()`은 코어 worker와 분리된 장치 worker가
`QueryPerformanceCounter` 또는 `CLOCK_MONOTONIC`에서 구한 호스트 단조 시간의
경과 나노초를 전달한다. 장치 worker는 고정밀 wait를 사용해 100us마다 시간을
반영한다. IRQ 주기는 클럭이 아니라 `TimerDevice`의 compare 레지스터가 결정한다.

```c
InterruptController interrupts;
interrupt_controller_init(&interrupts);

ram_write(&ram,
          vector_base + TIMER_INTERRUPT_LINE * 8,
          8,
          0x200);
cpu_set_vector_base(&cpu, &ram, vector_base);

Bus bus;
bus_init(&bus, &ram);

TimerDevice timer;
timer_device_init(&timer, TIMER_INTERRUPT_LINE);
bus_map_device(&bus, timer_base, TIMER_MMIO_SIZE,
               timer_device_as_bus_device(&timer));

InstructionClock instruction_clock;
ClockSource clock;
instruction_clock_init(&instruction_clock, &clock, 1);

cpu_run(&cpu, &bus, &interrupts, &clock);
```

CPU는 초기화 직후 인터럽트가 차단되어 있고 IRQ Controller의 외부 IRQ도 모두 masked 상태다. 게스트는 VBR과 벡터를 설치하고 controller에서 IRQ를 활성화한 뒤 `EI`해야 한다. VBR이나 해당 테이블 항목이 잘못되면 IRQ를 전달할 수 없어 실행 오류가 된다. 동일한 IRQ가 처리되기 전에 여러 번 발생하면 하나로 합쳐진다. Controller 레지스터와 EOI 규칙은 `IRQ_CONTROLLER.md`에 정의되어 있다.

`WAIT`를 실행한 스레드는 `WAITING` 상태가 되어 일반 명령 실행에서 제외되지만 활성 스레드 수에는 계속 포함된다. IRQ가 pending 되고 `INTERRUPT_ENABLE`이 설정되어 있으면 인터럽트 프레임을 만든 뒤 `RUNNABLE`로 돌아가 핸들러를 실행한다. `IRET`은 WAIT 다음 주소로 복귀한다. `DI` 상태에서 WAIT하면 일반 외부 IRQ로는 깨어날 수 없다. `HALT`는 종료 명령이므로 IRQ로 깨어나지 않는다.

멀티코어 장치 worker는 `bus_tick()`을 한 번만 호출하므로 코어 수가 늘어도 타이머가 빨라지지 않는다. 각 코어는 VM 명령어가 끝날 때 자기 IRQ pending을 확인한다.

## VBR과 통합 벡터 테이블

각 하드웨어 스레드는 독립적인 64비트 VBR(Vector Base Register)을 가진다. 초기에는 비활성 상태이며, supervisor 모드에서 8바이트 정렬된 물리 RAM 주소를 `SETVBR`로 지정하면 활성화된다. 현재 테이블은 77개의 64비트 핸들러 주소로 구성되어 총 616바이트다.

| 벡터 인덱스 | 용도 |
|---:|---|
| `0`~`47` | 외부 장치 IRQ 0~47 |
| `48`~`63` | IPI 0~15, 외부 IRQ보다 높은 우선순위 class |
| `64` | 예약됨 |
| `65`~`75` | 동기 예외 코드 1~11 |
| `76` | 시스템 콜 |

CPU는 MMU를 우회하여 물리 주소 `VBR + vector_index * 8`에서 리틀 엔디언 64비트 핸들러 주소를 읽는다. 사용하지 않는 항목에는 `UINT64_MAX`를 넣을 수 있으며, 이 값은 유효하지 않은 벡터로 처리된다. VBR 테이블 전체는 물리 RAM 안에 있어야 한다. 테이블에 저장된 handler PC는 MMU가 켜져 있으면 가상 주소이므로 supervisor 실행 권한 매핑이 필요하다.

```text
MOVI64 R0, vector_table_address
SETVBR R0
GETVBR R1
```

외부 IRQ와 IPI는 IRQ 번호를 그대로 벡터 인덱스로 사용한다. 동기 예외는 `64 + EXCAUSE`, `SYSCALL`은 인덱스 `76`을 사용한다. VBR과 테이블은 하드웨어가 조회하고, 각 RAM 항목에 실제 핸들러 주소를 기록하는 것은 게스트 소프트웨어가 담당한다.

## 시스템 콜

`SYSCALL`은 CPU가 서비스 내용을 직접 처리하는 명령이 아니라 게스트 운영체제로 안전하게 제어를 넘기는 동기 트랩이다. CPU는 범용 레지스터를 해석하거나 변경하지 않으므로 syscall 번호, 인자와 반환값 ABI는 게스트 운영체제가 정한다. 권장 초기 규약은 `R0=번호/반환값`, `R1`~`R6`=인자다.

supervisor 부트 코드는 user 프로그램을 시작하기 전에 각 하드웨어 스레드의 VBR, syscall 벡터와 KSP를 설정해야 한다.

```asm
MOVI64 R10, kernel_stack_top
SETKSP R10
MOVI64 R11, vector_table
SETVBR R11
ENTERUSER

; user code
MOVI32U R0, 1
MOVI64  R1, buffer
MOVI32U R2, 5
SYSCALL
```

user 모드의 `SYSCALL`은 다음 명령 주소, FLAGS/이전 모드, user SP를 KSP에 24바이트 프레임으로 저장하고 인터럽트를 차단한 뒤 `VBR + 76*8`의 supervisor 핸들러로 이동한다. 핸들러의 `IRET`은 KSP와 user SP를 복구하고 `SYSCALL` 다음 명령으로 돌아간다. 일반 동기 예외와 달리 실패한 명령 주소로 돌아가지 않으므로 syscall이 반복 실행되지 않는다.

supervisor 모드에서도 `SYSCALL`을 실행할 수 있으며 이때는 현재 R15에 기존 16바이트 프레임을 만든다. syscall 벡터가 비활성 또는 유효하지 않으면 `ILLEGAL_INSTRUCTION` 예외로 전달된다. KSP가 프레임을 저장할 수 없으면 `STACK_FAULT`가 발생하지만 같은 잘못된 KSP로 예외 프레임도 만들 수 없으므로 실행이 중단될 수 있다. `WRITE`, `EXIT`, 파일 핸들 검사와 사용자 포인터 검증 같은 실제 서비스는 CPU가 아니라 게스트 운영체제의 syscall 핸들러가 구현한다.

`examples/syscall/syscall_demo.asm`은 `SYS_WRITE`를 UART 한 바이트 출력으로 처리하고 `SYS_EXIT`을 supervisor `HALT`로 처리하는 최소 게스트 커널 예제다.

```powershell
.\build.ps1 -e syscall
.\build\main.exe -r 4096 -l .\build\examples\syscall_demo.bin
```

## 동기 CPU 예외

명령 실행 중 오류가 발생하면 설정된 예외 벡터로 동기 분기한다. 외부 IRQ와 달리 `INTERRUPT_ENABLE` 상태와 관계없이 발생한다.

| 코드 | 이름 | 발생 조건 |
|---:|---|---|
| `1` | `ILLEGAL_INSTRUCTION` | 존재하지 않는 opcode, 레지스터 번호 또는 잘못된 shift 값 |
| `2` | `INSTRUCTION_ACCESS` | 명령어·오퍼랜드 fetch 또는 분기 목적지가 RAM 밖임 |
| `3` | `DATA_ACCESS` | LOAD·STORE·atomic 주소 또는 MMIO 접근이 잘못됨 |
| `4` | `DIVIDE_BY_ZERO` | 정수 나눗셈 또는 나머지의 divisor가 0 |
| `5` | `ARITHMETIC_OVERFLOW` | 표현할 수 없는 signed division |
| `6` | `STACK_FAULT` | PUSH·POP·CALL·RET·IRET의 스택 접근이 RAM 밖임 |
| `7` | `PRIVILEGE_VIOLATION` | user 모드에서 privileged 명령을 실행함 |
| `8` | `INSTRUCTION_PAGE_FAULT` | 명령 fetch·분기 목적지의 가상 페이지가 없거나 실행 권한이 없음 |
| `9` | `LOAD_PAGE_FAULT` | 읽을 가상 페이지가 없거나 읽기·user 권한이 없음 |
| `10` | `STORE_PAGE_FAULT` | 쓸 가상 페이지가 없거나 쓰기·user 권한이 없음 |
| `11` | `MMU_CONFIGURATION` | PTBR 또는 MMU 전환 뒤의 다음 PC가 유효하지 않음 |

예외가 처음 pending 되는 순간 CPU는 아래 네 상태 레지스터를 동시에 갱신한다. 이 값들은 `IRET` 뒤에도 마지막 동기 예외 정보로 남으며 reset 때 모두 0으로 초기화된다. 외부 IRQ는 이 레지스터를 변경하지 않으므로 동기 예외 handler 실행 중 IRQ가 중첩되어도 원래 예외 정보가 보존된다.

| 레지스터 | 읽기 명령 | 의미 |
|---|---|---|
| `EPC` | `EXPC dst` | 예외를 발생시킨 명령의 시작 PC |
| `ECAUSE` | `EXCAUSE dst` | 동기 예외 코드 |
| `BADADDR` | `EXADDR dst` | 잘못 접근한 주소. `EINFO.BADADDR_VALID=0`이면 의미 없음 |
| `EINFO` | `EXINFO dst` | 접근 종류, 발생 모드, MMU 실패 상세 정보 |

`EINFO` 비트는 서로 조합될 수 있다. atomic 접근은 `READ`, `WRITE`, `ATOMIC`이 함께 설정된다.

| 비트 | 이름 | 의미 |
|---:|---|---|
| `0` | `BADADDR_VALID` | `BADADDR`에 유효한 주소가 들어 있음 |
| `1` | `ACCESS_READ` | 읽기 접근 중 발생 |
| `2` | `ACCESS_WRITE` | 쓰기 접근 중 발생 |
| `3` | `ACCESS_EXECUTE` | 명령 fetch 또는 분기 대상 검사 중 발생 |
| `4` | `ACCESS_ATOMIC` | atomic 접근 중 발생 |
| `5` | `ORIGIN_USER` | 예외 발생 당시 user 모드였음 |
| `6` | `MMU_ENABLED` | 예외 발생 당시 MMU가 켜져 있었음 |
| `8` | `PAGE_INVALID_VA` | 가상주소가 지원 범위를 벗어남 |
| `9` | `PAGE_INVALID_ROOT` | PTBR 또는 페이지 테이블 루트가 잘못됨 |
| `10` | `PAGE_NOT_PRESENT` | 유효한 PTE가 없음 |
| `11` | `PAGE_MALFORMED` | PTE 형식이나 다음 단계 테이블이 잘못됨 |
| `12` | `PAGE_PERMISSION` | R/W/X 또는 USER 권한이 부족함 |

나눗셈 오류, 산술 overflow, 잘못된 opcode처럼 특정 메모리 주소와 관계없는 예외는 `BADADDR=0`, `BADADDR_VALID=0`이다. 잘못된 opcode나 레지스터 오퍼랜드는 `EPC`로 문제 명령을 찾는다.

supervisor에서 발생한 예외는 FLAGS, 이전 권한 모드와 `EPC`를 현재 R15의 16바이트 프레임에 저장한다. user에서 발생하면 KSP로 전환하여 `EPC`, FLAGS/모드, user SP 순서의 24바이트 프레임을 저장한다. 두 경우 모두 인터럽트를 차단한 뒤 supervisor handler로 이동한다. 저장 FLAGS의 bit 62는 이전 모드가 `USER`였음을 표시하고 bit 63은 동기 예외 프레임임을 표시하는 내부 비트다. `IRET`은 두 내부 비트를 해석한 뒤 실제 FLAGS에서는 제거한다. 따라서 handler가 저장 FLAGS를 수정한다면 bit 62와 63은 보존해야 한다. 이 구분 덕분에 예외 처리 중 IRQ가 중첩되어도 IRQ의 `IRET`이 바깥쪽 예외 상태를 해제하지 않는다. `IRET`은 실패한 명령의 PC로 돌아가므로, 원인을 고친 뒤 재실행하거나 스택에 저장된 PC를 수정해 명령을 건너뛸 수 있다.

게스트는 VBR 테이블의 `64 + 예외 코드` 항목에 핸들러 주소를 기록한다. VBR이 비활성화됐거나 항목이 잘못됐거나, 예외 처리 도중 재귀 예외가 발생하거나, 프레임을 저장할 수 없는 잘못된 supervisor SP/KSP이면 복구할 수 없는 실행 실패다.

## MMU와 페이지 테이블

각 하드웨어 스레드는 범용 레지스터와 분리된 64비트 PTBR과 MMU 활성화 상태를 가진다. CPU reset 직후에는 PTBR이 설정되지 않고 MMU는 꺼져 있다. `main.c`와 VM은 페이지 테이블을 자동 생성하거나 RAM을 예약하지 않는다. 게스트 supervisor 부트 코드가 물리 RAM에 페이지 테이블을 작성하고 `SETPTBR`, `MMUON`을 실행해야 한다.

MMU가 꺼져 있으면 PC, R15와 주소 레지스터의 값이 물리 주소로 바로 사용된다. MMU가 켜져 있으면 명령 fetch, 분기 목적지, 일반 LOAD·STORE, 스택과 atomic 주소가 모두 가상 주소가 된다. 페이지 테이블 walker와 VBR 테이블 조회만 물리 RAM을 직접 사용한다.

기본 페이지 크기는 4KiB이며 현재 가상 주소는 unsigned 39비트 범위 `0x0`~`0x7FFFFFFFFF`다. 3단계 테이블은 단계마다 512개의 64비트 PTE를 가진다. L0 leaf는 4KiB, L1 leaf는 2MiB, L2 leaf는 1GiB를 매핑한다.

```text
가상 주소
┌─────────┬─────────┬─────────┬────────────┐
│ L2 9bit │ L1 9bit │ L0 9bit │ offset 12 │
└─────────┴─────────┴─────────┴────────────┘
```

PTBR은 4KiB 정렬된 L2 테이블의 물리 주소다. L2와 L1 엔트리에 R/W/X 권한이 하나도 없으면 다음 단계 테이블을 가리키며, 이때 물리 주소와 `VALID`만 가져야 한다. R/W/X 중 하나 이상이 있으면 해당 단계의 leaf다. L2 leaf의 물리 주소는 1GiB, L1 leaf는 2MiB, L0 leaf는 4KiB 정렬이어야 하며 정렬에 포함되는 주소 하위 비트가 0이 아니면 malformed page fault가 발생한다.

| PTE 비트 | 이름 | 의미 |
|---:|---|---|
| `0` | `VALID` | 엔트리가 존재함 |
| `1` | `READ` | 읽기 허용 |
| `2` | `WRITE` | 쓰기 허용 |
| `3` | `EXECUTE` | 명령 실행 허용 |
| `4` | `USER` | user 모드 접근 허용 |
| `5`~`11` | 예약 | 0이어야 함 |
| `12`~`63` | 물리 페이지 주소 | 하위 12비트가 0인 주소 |

모든 단계의 leaf는 `READ`, `WRITE`, `EXECUTE` 중 하나 이상을 가져야 하며 `WRITE`는 `READ`와 함께 설정해야 한다. supervisor도 R/W/X 권한 검사는 받지만 `USER` 비트는 필요하지 않다. user 모드는 요청 권한과 함께 `USER`도 설정돼 있어야 한다. atomic 명령은 `READ|WRITE`를 모두 요구한다.

일반 RAM을 가리키는 leaf는 해당 leaf 크기 전체가 실제 RAM 안에 있어야 한다. RAM 크기가 4KiB 배수가 아니면 마지막 자투리 영역은 MMU가 켜진 상태에서 페이지로 매핑할 수 없다. RAM 범위 밖의 정렬된 물리 주소는 Bus의 MMIO를 가리킬 수 있으므로 walker가 허용하며 실제 접근 가능 여부는 Bus가 최종 검사한다.

`SETPTBR`은 루트 테이블이 정렬되고 4KiB 전체가 물리 RAM 안에 있는지 검사한다. MMU가 이미 켜져 있을 때 PTBR을 바꾸려면 새 테이블로 다음 PC를 실행할 수 있어야 한다. `MMUON`도 설정된 PTBR과 다음 PC의 supervisor 실행 매핑을 확인한 뒤 상태를 바꾼다. `MMUOFF`는 다음 PC가 같은 숫자의 물리 RAM 주소로 존재할 때만 상태를 끈다. 전환 명령 자체는 이전 MMU 상태에서 끝까지 실행되고 다음 opcode fetch부터 새 상태가 적용된다.

최대 8바이트의 일반 fetch·LOAD·STORE가 4KiB 경계를 넘으면 CPU는 양쪽 가상 페이지를 각각 변환하고 리틀 엔디언 결과를 조합한다. STORE는 두 페이지의 변환이 모두 성공한 뒤 쓰기를 시작한다. 64비트 atomic은 기존 규칙대로 8바이트 정렬이 필요하므로 페이지 경계를 넘지 않는다.

PTBR이 가리키는 페이지 테이블은 일반 물리 RAM을 사용하지만 CPU나 `main.c`가 특정 영역을 예약하지 않는다. 어떤 물리 페이지를 테이블로 사용할지, 일반 메모리와 겹치지 않게 관리할지는 게스트 부트로더나 운영체제의 책임이다. 현재 TLB는 없으므로 PTE 변경은 다음 접근부터 즉시 보이며 `TLBFLUSH`도 아직 필요하지 않다.

## 코어와 하드웨어 스레드

`vm_run()`은 가상 코어마다 자유 실행 host worker thread를 만든다. 코어 사이에는 전역 barrier가 없으며 실행 순서는 호스트 운영체제 스케줄링에 따라 달라질 수 있다. 같은 코어 안의 하드웨어 스레드는 해당 코어 worker가 명령어 하나씩 round-robin으로 실행한다.

```text
Core worker 0: Thread 0 → Thread 1 → Thread 0 → ...
Core worker 1: Thread 0 → Thread 1 → Thread 0 → ...
```

`-c`, `-t`는 사용할 수 있는 하드웨어 구성의 최대 용량만 정한다. VM 초기화 직후에는 Core 0의 Thread 0만 실행 가능하다. `main.c`는 바이너리를 적재한 주소를 이 부트 스레드의 PC에 설정할 뿐이며 SP나 스택 메모리를 정하지 않는다. 나머지 하드웨어 스레드는 `OFFLINE`이고 PC와 SP가 0이다. `COREID`, `THREADID`로 실행 주체를 구분할 수 있다.

추가 하드웨어 스레드는 host API 또는 Core Control MMIO를 사용하는 펌웨어·운영체제가 명시적으로 활성화한다.

```c
vm_activate_hardware_thread(&vm, 1, 0, entry);
```

활성화 API는 PC만 지정하고 RAM을 할당하거나 스택 범위를 만들지 않는다. 새 스레드의 `R15(SP)`는 0에서 시작하고 MMU도 꺼져 있다. 스레드에서 스택을 사용하려면 부트 코드나 운영체제가 `MOVI64`, `MOV`, 산술 명령 등으로 R15에 적절한 주소를 설정해야 한다. MMU를 켜면 해당 가상 스택 페이지에 필요한 R/W 및 권한 PTE도 준비해야 한다.

게스트 프로그램은 Core Control MMIO로 다른 하드웨어 스레드를 시작·정지·초기화할 수 있다. 실행 가능한 스레드가 없는 코어 worker는 대기한다. 모든 스레드가 WAITING이면 장치 worker와 VM은 계속 실행되며 IRQ를 기다린다. 마지막 활성 스레드가 `HALT`하거나 STOP되면 VM 실행이 종료된다.

실행 인자는 다음과 같다. `-d`는 외부 VIO 장치 모듈 하나를 연결하며 여러 번 지정할 수 있다. `-display`는 host display frontend를 선택하며 기본값은 `headless`, Windows 창 출력은 `window`다.

```text
-r RAM바이트 -l 프로그램.bin [-c 코어수] [-t 코어당_하드웨어스레드수] [-d 장치모듈]... [-display headless|window]
```

기본값은 1코어, 코어당 하드웨어 스레드 1개다. 최대값은 64코어, 코어당 8스레드다.

추가 코어와 스레드의 상태 구조체 및 host worker stack은 host 메모리를 조금 사용한다. 그러나 VM은 코어별로 게스트 RAM(`-r`)을 예약하거나 분할하지 않는다. 모든 코어가 같은 RAM을 공유한다.

장치 IRQ는 programmable IRQ Controller가 지정된 논리 프로세서의 로컬 `InterruptController`로 전달한다. 논리 프로세서 번호는 `core_id * threads_per_core + thread_id`다. reset route는 논리 프로세서 0이며 `LINE_SELECT/ROUTE`로 변경한다. 각 하드웨어 스레드는 IRQ를 활성화하기 전에 자기 VBR과 테이블을 설정해야 한다.

## 공유 RAM과 원자성

코어들은 하나의 RAM을 공유한다. GCC/Clang 빌드에서 자연 정렬된 1·2·4·8바이트 일반 접근은 host atomic을 사용한다. 비정렬 접근은 atomic byte 접근과 64바이트 단위 striped spin lock을 사용한다. 다른 컴파일러의 fallback은 striped lock으로 보호한다.

현재 메모리 순서는 단순성을 위해 sequential consistency를 사용한다. VM 프로그램이 여러 명령어로 공유 값을 수정할 때는 `CAS64`, `XCHG64`, `ATOMIC_ADD64`, `FENCE`를 사용해야 한다. 일반적인 `LOAD → ADD → STORE` 묶음은 원자적이지 않다.

## 버스와 MMIO

명령어 fetch는 MMU 상태에 따라 주소를 변환한 뒤 물리 RAM 또는 읽기 전용 Boot ROM만 사용한다. MMIO와 미할당 주소의 fetch는 `INSTRUCTION_ACCESS` 예외다. 스택과 벡터 테이블, 페이지 테이블은 항상 물리 RAM에 있어야 한다. `LOAD8/16/32/64`와 `STORE8/16/32/64`는 변환된 물리 주소로 `Bus`를 사용하므로 RAM과 MMIO에 접근할 수 있고 LOAD는 ROM도 읽을 수 있지만 ROM STORE는 `DATA_ACCESS` 예외다. MMIO나 ROM을 가상 주소에 노출하려면 leaf PTE의 물리 페이지 주소를 해당 영역에 지정해야 하며 최종 물리 영역의 접근 능력이 PTE 권한보다 우선한다.

장치는 RAM이나 다른 장치와 겹치지 않는 주소 범위에 등록해야 한다. 고정 영역은 VIO Hub `0xFFFFFFFFFFFC0000`~`0xFFFFFFFFFFFC0FFF`, System Information `0xFFFFFFFFFFFC1000`~`0xFFFFFFFFFFFC10FF`, System Control `0xFFFFFFFFFFFC2000`~`0xFFFFFFFFFFFC203F`, IRQ Controller `0xFFFFFFFFFFFC3000`~`0xFFFFFFFFFFFC30FF`, UART `0xFFFFFFFFFFFD0000`~`0xFFFFFFFFFFFD002F`, Core Control `0xFFFFFFFFFFFE0000`~`0xFFFFFFFFFFFE0037`, Timer `0xFFFFFFFFFFFF0000`~`0xFFFFFFFFFFFF001F`다. 시스템 장치 레지스터는 `SYSTEM.md`와 `IRQ_CONTROLLER.md`, 외부 장치 ABI와 슬롯 설정 공간은 `DEVICE_ABI.md`에 정의되어 있다.

## Core Control 장치

Core Control 레지스터는 모두 64비트이며 8바이트 정렬된 `LOAD64`, `STORE64`로만 접근할 수 있다. 기본 주소는 `0xFFFFFFFFFFFE0000`이다.

| 오프셋 | 이름 | 읽기 | 쓰기 |
|---:|---|---|---|
| `0x00` | `TARGET_CORE` | 대상 코어 | 대상 코어 선택 |
| `0x08` | `TARGET_THREAD` | 대상 스레드 | 대상 스레드 선택 |
| `0x10` | `ENTRY_PC` | 시작 PC | 시작 PC 설정 |
| `0x18` | `COMMAND` | 마지막 명령 | 명령 실행 |
| `0x20` | `STATUS` | 선택한 스레드 상태 | 불가 |
| `0x28` | `RESULT` | 마지막 명령 결과 | 불가 |
| `0x30` | `IPI_LINE` | 전송할 IRQ 번호 | 전송할 IRQ 번호 설정 |

`COMMAND` 값은 다음과 같다.

| 값 | 이름 | 동작 |
|---:|---|---|
| `0` | `NONE` | 결과를 `NONE`으로 초기화 |
| `1` | `START` | `OFFLINE` 스레드를 `ENTRY_PC`에서 시작 |
| `2` | `STOP` | `RUNNABLE` 또는 `WAITING` 스레드에 비동기 정지 요청 |
| `3` | `RESET` | `OFFLINE` 또는 `HALTED` 스레드의 CPU 상태를 초기화하고 `OFFLINE`으로 전환 |
| `4` | `IPI` | 선택한 RUNNABLE 또는 WAITING 스레드에 IPI_LINE IRQ 전송 |

`STATUS` 값은 `0=OFFLINE`, `1=STARTING`, `2=RUNNABLE`, `3=HALTED`, `4=WAITING`이다. 대상이 잘못되면 `UINT64_MAX`가 반환된다. STOP 성공은 요청이 접수됐다는 의미이므로 STATUS가 HALTED로 바뀌기까지 현재 명령어 하나가 마무리될 수 있다. HALTED 스레드를 다시 시작하려면 RESET 후 START해야 한다.

`RESULT` 값은 다음과 같다.

| 값 | 의미 |
|---:|---|
| `0` | 명령 없음 |
| `1` | 성공 |
| `2` | 잘못된 코어 또는 스레드 |
| `3` | ENTRY_PC가 RAM 밖임 |
| `4` | 현재 상태에서 실행할 수 없는 명령 |
| `5` | 존재하지 않는 명령 |
| `6` | IPI_LINE이 IPI 전용 범위 48~63을 벗어남 |

명령 자체가 거부돼도 MMIO 쓰기는 정상 완료된다. 게스트는 STORE64 이후 RESULT를 읽어 성공 여부를 확인해야 한다.

```text
STORE64 [TARGET_CORE],   core_id
STORE64 [TARGET_THREAD], thread_id
STORE64 [ENTRY_PC],      entry
STORE64 [COMMAND],       1       ; START
LOAD64  result,          [RESULT]
```

IPI는 48~63의 전용 벡터와 로컬 pending 비트를 사용하고 programmable IRQ Controller를 거치지 않은 채 선택한 코어·스레드로 직접 전달된다. 대상이 WAITING이고 인터럽트가 허용되어 있으면 해당 스레드가 깨어나 IPI 핸들러를 실행한다.

## 타이머 장치

타이머 레지스터는 모두 64비트이며 8바이트 정렬된 `LOAD64`, `STORE64`로만
접근할 수 있다. `COUNTER`와 `COMPARE`의 단위는 나노초이며 주파수는 1GHz다.
64비트 counter는 약 584년 뒤 자연스럽게 wrap한다.

| 오프셋 | 이름 | 읽기 | 쓰기 |
|---:|---|---|---|
| `0x00` | `CONTROL` | 현재 제어 비트 | 제어 비트 설정 |
| `0x08` | `COUNTER` | 현재 tick | 카운터 값 설정 |
| `0x10` | `COMPARE` | 비교값 | IRQ 발생 비교값 설정 |
| `0x18` | `STATUS` | pending 상태 | bit 0에 1을 쓰면 ACK 및 해제 |

`CONTROL` 비트는 다음과 같다.

| 비트 | 이름 | 의미 |
|---:|---|---|
| 0 | `ENABLE` | 카운터 동작 |
| 1 | `REPEAT` | compare 도달 후 반복 |
| 2 | `IRQ_ENABLE` | compare 도달 시 설정된 IRQ 발생 |

`COMPARE`가 0이면 카운터만 증가하고 IRQ는 발생하지 않는다. 반복하지 않는 타이머는 compare에 도달하면 자동으로 비활성화된다. 반복 타이머가 한 번의 큰 tick 증가로 여러 주기를 통과해도 동일 IRQ는 pending 비트 하나로 합쳐진다.

## UART 장치

UART는 플랫폼 독립적인 256바이트 RX/TX FIFO와 MMIO 인터페이스만 구현한다. 장치 코어는 호스트 콘솔이나 운영체제 API를 직접 호출하지 않는다. 기본 물리 주소는 `0xFFFFFFFFFFFD0000`, 기본 IRQ는 `1`이다.

| 오프셋 | 이름 | 접근 크기 | 읽기 | 쓰기 |
|---:|---|---:|---|---|
| `0x00` | `TXDATA` | 1바이트 | 불가 | 하위 8비트를 TX FIFO에 추가 |
| `0x08` | `RXDATA` | 1바이트 | RX FIFO에서 한 바이트 제거, 비어 있으면 0 | 불가 |
| `0x10` | `STATUS` | 8바이트 | FIFO 및 오류 상태 | 오류 비트에 1을 써서 해제 |
| `0x18` | `CONTROL` | 8바이트 | 현재 제어 비트 | 장치 및 IRQ 활성화 |
| `0x20` | `BAUD` | 8바이트 | baud 설정값 | baud 설정값 저장 |
| `0x28` | `IRQ_STATUS` | 8바이트 | pending 원인 | 해당 비트에 1을 써서 ACK |

`STATUS` 비트는 다음과 같다.

| 비트 | 이름 | 의미 |
|---:|---|---|
| 0 | `RX_READY` | RX FIFO에 읽을 데이터가 있음 |
| 1 | `TX_READY` | UART가 활성화됐고 TX FIFO에 빈 공간이 있음 |
| 2 | `RX_OVERRUN` | 가득 찬 RX FIFO에 데이터가 추가됨 |
| 3 | `TX_OVERRUN` | 가득 찬 TX FIFO에 데이터를 기록함 |
| 4 | `TX_EMPTY` | TX FIFO가 비어 있음 |

`CONTROL`의 bit 0은 `ENABLE`, bit 1은 `RX_IRQ_ENABLE`, bit 2는 `TX_IRQ_ENABLE`이다. `IRQ_STATUS`의 bit 0은 `RX_PENDING`, bit 1은 `TX_PENDING`이다. 읽지 않은 RX 데이터가 남아 있으면 ACK 후 다음 장치 tick에 RX IRQ가 다시 pending 된다. TX IRQ는 FIFO의 마지막 바이트가 출력 sink로 전달될 때 pending 된다.

TX는 `UartTxCallback`으로 추상화되어 있다. `main.c`는 이 콜백을 `stdout`에 연결하지만 디버거, GUI 콘솔, 로그 파일 등 다른 frontend로 교체할 수 있다. 콜백은 VM 장치 worker 또는 `uart_device_flush_tx()`를 호출한 host thread에서 실행될 수 있으므로 오래 block하거나 같은 UART MMIO로 재진입하면 안 된다.

```c
uart_device_set_tx_callback(&uart, tx_callback, context);
uart_device_receive_byte(&uart, byte);
```

RX 입력은 `uart_device_receive_byte()` 또는 `uart_device_receive()`로 host가 주입한다. 현재 `main.c`는 플랫폼 종속적인 non-blocking 키보드 입력을 연결하지 않으므로 RX frontend는 비어 있다. 향후 디버거나 콘솔 frontend가 이 API를 호출하면 UART 코어를 수정하지 않고 입력을 연결할 수 있다.

`BAUD`의 reset 값은 115200이다. 현재 VM은 실제 전송 지연을 모사하지 않고 장치 tick마다 출력 가능한 TX FIFO를 비운다. 따라서 `BAUD`는 guest-visible 설정값으로 보존되지만 아직 전송 속도에는 영향을 주지 않는다.

## CPU 초기화와 스택

단일 CPU를 직접 실행하려면 `cpu_init()`을 호출해야 한다.

```c
CPU cpu;
RAM ram = { memory, memory_size };

if (!cpu_init(&cpu, &ram)) {
    /* 초기화 실패 */
}
```

초기 `R15(SP)`와 KSP는 모두 0이다. CPU는 스택의 시작과 끝을 따로 저장하거나 관리하지 않는다. R15는 다른 범용 레지스터와 똑같이 `MOVI64`, `MOV`, `ADD`, `SUB` 등의 대상이나 입력으로 사용할 수 있다. 프로그램은 스택을 사용하기 전에 R15를 초기화해야 하며, user 모드에 들어갈 supervisor 코드는 `SETKSP`도 먼저 실행해야 한다.

```text
MOVI64 R15, stack_top
```

`CALL`, `RET`, `PUSH`, `POP`은 R15를 자동으로 변경하며 한 번에 8바이트를 사용한다. supervisor→supervisor 인터럽트·예외·syscall 프레임은 16바이트다. user→supervisor 프레임은 KSP에 24바이트를 사용하며 user SP도 저장한다. MMU가 켜져 있으면 user 스택은 user 권한으로, 전환 뒤의 KSP는 supervisor 권한으로 변환한다. 스택끼리 겹치거나 코드·데이터를 덮어쓰는 논리적 충돌은 CPU가 감지하지 않으며 페이지 배치와 스택 관리는 게스트 소프트웨어가 담당한다.

## 복구할 수 없는 실행 오류

일반적인 명령 오류는 위 동기 예외로 전달된다. 다음 상태에서는 예외 전달을 완료할 수 없으므로 `cpu_run()`이 `0`을 반환한다.

- 해당 원인의 예외 벡터가 설정되지 않음
- 예외 핸들러 주소가 RAM 범위를 벗어남
- supervisor SP 또는 user 진입용 KSP에 필요한 16/24바이트 프레임을 저장할 수 없음
- 예외 핸들러 실행 중 다시 동기 예외가 발생함
- 외부 인터럽트 핸들러 주소나 인터럽트 프레임 스택이 잘못됨

## 어셈블러

`tools/assembler/`에는 이 ISA의 모든 현재 opcode(`0x00`~`0x9C`)를 인코딩하는 C11 2-pass 어셈블러가 있다. opcode와 조건 코드, 레지스터 개수는 CPU와 어셈블러가 공통으로 포함하는 `include/isa.h`에 한 번만 정의되어 있다.

```powershell
.\build.ps1 -e counter
.\build\tools\vmasm.exe .\examples\counter\counter.asm `
    -o .\build\examples\counter.bin `
    --symbols .\build\examples\sym\counter.sym `
    --listing .\build\examples\lst\counter.lst
```

레이블 기반 절대/상대 분기, `R0`~`R15`와 `SP` 별칭, 숫자 표현식, 문자열 및 `.byte`/`.word`/`.dword`/`.qword`/`.ascii`/`.asciz`/`.space`/`.zero`/`.align`/`.org`/`.entry` 지시어를 지원한다. 재배치 모드에는 전역·weak·common·타입·크기 심볼 메타데이터도 있다. 전체 문법과 raw 바이너리의 진입점 제약은 `docs/tools/ASSEMBLER.md`를 참고한다.
