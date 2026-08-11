# RISC-VM 링커 (`cvmlink`)

레거시 이름의 `cvmlink`는 재배치 가능한 오브젝트를 RISC-VM EXF 이미지로
묶는다.

```powershell
.\build\tools\vmasm.exe kernel.s -c -o kernel.o
.\build\tools\cvmlink.exe kernel.o support.o `
    -o KERNEL.EXF --base 0x40000000 --physical-relocatable `
    --map kernel.map
```

정적 라이브러리 `.a`도 일반 `.o`와 함께 입력할 수 있다. 링커는 현재
미해결 심볼을 제공하는 archive 멤버만 선택적으로 추출한다.

```powershell
.\build\tools\cvmar.exe create libsupport.a support.o
.\build\tools\cvmlink.exe kernel.o libsupport.a -o KERNEL.EXF
```

파일 이름은 일반 도구 체인 관례를 따른다. `.s`는 어셈블리 소스, `.o`는
재배치 오브젝트이며 `.a`는 정적 라이브러리다. `.exf`는
RISC-VM 전용 실행/부팅 이미지다. 기존 고정 주소 경로는 `.asm`에서 `.bin`을
계속 지원한다.

링커는 `.text` (`r-x`), `.rodata` (`r--`), `.data` (`rw-`), `.bss`
(`rw-`, zero-fill)를 각각 페이지 정렬된 LOAD 세그먼트로 만든다. 오브젝트의
`ABS32`, `ABS64`, `REL32` 재배치를 최종 주소에 적용하며 범위를 벗어난 값은
링크 오류다. strong 심볼은 common과 weak보다 우선하고 common 객체는 최대
크기·정렬로 병합된다. 미정의 weak는 주소 0이며 중복 strong은 오류다.
`--physical-relocatable`을 사용하면 `--base`는 고정 가상 base가 되고 각
세그먼트의 load 필드는 물리 offset으로 기록된다. 실제 물리 base와 초기
페이지 테이블은 부트로더가 메모리 맵을 보고 결정한다.
바이너리 오브젝트 구조는 [OBJECT_FORMAT.md](OBJECT_FORMAT.md),
함수 호출 규약은 [ABI.md](../system/ABI.md)를 참고한다.
정적 라이브러리 생성법과 선택 규칙은
[STATIC_LIBRARY.md](STATIC_LIBRARY.md)를 참고한다.
