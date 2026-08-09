# W6 진행 기록

브랜치: `stuxnet147/w6-evidence-pipeline` (워크트리 기본 브랜치. 조율자 확인 완료)
지시서: `docs/workstreams/W6.md`

## 지금 하는 것

WU2. `src/egraph_check.c` 로 merge log 를 e-graph 코드와 독립적으로 재생하는 checker 를 만드는 중입니다.

## 작업 단위

| 단위 | 내용 | 상태 | 커밋 |
|---|---|---|---|
| WU1 | rewrite rule catalogue (W6.md 2번) | 완료 | (아래) |
| WU2 | 독립 merge replay checker `src/egraph_check.c` (1번) | 진행 중 | |
| WU3 | 증거 우선 결합 `src/combine.c` (3번) | 대기 | |
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

## 막힌 것

없습니다.

## 다음에 할 것

WU2 checker. catalogue 의 shape 과 조건 비트를 소비해서 각 merge record 를 자체 union-find 위에서 재생합니다.
