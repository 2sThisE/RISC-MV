# RISC-VM 정적 라이브러리 (legacy `CVMAR1`)

정적 라이브러리는 여러 `CVMOBJ2` 오브젝트를 하나의 `.a` 파일로 묶는다.
현재 포맷 magic은 `CVMAR1`, 버전은 1이다. 일반 Unix 도구와 같은 `.a`
확장자를 사용하지만 내부 포맷은 RISC-VM 프로젝트 전용이므로 호스트의 `ar`로 만들지 않고
`cvmar`를 사용한다.

```powershell
# 오브젝트 생성
.\build\tools\vmasm.exe math.s -c -o math.o
.\build\tools\vmasm.exe string.s -c -o string.o

# 라이브러리 생성: 두 명령은 같은 동작
.\build\tools\cvmar.exe create libbase.a math.o string.o
.\build\tools\cvmar.exe rcs libbase.a math.o string.o

# 멤버와 제공 심볼 확인
.\build\tools\cvmar.exe list libbase.a
.\build\tools\cvmar.exe t libbase.a

# 일반 오브젝트와 함께 링크
.\build\tools\cvmlink.exe kernel.o libbase.a -o KERNEL.EXF
```

archive는 각 멤버의 원본 바이너리 오브젝트와 다음 정보를 가진 심볼 인덱스를
저장한다.

- 멤버 이름, 데이터 위치와 크기
- 멤버가 제공하는 strong/weak `.global` 및 common 심볼
- archive에 들어간 오브젝트가 선언한 `.entry` 표시

`cvmlink`는 처음부터 모든 멤버를 합치지 않는다. 현재 링크에 남은 `.extern`
또는 `--entry`를 제공하는 멤버만 꺼내며, 그 멤버가 새 외부 심볼을 요구하면
필요한 멤버를 다시 찾는다. 따라서 사용하지 않은 함수가 든 멤버는 최종
`.exf`의 섹션과 심볼에 포함되지 않는다. 같은 멤버 안의 함수들은 하나의
선택 단위이므로 더 세밀한 제거가 필요하면 함수를 별도 `.o`로 어셈블해야 한다.

현재 `cvmar`는 새 archive 생성과 목록 조회를 지원한다. 기존 archive의 부분
교체·삭제, thin archive, archive 중첩, 동적 라이브러리는 지원하지 않는다.
멤버 파일명은 archive 안에서 고유해야 한다.
