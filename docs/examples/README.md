# 예제 프로젝트

예제 소스는 `examples/<프로젝트 이름>`으로 나뉘지만 최종 산출물은 모두
`build/examples`에 평탄하게 생성된다. `.lst`는 `build/examples/lst`,
`.sym`과 linker map은 `build/examples/sym`에 분리된다.

```powershell
.\build.ps1 -e counter
.\build\main.exe -r 4096 -l .\build\examples\counter.bin
```

예제 이름은 `benchmarks`, `block`, `boot`, `calculation`, `counter`,
`display`, `hello`, `keyboard`, `linker`, `llvm_ir`, `syscall`이다. 부팅 예제 실행은
다음 스크립트를 사용한다.

```powershell
.\examples\boot\run_boot_demo.ps1
```

`llvm_ir` 예제는 LLVM text IR과 Clang C 입력을 `cvmir`/`cvmclang`으로
변환하고 startup 오브젝트와 링크한다. 산출물에는
`llvm_arithmetic.exf`, `clang_arithmetic.exf`, `kernel_memory.exf`와 기본
`crt0.o`/`libcvm.a`를 사용하는 `freestanding_main.exf`가 있다.

`examples/compiler`의 자체 `cvmcc` 예제는 지원·유지보수 종료 당시 상태를
보존한 회귀 참고 자료다. 기본 또는 선택 빌드 대상이 아니며 다시 지원할
계획도 없다.
