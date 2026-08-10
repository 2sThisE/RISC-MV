# 외부 장치 프로젝트

각 장치는 `devices/<프로젝트 이름>`에 독립적으로 배치되고 루트
`build.ps1`에서 빌드한다.

```powershell
.\build.ps1 -d sample_counter
.\build.ps1 -d display -nc
.\build.ps1 -d keyboard -e keyboard -nc
```

산출물은 `build/devices`에 평탄하게 생성되고 DLL은 `build/modules`에도
자동 복사된다. 따라서 별도 `-d` 없이 VM을 시작해도 연결된다.

```powershell
.\build\main.exe -r 1024 `
    -l .\build\examples\calculation.bin
```

현재 프로젝트 이름은 `block`, `display`, `keyboard`, `sample_counter`다.
