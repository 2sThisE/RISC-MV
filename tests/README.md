# 통합 테스트 러너

모든 테스트 suite는 `build/test_runner.exe`에 들어간다. 각 suite는 별도 프로세스로 실행되므로 하나가 assertion으로 종료되어도 runner가 다음 suite를 계속 실행하고 마지막에 전체 결과를 반환한다.

## 빌드와 실행

```powershell
.\tests\build_runner.ps1
.\build\test_runner.exe
```

지원 옵션:

```powershell
.\build\test_runner.exe --list
.\build\test_runner.exe --run privilege
.\build\test_runner.exe --filter interrupt
.\build\test_runner.exe --repeat 100
.\build\test_runner.exe --filter exception --repeat 100
```

- `--list`: 등록된 suite 이름 출력
- `--run NAME`: 지정한 suite를 현재 프로세스에서 한 번 실행
- `--filter TEXT`: 이름에 TEXT가 포함된 suite만 실행
- `--repeat N`: 선택된 suite들을 N회 반복

성공하면 종료 코드 0, 테스트 실패가 있으면 1, 잘못된 옵션이면 2를 반환한다.

## suite 추가

1. `tests/example_test.c`를 만들고 `int test_example(void)` 함수를 구현한다.
2. 성공하면 0을 반환하고 검증에는 `assert()`를 사용한다.
3. `test_suites.h`의 `TEST_SUITE_LIST`에 `X("example", test_example)` 한 줄을 추가한다.
4. `build_runner.ps1`을 다시 실행한다. `*_test.c` 파일은 자동으로 빌드 대상에 포함된다.

suite 내부 helper는 다른 파일과 이름이 겹치지 않도록 `static`으로 선언한다. 생성한 host thread와 VM 자원은 성공 경로에서 suite가 직접 정리해야 한다.
