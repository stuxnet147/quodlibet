# 건너뛰는 경로를 분기로 내린다

2026-08-11, `docs/coverage/coverage-20260811-recursion.md` 의 후속입니다. 그 문서가 남긴 지배적 잔여 하나를 닫습니다.

## 재현

```sh
bash scripts/coverage-parallel.sh out/corpus/train/units.txt \
    out/corpus/train/detail-g9.tsv 16
awk -F'\t' '$3!="lowered"{print $4"\t"$5}' \
    out/corpus/train/detail-g9.tsv | sort | uniq -c | sort -rn
```

`coverage` 는 단일 스레드이고 train 한 번이 코어 하나로 95초입니다. 단위끼리 독립이므로 목록을 쪼개면 됩니다. 16 갈래로 33초입니다. 합계는 직렬과 바이트까지 같습니다.

## 수치

| 상태 | train `SUPPORTED` | 비율 | verifier |
|---|---:|---:|---|
| 세션 시작 (`b115310`) | 29,150 | 97.56% | 통과. 단 3,121 건의 IR 이 C 와 다름 |
| 재귀 + 효과 거부 (`b1e5a3c`) | 26,219 | 87.75% | 26,219 / 26,219 |
| 분기 로어링 | **29,340** | **98.19%** | **29,340 / 29,340** |

val 은 1,050 중 1,018 (96.95%) 입니다. status 실패는 셋 다 0 입니다.

**시작 지점과 끝 지점의 비율이 비슷한 것은 우연입니다.** 시작의 29,150 에는 틀린 IR 3,121 건이 들어 있었고 재귀 190 건이 빠져 있었습니다. 끝의 29,340 은 그 3,121 이 고쳐진 것이고 190 이 들어온 것입니다.

## 무엇을 했나

`?:`, `&&`, `||` 는 양쪽 피연산자를 로어링하고 `SELECT` 로 골랐습니다. 이제 피연산자에 효과가 있으면 그 피연산자가 자기 블록을 받고 결과가 join 의 PHI 가 됩니다. 순수한 피연산자는 그대로 `SELECT` 입니다. 블록이 적고 같은 것을 말합니다.

세 가지가 따라 나왔습니다.

**변환은 그것을 실행할 팔에 속합니다.** `usual_arithmetic_conversions` 는 promotion 과 conversion 을 현재 블록에 냅니다. 두 팔이 다른 블록에 있으면 각자의 블록에 나야 합니다. 그래서 그 함수를 풀어 쓰고 각 호출 앞에서 블록을 정합니다. 팔이 블록을 공유하는 경우에는 같은 코드가 예전과 같은 것을 냅니다.

**부분 연산은 팔 안에서 해소합니다.** verifier 가 PHI 의 각 incoming 에 대해 그 선행 블록에서 이미 UB 가 해소되었을 것을 요구합니다(`src/ir_verify.c:1817`, "no single guard in the join block dominates the branches"). 이건 C 의미론과 같은 말입니다. 실행된 팔은 이미 그 식을 평가했으므로 UB 가 일어난 시점이 join 이 아니라 그 팔입니다. `SELECT` 경로는 두 팔을 어차피 계산하므로 관찰 시점까지 미룰 수 있었고, 분기는 미룰 수 없습니다. 이걸 안 하면 verifier 가 31 건을 거부합니다.

**블록을 붙잡고 있던 자리를 전부 다시 읽습니다.** `if`, `switch`, `for`/`while` 의 preheader, `for` 의 update 는 문장이 시작한 블록을 지역 변수에 담아 두고 나중에 그 블록에 종료자를 답니다. 조건이 블록을 쪼개면 그 변수가 낡습니다. 전부 표현식을 내린 뒤의 `context->current_block` 으로 바꿨습니다.

루프가 가장 까다로운 자리였습니다. 헤더는 루프 PHI 를 담고 backedge 를 받으며 둘 다 그대로 있습니다. 움직이는 것은 조건 자신의 두 간선이 나가는 블록뿐입니다. 조건이 만든 블록들은 매 반복 실행되고 헤더가 그것들을 지배하므로 루프의 모양은 바뀌지 않습니다. `while (node != 0 && limit-- > 0)` 를 쓰는 기존 시험(`CLowerPointers.CarriesAChangingPointerThroughALoopAuthorityRegion`)이 원문 그대로 컴파일된 C 와 대조해 통과합니다. 앞선 커밋에서 이 시험의 C 를 고쳐야 했던 것이 되돌려졌습니다.

## 남은 540

| 진단 | 건수 | 성격 |
|---|---:|---|
| `undeclared_identifier` | 58 | 재분류 필요 |
| `duplicate_declaration` | 58 | 재분류 필요 |
| `unsupported_type` 다차원/접을 수 없는 배열 | 47 | 구현 가능 |
| `unsupported_control_flow` goto 가 초기화나 중첩 자동 상태를 우회 | 42 | 구현 가능 |
| `unsupported_control_flow` 루프 안 VLA 의 반복별 수명 | 42 | 구현 가능 |
| `unsupported_volatile_or_atomic` | 50 | 구현 가능. 상한 약 42 |
| `uninitialized_read` 주소가 callee 로 탈출 | 32 | 원리상 거부 |
| `unsupported_call` 선언 없는 callee | 12 | 원리상 거부 (코퍼스 산물) |
| 불완전 레코드, 잘못된 C, 함수 포인터 변환 | 약 60 | 원리상 거부 |
| 나머지 꼬리 | 약 139 | 섞임 |

99% 는 29,582 이므로 242 건이 더 필요합니다. 원리상 거부가 약 100 건이므로 나머지 440 중 242 를 닫으면 됩니다. **이번 커밋 전과 달리 이제 그것이 산술적으로 가능합니다.**

## 판정 쪽은 그대로입니다

분기가 늘어난 것은 로어링이고 miter 는 여전히 loop-free 이며 재귀를 거부합니다. 커버리지와 판정 가능 범위의 간격은 이 커밋으로 좁아지지 않았습니다. 좁히는 것은 루프 불변식 또는 CHC/PDR 과 재귀의 assume-guarantee 규칙이고 둘 다 `todo.md` 에 열려 있습니다.
