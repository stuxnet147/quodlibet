# W6 진행 기록

브랜치: `stuxnet147/w6-evidence-pipeline` (워크트리 기본 브랜치. 조율자 확인 완료)
지시서: `docs/workstreams/W6.md`

## 지금 하는 것

W6.md 의 다섯 항목을 모두 닫았습니다. 마지막 커밋 후 전 구성 검증을 정리하는 중입니다.

## 작업 단위

| 단위 | 내용 | 상태 | 커밋 |
|---|---|---|---|
| WU1 | rewrite rule catalogue (W6.md 2번) | 완료 | (아래) |
| WU2 | 독립 merge replay checker `src/egraph_check.c` (1번) | 완료 | (아래) |
| WU3 | 증거 우선 결합 `src/combine.c` (3번) | 완료 | (아래) |
| WU4 | persistent artifact/evidence cache `src/cache.c` (4번) | 완료 | (아래) |
| WU5 | cache key 완전성 통합 검증 (5번) | 완료 | (아래) |

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

### WU3 가 실제로 넣은 것

`src/combine.c` 는 `METHODS.md` 의 병렬 결합 규칙 1..8 을 구현합니다(9번 cache key 는 WU5).

핵심 설계 결정은 **건전하지 않은 수단을 API 에서 표현할 수 없게 만든 것**입니다. 동의한 method 수를 세는 필드가 인터페이스에도 구현에도 없고, method 우선순위 표도 없으며, proof 와 counterexample 을 화해시키는 인자도 없습니다.

- 규칙 1: 각 input 이 자기가 답한 contract(problem digest, contract digest, IR semantics version, relation, UB policy, observation projection)를 다시 진술합니다. 하나라도 다르면 결합하지 않고 status 오류로 거부하며 어긋난 축을 메시지에 적습니다.
- 규칙 2: `ql_policy_evidence_v1` 을 그대로 재사용해 단일 결과 경계의 규율을 결합 시점에 다시 강제합니다. `PROVED_*` 는 checked proof 또는 request 가 명시한 trusted backend 만, `COUNTEREXAMPLE` 은 replay 된 witness 만 살아남고 budget 소진은 무조건 철회입니다. 철회는 항상 `UNKNOWN` 이고 더 약한 긍정 주장으로 내려가지 않습니다.
- 규칙 3: 요청한 relation 을 정확히 세우는 proof 만 판정합니다. 반대 방향 refinement 두 개의 합성은 호출자가 `allow_refinement_composition` 으로 명시할 때만 일어납니다.
- 규칙 4: checked proof 와 replayed counterexample 이 만나면 `inconsistent` 이고 verdict 는 `UNKNOWN` 이며 양쪽 input 을 지목합니다. 우선순위, 순서, 다수결 어느 것으로도 해소하지 않습니다.
- 규칙 6: bound 는 vector 째로 보존하고 서로 다른 차원을 합치지 않습니다.
- 규칙 7: normalization 위에서 돈 결과는 `depends_on` 으로 그것을 가리키고, normalization proof 가 유효하지 않으면 철회됩니다.
- 규칙 8: 순서는 declaration order, evidence digest, input 위치 순입니다. `completion_order` 는 구조체에 기록만 하고 읽지 않습니다.

시험 `tests/test_combine.cpp` 24개. 규칙별로 하나 이상이고, 특히 `ThreeAgreeingUncheckedProofsStillDecideNothing` 과 `OneCheckedProofOutweighsAnyNumberOfUncheckedDisagreements` 가 다수결 금지를 양쪽에서 고정합니다.

검증: `ctest --preset windows-clang` 369/369 통과.

### WU4 와 WU5 가 실제로 넣은 것

`src/cache.c` 는 BLAKE3 cache key 로 artifact 와 evidence envelope 을 디스크에 보관합니다. 위치는 호출자가 정하고 모듈이 스스로 디렉터리를 고르지 않습니다.

설계 결정:

- **record 마다 content digest 와 encoding 전체 digest 를 싣고 load 마다 둘 다 검증**합니다. 저장된 key 도 record 안에 적어 조회 key 와 비교하므로, 파일이 엉뚱한 자리로 가도 다른 질문의 답으로 쓰이지 않습니다.
- **검증 실패는 miss 가 아니라 거부**이고 통계에서도 분리합니다. 잘못된 답을 주는 저장소가 비어 있는 저장소처럼 보이면 안 됩니다.
- **같은 key 에 다른 내용을 저장하면 `ALREADY_EXISTS`** 이고 덮어쓰지 않습니다. 한 key 에 두 답이 있다는 것은 key 가 축을 빠뜨렸다는 뜻이고, 덮어쓰면 그 결함이 조용한 오답이 됩니다.
- evidence record 는 envelope 의 identity 를 같이 저장해 load 시 cache key 를 **다시 계산**합니다. 파일에 적힌 key 를 믿지 않습니다.

WU5 는 `tests/test_cache_key.cpp` 16개로 축을 하나씩 바꿔 key 가 움직이는지, 그리고 저장소가 실제로 두 답을 분리해 돌려주는지를 함께 확인합니다. semantic contract 는 problem artifact digest 를 통해 key 에 들어가므로 relation, UB policy, observation projection 과 두 observation mode, compiler/codegen 집합, target feature, precondition, 양쪽 source 를 각각 변주했습니다.

**backend version 은 전용 필드가 없습니다.** `method_version` 이나 canonical option 에 실려야 하며, 시험이 이 요구를 양쪽에서 고정합니다. option 에 backend 를 적으면 key 가 갈리는 것과, 아무 데도 적지 않으면 두 solver 빌드가 같은 key 로 충돌한다는 것을 모두 확인합니다. 이것이 W6.md 5번이 막으려는 결함 그 자체입니다.

검증: `ctest --preset windows-clang` 397/397 통과.

## 막힌 것

없습니다.

## 다음에 할 것

W6.md 의 다섯 항목은 모두 닫혔습니다. `todo.md` 의 W6 절 중 AIG/SAT certificate checker, concrete differential refutation, CHC/PDR 은 각각 W10 과 다른 워크스트림 소관이라 이 지시서 범위 밖입니다.
