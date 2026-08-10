# VM 커널 이미지 도구 (`vmkimg`)

`vmkimg`는 헤더 없는 raw VM 기계어를 검증 가능한 `kernel.cvm` 이미지로
포장한다. 도구는 호스트에서 실행되며 VM 안의 C 컴파일러를 요구하지 않는다.

## 빌드

```powershell
.\tools\kernel_image\build.ps1
```

## 이미지 생성

```powershell
.\build\vmkimg.exe pack .\build\kernel.bin `
    -o .\build\kernel.cvm `
    --load 0x10000 `
    --entry 0x10000 `
    --memory-size 0x20000
```

- `--load`는 raw 파일을 적재할 물리 주소이며 필수다.
- `--entry` 기본값은 `--load`와 같다.
- `--virtual` 기본값은 `--load`와 같다.
- `--memory-size`는 BSS까지 포함한다. 생략하면 raw 파일 크기와 같다.
- `--alignment` 기본값은 VM 페이지 크기인 4096이다.
- `--flags` 기본값은 `rx`이며 `r`, `w`, `x` 조합을 받는다.
- `--features`는 커널이 요구하는 System Information feature mask다.
- `--build-id`는 정확히 32자리인 16진수 식별자다.

현재 패키저는 raw 파일 하나를 단일 LOAD 세그먼트로 만든다. 이미지 규격
자체는 최대 64개 세그먼트를 지원하므로 링커를 추가할 때 `.text`,
`.rodata`, `.data`, `.bss`를 별도 권한으로 출력할 수 있다.

## 검사

```powershell
.\build\vmkimg.exe inspect .\build\kernel.cvm
```

`inspect`는 헤더와 payload CRC32, ISA 버전, 모든 세그먼트 범위와 정렬,
RAM 적재 영역 중첩, 실행 진입점을 검사한 뒤 메타데이터를 출력한다.
