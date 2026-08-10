# 커버리지 재측정 2026-08-12 (두 번째)

같은 날 두 번째 단위의 재측정입니다. `docs/coverage/coverage-20260812.md` 이후이고 같은 도구, 같은 코퍼스, 같은 명령입니다.

## train 스플릿 (서로 다른 C 본문 29,880)

| | 기준선 2026-08-10 | 2026-08-11 | 2026-08-12 | 이번 |
|---|---:|---:|---:|---:|
| **IR 로어링 SUPPORTED** | **53 (0.18%)** | **6,129 (20.51%)** | **7,233 (24.21%)** | **8,541 (28.58%)** |
| status 실패 | 0 | 0 | 0 | **0** |

## 이번 단위가 더한 몫

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 | 7,233 | **8,541** |
| val 로어링 (1,050 중) | 288 | **349** |
| val 판정률 | 279/288 (96.9%) | **337/349 (96.6%)** |

식 어휘의 빈 자리를 닫았습니다. `unsupported_expression` 이 4,622 에서 **2,154** 로 줄었습니다.

- **조건식 `c ? a : b`** (측정 시점 667회 등장). 값은 `SELECT` 로 계산하되 **정의성은 단락**시킵니다. 조건이 고르지 않은 쪽의 정의성을 요구하면 C 에 없는 undefined behaviour 를 만들어 냅니다. `&&` 와 `||` 가 이미 쓰던 규칙 그대로입니다
- **복합 대입** `+= -= *= /= %= &= |= ^= <<= >>=` (첫 차단 1,021). C 는 `x op= v` 를 `x` 를 한 번만 평가하는 `x = x op v` 로 정의하므로, 대상을 먼저 한 번 확정하고 일반 이항 연산 경로를 그대로 씁니다. shift 의 정의성 규칙도 따라서 그대로 걸립니다
- **값으로 쓰는 대입** (433회), **`++` 와 `--`** 의 전위와 후위 (403회), **쉼표 연산자** (53회)

넷은 전부 같은 대상 모델을 쓰므로 하나의 read-modify-write 로 구현했습니다. 셋을 따로 쓰면 어긋날 수 있는 규칙(대상 한 번 평가, 선언 타입으로의 되변환, const 거부, 초기화 전 읽기 거부)이 한 곳에 있습니다.

## 정확성을 대가로 지불하지 않았습니다

| | 이전 | 이후 |
|---|---:|---:|
| **verifier**: 로어링 시험이 만드는 모든 IR | 통과 | **통과** |
| **differential**: 직렬 `ctest` | 507 / 507 | **511 / 511** |
| status 실패 | 0 | **0** |

`tests/test_c_lower_expressions.cpp` 가 13개 함수를 **이 시험을 컴파일한 그 컴파일러가 실제로 돌린 결과**와 대조합니다. 한 텍스트에서 정의와 소스 문자열을 같이 만들므로 둘이 갈라질 수 없습니다.

양쪽을 다 봅니다.

- 정의된 입력: 값이 참조와 정확히 같아야 합니다 (사례당 무작위 512회)
- 정의되지 않은 입력: 값이 아니라 `UNDEFINED_BEHAVIOR` 여야 하고, **참조는 그 입력으로 부르지 않습니다.** UB 를 실제로 실행하면 비교가 무의미해집니다
- UB 쪽에 실제로 닿았는지를 세고, 닿은 사례가 3개 미만이면 시험이 실패합니다. 그렇게 하지 않으면 guard 를 하나도 검사하지 않고 통과하는 시험이 됩니다

`AConditionalDoesNotDemandTheArmItDidNotTake` 가 이 단위의 핵심을 못 박습니다. `b != 0 ? a / b : a` 는 `b == 0` 에서 값이지 UB 가 아닙니다.

포인터 differential 에도 두 사례를 더했습니다. `p[i] += 7` 과 `p[i]++` 는 같은 원소를 읽고 쓰므로, **최종 메모리 이미지**가 주소를 한 번만 계산해 둘 다에 썼는지를 말해 줍니다.

한계는 여전히 UNKNOWN 입니다. 초기화 전 대상을 읽는 복합 대입, const 지역, 이 slice 가 값으로 나를 수 없는 arm 을 가진 조건식은 거부합니다.

### 도중에 잡은 status 실패

복합 대입 작업 중 train 에서 status 실패 1건이 났습니다. `void **` 를 역참조한 자리에 쓰면 대상의 선언 타입이 `void` 인데, 그 타입으로 값을 변환하려 하면 IR 이 값을 가질 수 없는 타입을 요구받습니다. 계약대로 `type_error` UNKNOWN 으로 고쳤습니다. 의미 한계는 언제나 status 실패가 아닙니다.

## 남은 첫 차단 (train)

| 사유 | 건수 |
|---|---:|
| (로어링 성공) | 8,541 |
| `unsupported_pointer` | 5,614 |
| `unsupported_type` | 3,684 |
| `unsupported_loop` | 3,020 |
| `uninitialized_read` | 2,350 |
| `unsupported_expression` | 2,154 |
| `unsupported_control_flow` | 1,638 |
| `integer_literal_out_of_range` | 906 |
| `unsupported_call` | 868 |
| `invalid_declaration` | 491 |
| `type_error` | 395 |

다음 순위는 이렇습니다.

1. **메모리에서 읽은 포인터의 provenance** (pointer 5,614 의 대부분). 단위 7에서 정직하게 거부하기로 한 규칙이고, 이것을 열려면 object 표가 파라미터에서만 유도되지 않아야 합니다. W2 의 miter 와 같이 움직여야 하는 설계 변경입니다
2. **`sizeof`** (측정 1,371회). 피연산자를 평가하지 않고 타입만 알아내야 하므로 emit 하지 않는 정적 타입 질의가 필요합니다. `sizeof(TYP_0)` 같은 타입 이름이 4,986, `*VAR_0` 과 `ARG_0->FLD_n` 이 그 다음입니다
3. **loop** 3,020. 닫으면 W6 의 CHC/PDR proof method 선행이 풀립니다
4. `uninitialized_read` 2,350. 읽기 전에 반드시 쓰이는지를 보는 분석이 필요합니다

## 재현

```sh
./out/build/windows-clang/quodlibet.exe coverage \
    out/corpus/train/units.txt out/corpus/train/detail.tsv
cut -f4 out/corpus/train/detail.tsv | sort | uniq -c | sort -rn
```

판정률은 `tools/corpus/verdict_rate.py` 로 val 에서 잽니다. CMake 가 고른 Python 으로 돌려야 합니다.
