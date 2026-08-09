# W6 진행 기록

브랜치: `stuxnet147/w6-evidence-pipeline` (워크트리 기본 브랜치. 조율자 확인 완료)
지시서: `docs/workstreams/W6.md`

## 지금 하는 것

WU3. `src/combine.c` 로 여러 method 의 결과를 증거 우선 규칙으로 결합하는 중입니다.

## 작업 단위

| 단위 | 내용 | 상태 | 커밋 |
|---|---|---|---|
| WU1 | rewrite rule catalogue (W6.md 2번) | 완료 | (아래) |
| WU2 | 독립 merge replay checker `src/egraph_check.c` (1번) | 완료 | (아래) |
| WU3 | 증거 우선 결합 `src/combine.c` (3번) | 진행 중 | |
| WU4 | persistent artifact/evidence cache `src/cache.c` (4번) | 대기 | |
| WU5 | cache key 완전성 통합 검증 (5번) | 대기 | |

## 설계 결정

### 순서를 2번(rule catalogue) 부터 시작

W6.md 는 1번(replay checker) 을 먼저 적었지만, checker 가 검사할 대상이 되는 rule 전제가 2번에서 생깁니다. rule catalogue 가 없으면 checker 는 `reason` 문자열을 그대로 믿는 것 말고 할 일이 없습니다. 그래서 catalogue 를 먼저 만들고 checker 가 그것을 소비합니다.

### `ql_egraph_merge_record_v1` 을 넓히지 않음

이 구조체는 다른 워크스트림이 지금 쓰고 있고 ABI 가 고정입니다. rule 전제는 record 안이 아니라 **rule 이름으로 조회하는 versioned catalogue** 에 둡니다. record 에는 이미 `reason` 이 있으므로 catalogue lookup 키로 충분하고, evidence 재생 시점에 catalogue version 과 catalogue digest 를 같이 고정하면 "어떤 rule set 아래에서 정당한가" 가 결정됩니다. catalogue digest 는 `METHODS.md` 가 요구하는 rewrite-set digest 역할도 합니다.

### catalogue 가 담는 전제

W6.md 가 지목한 세 축(비트 폭, 부호 해석, 오버플로 조건)을 조건 비트로 명시합니다.

- `QL_EGRAPH_RULE_COND_WIDTH_AGNOSTIC`: sort 가 허용하는 모든 bit width 에서 성립
- `QL_EGRAPH_RULE_COND_SIGN_AGNOSTIC`: 부호 해석과 무관하게 성립
- `QL_EGRAPH_RULE_COND_TOTAL_ARITHMETIC`: 2^w modulo 전체 산술에서만 성립. 오버플로가 UB 인 언어 수준 해석에서는 frontend 가 별도로 discharge 해야 함

e-graph 는 순수 term engine 이고 UB/poison/effect 를 모르므로(`egraph.h` 머리말), `TOTAL_ARITHMETIC` 이 붙은 rule 은 그 전제를 evidence 에 남기는 것이 목적입니다.

### WU1 이 실제로 넣은 것

- `ql_egraph_rule_descriptor_v1` 과 catalogue 조회 API 4개. 36개 rule 전부 등재
- `QL_EGRAPH_RULE_CATALOGUE_DIGEST_HEX` 로 v1 digest 를 헤더에 못박음. 전제를 조용히 고치면 시험이 깨짐
- `ql_egraph_term_initial_class`. replay checker 가 derivation 의 시작 상태를 알아야 하는데 `ql_egraph_term_view_v1.current_class` 는 조회 시점 root 라 쓸 수 없음
- `tests/test_egraph_rules.cpp` 13개. 그중 `DescribesEveryRewriteTheEngineRecords` 는 saturation 이 실제로 남긴 모든 REWRITE reason 이 catalogue 에 있는지 확인하므로, 새 rule 을 등재 없이 추가하면 깨짐

검증: `ctest --preset windows-clang` 330/330 통과.

### WU2 가 실제로 넣은 것

`src/egraph_check.c` 는 e-graph 엔진 코드를 부르지 않습니다. term table 과 merge log 를 데이터로 받아 자체 union-find 를 세우고 record 를 순서대로 재생하면서, 각 merge 가 앞선 record 들이 만든 상태에서 그 rule 로 정당화되는지를 직접 계산합니다. `reason` 문자열을 믿지 않습니다.

설계 결정:

- **class representative 를 "그 class 에서 가장 작은 식별자" 로 정의**했습니다. record 의 class snapshot 을 검사하려면 checker 가 엔진과 같은 이름을 얻어야 하는데, 엔진의 tie-breaking 을 베끼는 대신 canonical form 을 유도해서 씁니다. 어긋나면 `STALE_CLASS` 로 거부합니다.
- **rejected 여도 merge 를 적용하고 계속 갑니다.** 첫 결함에서 멈추면 뒤의 결함이 전부 그 뒤에 숨습니다. `ReportsEveryRejectionNotOnlyTheFirst` 가 이것을 고정합니다.
- **AXIOM 은 JUSTIFIED 가 아니라 ASSUMED** 로 따로 셉니다. axiom 이 하나라도 있으면 `all_merges_justified` 는 0 입니다.
- **catalogue version 과 digest 가 이 빌드와 다르면 replay 를 거부**합니다. 다른 rule set 의 side condition 으로 검사하게 되기 때문입니다.
- witness 후보가 여럿일 수 있어(양쪽 operand class 가 모두 상수를 품는 경우) 모든 후보를 시도한 뒤에만 거부합니다. 처음 후보만 보면 정당한 merge 를 오탐합니다.

시험 `tests/test_egraph_check.cpp` 15개. `JustifiesEveryMergeAWideSaturationProduces` 는 엔진의 모든 rewrite 계열이 발화하는 그래프를 saturate 시켜 rejected 0 을 요구하므로, 엔진이 catalogue 와 어긋나게 rewrite 하면 깨집니다. 나머지는 위조 record(없는 side condition, 잘못된 operator, 없는 rule, stale class, 재정렬, 잘못된 congruence)를 넣어 거부를 고정합니다.

검증: `ctest --preset windows-clang` 345/345 통과.

## 막힌 것

없습니다.

## 다음에 할 것

WU3 결합기. `METHODS.md` 의 "Parallel combination rules" 9개 규칙이 이미 문서에 있으므로 그것을 구현하고 규칙별 시험으로 고정합니다.
