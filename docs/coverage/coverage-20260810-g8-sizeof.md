# G8 `sizeof(type)` 확장과 전체 IR 검증

2026-08-10에 G8의 확장 절차 예시로 `sizeof(type)`의 첫 슬라이스를
추가하고, 같은 코퍼스를 다시 측정했습니다. 측정 도구는 이제
`SUPPORTED`인 모든 로어링 산출물을 열어 독립 IR verifier에 통과시킵니다.
하나라도 실패하면 `coverage` 명령도 실패합니다.

## 지원한 범위

- 스칼라 타입과 포인터 타입
- 정의가 보이는 `struct`와 `union`
- 유닛에서 선언한 typedef와 asm2c 코퍼스의 고정 typedef
- 피연산자를 실행하지 않는 상수 결과

배열 또는 함수 type descriptor, `sizeof(expression)`, 불완전 타입,
현재 모델 밖의 포인터 깊이는 계속 `UNKNOWN`입니다. 특히
`sizeof(1 / a)`는 나눗셈을 실행하거나 UB를 만들지 않고
`unsupported_expression`으로 남습니다.

## 코퍼스 결과

| split | 함수 | 이전 `SUPPORTED` | 현재 `SUPPORTED` | verifier 통과 | verifier 실패 | status 실패 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| val | 1,050 | 349 (33.24%) | 365 (34.76%) | 365 | 0 | 0 |
| train | 29,880 | 8,541 (28.58%) | 8,755 (29.30%) | 8,755 | 0 | 0 |

train의 `unsupported_expression` 첫 차단은 2,154건에서 1,514건으로
줄었습니다. 성공 증가는 214개입니다. `sizeof`가 있어도 다른 미지원
표현식이 먼저 막는 함수가 있으므로 등장 횟수와 성공 증가량은 같지
않습니다.

## 정확성 근거

`tests/test_c_lower_expressions.cpp`는 typedef, 포인터, padding이 있는
구조체의 크기를 합한 동일 C 함수를 테스트 바이너리에 실제로 컴파일하고,
인터프리터 결과를 무작위 입력으로 대조합니다. 같은 테스트가 생성된 IR을
verifier에도 통과시킵니다. 미지원 배열과 expression form, 비객체 typedef는
정확한 진단 코드의 `UNKNOWN`인지 별도로 검사합니다.

최종 검증 결과는 다음과 같습니다.

- Windows Clang: 511/511 CTest 통과
- Linux Clang ASan+UBSan: 510/510 CTest 통과
- val: 로어링 IR 365/365 verifier 통과
- train: 로어링 IR 8,755/8,755 verifier 통과

Linux에서는 pytest가 설치되지 않아 Python binding CTest 하나가 등록되지
않으며, 그래서 Windows보다 총 테스트 수가 하나 적습니다.

## 재현

```sh
cmake --build --preset windows-clang --parallel
ctest --preset windows-clang --output-on-failure
./out/build/windows-clang/quodlibet.exe coverage out/corpus/val/units.txt
./out/build/windows-clang/quodlibet.exe coverage out/corpus/train/units.txt

MSYS_NO_PATHCONV=1 wsl --cd /mnt/d/projects/machine-model/python/quodlibet -- \
  cmake --build --preset linux-fuzz --parallel
MSYS_NO_PATHCONV=1 wsl --cd /mnt/d/projects/machine-model/python/quodlibet -- \
  ctest --test-dir out/build/linux-fuzz --output-on-failure --parallel 12
```

Git Bash에서 WSL 명령을 직접 실행할 때는 MSYS 경로 자동 변환을 끄기 위해
`MSYS_NO_PATHCONV=1`을 앞에 둡니다.
