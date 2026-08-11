# 직접 재귀와, 건너뛰는 경로 위의 효과

2026-08-11 측정입니다. 두 가지를 같이 담습니다. 하나는 커버리지를 올린 변경이고, 하나는 그 변경이 드러낸 기존 결함이라 커버리지를 내렸습니다. **두 수치는 같은 코드 상태에서 잰 것이므로 따로 인용하지 않습니다.**

## 재현

```sh
./out/build/windows-clang/quodlibet.exe coverage \
    out/corpus/train/units.txt out/corpus/train/detail-g9-final.tsv
awk -F'\t' '$3!="lowered"{print $4"\t"$5}' \
    out/corpus/train/detail-g9-final.tsv | sort | uniq -c | sort -rn
```

## 수치

| 상태 | train `SUPPORTED` | 비율 |
|---|---:|---:|
| 직전 (`b115310`) | 29,150 | 97.56% |
| 재귀 호출을 연 뒤 | 29,340 | 98.19% |
| 건너뛰는 경로의 효과를 거부한 뒤 | **26,219** | **87.75%** |

`definitions` 는 29,880 으로 셋 다 같습니다. verifier 통과는 26,219 / 26,219 이고 status 실패는 0 입니다. val 은 1,050 중 884 입니다.

## 1. 직접 재귀 (+190)

`unsupported_call` 의 "the callee has no declaration in this unit" 202 건 중 **190 건이 자기 자신을 부르는 함수**였습니다. 나머지 12 건은 추출이 이름을 바꾸지 않고 남긴 실제 미선언 식별자입니다(예: `_bfd_error_handler`).

원인은 `collect_callees` 가 `declaration` 노드만 훑었다는 것입니다. 정의도 선언이고 그 이름은 자기 본문 안에서 유효 범위에 있습니다. `function_definition` 을 같이 훑게 하면 190 건이 전부 열립니다.

**정의로 들어온 항목은 호출 대상일 뿐입니다.** 선언된 코퍼스 함수를 값으로 쓰면 양쪽이 공유하는 불투명 토큰이 되는데(`ARCHITECTURE.md` 의 "Declared corpus function designators are opaque tokens"), 비교 중인 함수에 그 규칙을 그대로 주면 토큰 하나가 좌우의 서로 다른 두 정의를 같은 함수라고 말하게 됩니다. `is_definition` 이 그 자리를 막습니다.

같은 이유로 **miter 는 이 IR 을 거부합니다.** `src/product.c` 의 `check_ir_fragment` 이 호출 심볼과 비교 대상 함수 이름을 대조합니다. 좌우가 같은 심볼을 부르지만 각자 자기 정의를 가리키므로, 이 인코딩이 짝지은 호출 자리에 세우는 합동이 곧 증명하려는 동치가 됩니다. 재귀를 증명 가능하게 만들려면 assume-guarantee 규칙이 필요하고, 그것은 가정과 종료 side condition 을 증거에 적는 별개의 작업입니다. 지금은 로어링이 열리고 판정은 `UNKNOWN` 입니다.

## 2. 건너뛰는 경로 위의 효과 (-3,121)

`?:`, `&&`, `||` 는 **양쪽 피연산자를 먼저 로어링하고 나서 `SELECT` 로 고릅니다.** 정의됨(definedness)은 따로 short-circuit 하지만 **효과는 그렇지 않습니다.** 그래서 C 가 평가하지 않는 피연산자의 호출, 대입, 증감이 실행됩니다.

이것이 실제로 틀린 IR 이라는 것은 재귀가 드러냈습니다. `return n <= 0 ? 0 : n + REC_sum(n - 1)` 에서 거짓 팔을 평가하면 기저 경우에 값이 틀리는 것이 아니라 재귀가 끝나지 않습니다. 인터프리터에 컴파일된 같은 함수를 callee 명세로 주고 `n = -3` 으로 돌리면 C 가 부르지 않는 호출이 한 번 일어납니다.

코퍼스에서 **3,121 개 본문이 이 상태로 `SUPPORTED` 였습니다.**

| 메시지 | 건수 |
|---|---:|
| the right operand of a short-circuit operator has an effect this slice would run on the path C skips | 2,876 |
| an arm of a conditional expression has an effect this slice would run on the path C skips | 296 |

(합이 3,172 인 것은 이 거부가 먼저 걸리면서 다른 진단에 세어지던 본문이 옮겨왔기 때문입니다. 순증감은 -3,121 입니다.)

기존 시험 하나도 이 결함을 타고 있었습니다. `CLowerPointers.CarriesAChangingPointerThroughALoopAuthorityRegion` 의 `while (node != 0 && limit-- > 0)` 는 `node` 가 널일 때도 `limit` 을 내렸습니다. **그 시험이 통과하고 있었던 것은 `limit` 을 아무도 관찰하지 않았기 때문입니다.** 감소를 본문 문장으로 옮겨 의도한 것을 그대로 재게 했습니다.

거부는 보수적입니다. Tree-sitter 가 캐스트를 `call_expression` 으로 풀어놓는 경우까지 효과로 셉니다. 그쪽을 정밀하게 가르려면 타입이 필요하고, 지금은 덜 받는 쪽으로 틀립니다.

## 이것이 G9 에 대해 말하는 것

**99% 는 지금 구조로 도달할 수 없습니다.** 남은 3,661 건 중 3,181 건이 이 하나의 결함이고, 그것을 닫는 방법은 피연산자를 실제 제어 흐름으로 내리는 것뿐입니다. IR 은 이미 다중 블록과 PHI 와 순환 SSA 를 갖고 있으므로(루프 로어링이 그 위에 서 있습니다) 기계는 있습니다. 없는 것은 표현식 로어링이 블록을 중간에서 쪼개고 PHI 로 합치는 경로입니다.

그 다음 순서는 이렇습니다.

1. **short-circuit 과 조건 표현식을 분기로 내린다.** 상한 3,181. 이것이 열리면 재귀 본문 다수도 같이 열립니다(재귀는 대부분 `?:` 안에 있습니다).
2. `unsupported_control_flow` 105, `unsupported_type` 78, `volatile` 50.
3. 원리상 거부로 남는 것: 미선언 callee 12, `uninitialized_read` 26, 불완전 레코드, 잘못된 C.
