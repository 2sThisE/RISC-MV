# 예제 프로젝트

예제 소스는 `examples/<프로젝트 이름>`으로 나뉘지만 최종 산출물은 모두
`build/examples`에 평탄하게 생성된다. `.lst`는 `build/examples/lst`,
`.sym`과 linker map은 `build/examples/sym`에 분리된다.

```powershell
.\build.ps1 -e counter
.\build\main.exe -r 4096 -l .\build\examples\counter.bin
```

예제 이름은 `benchmarks`, `block`, `boot`, `calculation`, `counter`,
`display`, `hello`, `keyboard`, `linker`, `syscall`이다. 부팅 예제 실행은
다음 스크립트를 사용한다.

```powershell
.\examples\boot\run_boot_demo.ps1
```
