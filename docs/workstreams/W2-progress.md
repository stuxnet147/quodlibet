# W2 진행 기록

브랜치: `stuxnet147/w2-exact-backend`
워크트리: `C:/Users/hkdc/orca/workspaces/quodlibet/w2-exact-backend`
지시서: `docs/workstreams/W2.md`

## 지금 하는 중

WU5. prove.smt-product proof method 와 UNSAT 승격 경계.

## 기준선과 통합

- `6c6a34e` 에서 분기했고 조율자 지시에 따라 `cfbbb20` (origin/main) 위로 rebase 했습니다.
- 작업 단위마다 `git fetch origin` 후 `git rebase origin/main`, 자기 브랜치는 force push 합니다.
- 기준선 `ctest` 98/98 통과 확인. WU1 후 105/105, WU2 후 113/113, WU3 후 125/125. `784e719` (W1/W2/W3 통합) 위로 rebase 한 뒤 WU4 후 168/168.

## 계획한 작업 단위

| 단위 | 내용 | 상태 | 커밋 |
|---|---|---|---|
| WU1 | `quodlibet.source-signature` artifact 와 IR 결합 검사 | 완료 | `530d54b` |
| WU2 | problem schema v2, v1 non-null precondition gate | 완료 | `7b95ff6` |
| WU3 | SMT-LIB `define-fun` 직렬화, product/miter (`src/product.c`) | 완료 | `426b81e` |
| WU4 | SAT model decode 와 concrete replay (`src/replay.c`) | 완료 | 다음 커밋 |
| WU5 | `prove.smt-product` proof method, UNSAT 승격 경계 | 진행 | |
| WU6 | 세 결과 end-to-end 통합 시험 | 대기 | |

## 설계 결정

### WU1. source-signature artifact

- 새 artifact kind `quodlibet.source-signature` schema v1 을 두었습니다. IR bit-vector 타입은 signless 이므로(`ARCHITECTURE.md` IR schema v1 절) signedness, pointer depth, address space, ABI profile 은 이 별도 artifact 만 보존합니다.
- `ql_source_type_kind` 의 1..4 값을 `ql_signature_argument_kind` 와 같게 두고 `_Static_assert` 로 고정했습니다. 그래서 typed precondition 이 이 signature 로 바로 type check 됩니다(`ql_source_signature_precondition_view`).
- C 타입 spelling 표를 `src/c_lower.c` 의 `parse_type_spelling` 과 **일부러 따로** 구현했습니다. `ql_source_signature_bind_ir` 이 두 경로의 결과를 대조하는 실제 교차 검사가 되게 하려는 것입니다. 표에 없는 spelling 은 추측하지 않고 `QL_STATUS_TYPE_MISMATCH` 입니다.
- `precondition_json == NULL` 은 `ql_precondition_parse` 가 `{"schema_version":1,"expression":true}` 로 파싱하므로 null precondition 도 signature 에 묶인 실제 digest 를 갖습니다. WU2 의 problem v2 는 이 성질을 씁니다.

### WU2. problem schema v2 와 proof binding gate

- v1 구조체와 v1 artifact 는 **그대로** 두고 `ql_problem_definition_v2`, `ql_problem_view_v2` 를 새로 더했습니다. `ql_problem_open` 이 schema 1 과 2 를 모두 읽고, v2 도 `ql_problem_get_view` 로 v1 view 를 냅니다(그 view 의 `schema_version` 이 2 로 나옵니다). 다른 워크스트림이 쓰는 v1 경로는 건드리지 않았습니다.
- v2 payload 는 좌우 signature 의 **canonical bytes 를 그대로** JSON 문자열로 넣고 digest 를 따로 적습니다. open 할 때 그 bytes 로 artifact 를 다시 만들어 digest 를 대조합니다. 파싱한 문서를 다시 직렬화해서 digest 가 같기를 기대하는 방식은 취약해서 쓰지 않았습니다.
- 인자 대응은 **전단사**를 강제합니다. 대응되지 않은 인자가 하나라도 있으면 그 입력의 공유 의미가 정의되지 않아 어떤 관계도 진술할 수 없습니다. 대응된 인자쌍은 `ql_source_type_compatible` 로 kind, width, address space, pointer depth 가 전부 같아야 하므로 같은 폭의 signed 와 unsigned 는 서로 대응되지 않습니다. 반환 타입도 같아야 합니다.
- typed-precondition digest 는 **좌측 signature** 기준으로 계산합니다. precondition 의 `arg` index 는 좌측 인자 index 이고 인자 대응이 그것을 우측으로 옮깁니다. 좌우 인자 타입이 쌍마다 동일함을 이미 강제하므로 좌측 digest + 기록된 전단사 + 우측 signature digest 가 함께 양쪽을 묶습니다.
- open 은 기록된 세 digest 를 전부 **다시 계산해서** 대조합니다. 편집된 artifact 는 `QL_STATUS_SCHEMA_MISMATCH` 입니다.
- gate 는 `ql_problem_require_proof_binding` 하나입니다. **schema v1 은 precondition 유무와 무관하게 통과하지 못합니다.** v1 은 인자 대응도 signature digest 도 없으므로 non-null precondition 만 막는 것보다 이쪽이 맞습니다. 지시서가 요구한 "v1 non-null precondition 으로 `PROVED_*` 불가" 는 이 규칙에 포함되고, 그 경우만 별도 메시지로 구분해 시험이 정확히 고정합니다.

### WU3. product/miter

- `ql_smt2_builder` 에 nullary `define-fun` 두 개(`define_bool`, `define_bv`)를 더했습니다. 관계형 인코딩이 중간값을 한 번씩 이름 붙이지 않으면 항 크기가 좌우 CFG 곱으로 커집니다. body 는 그대로 직렬화하고 파싱과 sort check 는 Bitwuzla 만 합니다.
- 인코딩은 acyclic CFG 를 **경로 조건으로 평탄화**합니다. 블록마다 `X_bN` 도달 조건, 간선마다 `X_eF_T`, 값마다 `X_vN` 을 define 합니다. topological order 로 내보내므로 모든 피연산자가 먼저 정의됩니다. PHI 는 들어오는 간선 기호 위의 `ite` 사슬입니다.
- 값 정의는 **전역 함수**입니다. SMT 의 `bvudiv`/`bvsdiv`/`bvshl` 은 전역화되어 있는데, C 의 UB 는 IR 의 `UB_GUARD` 가 담고 있고 모든 정책의 위반식이 관찰 동치를 **양쪽 defined 와 반드시 함께** 묶으므로 전역화된 값이 관찰 주장에 새지 않습니다.
- 관찰 동치(`quodlibet_observation_equal`):
  - 반환값 축은 `(= l_returns r_returns)` 와 `(=> l_returns (= l_return_value r_return_value))` 입니다. trap 이나 발산은 "반환값 없음" 이라는 다른 관찰이므로 반환 여부부터 비교합니다.
  - 종료 축은 `(= l_terminates r_terminates)`, trap 축은 발생과 code 를 함께 비교합니다.
  - memory, volatile, atomics, IO, external call 축은 두 IR 에 해당 effect 와 타입이 **하나도 없음을 먼저 검사**하고 나서 공허하게 성립하는 것으로 처리합니다. effect 가 있으면 인코딩하지 않고 `QL_STATUS_TYPE_MISMATCH` 입니다. 축을 조용히 떨어뜨리지 않습니다.
  - `QL_OBSERVE_UNDEFINED_BEHAVIOR` 는 별도 conjunct 를 만들지 않습니다. `METHODS.md` 가 definedness 를 "selected UB policy 에 따라" 관찰한다고 정의하므로 UB 축은 정책 항이 담당합니다. 별도로 더 강하게 걸면 LANGUAGE_REFINEMENT 에서 replay 로 확인되지 않는 가짜 counterexample 이 납니다.
- UB 정책별 위반식은 `src/product.c` 의 `encode_violation` 에 있습니다. MUST_MATCH 는 정의역 불일치 또는 양쪽 defined 에서의 관찰 불일치, LANGUAGE_REFINEMENT 는 방향에 따라 refined 쪽 defined 를 전제로 한 불일치, COMPARE_WHERE_BOTH_DEFINED 는 교집합 위 불일치입니다.
- **domain query 를 따로 냅니다.** `quodlibet_domain` 은 precondition 과 정책별 비교 정의역이 실제로 비어 있지 않은지 묻습니다. 이것이 UNSAT 이면 miter 의 UNSAT 은 공허하므로 proof 가 아닙니다. `METHODS.md` 는 COMPARE_WHERE_BOTH_DEFINED 에만 이 보고를 요구하지만 모든 정책에 대해 계산합니다.
- prefix 와 두 terminal assertion 을 **분리된 artifact 세 개**로 냅니다. 하나의 prefix 를 두 질의가 공유하고, `push`/`pop` 의미론에 기대지 않습니다.
- 축별 고정 시험: 반환값, trap code, 종료는 각각 축을 끄면 UNSAT, 켜면 SAT 인 쌍으로 고정했습니다. trap 과 발산은 현재 C 슬라이스가 만들 수 없으므로 **IR 을 직접 지어서** 시험합니다. UB 정책과 관계 방향은 `x / y` 와 `if (b == 0) return 0; return a / b;` 쌍으로 세 정책이 서로 다른 답을 내는 것을 고정했습니다.
- 시험 중 발견: `int g(int x){ return x + 1; }` 는 signed overflow UB 때문에 `int f(int x){ return x; }` 와 **정의역이 다릅니다**. MUST_MATCH 에서 반환값 축을 꺼도 위반이 납니다. 인코딩이 맞고 처음 세운 시험 전제가 틀렸습니다.

### WU4. model decode 와 concrete replay

- **W1 의 `src/ir_interp.c` 를 씁니다.** 조율자 안내대로 자체 최소 실행기를 만들지 않았습니다. `ql_ir_interp_run` 이 outcome(RETURN/TRAP/DIVERGE/UNDEFINED_BEHAVIOR/...)과 `ub_reason`, 반환 바이트를 주므로 W2 가 필요한 관찰 tuple 이 그대로 나옵니다.
- model parser 는 작은 S-expression 스캐너입니다. `#b`, `#x`, `(_ bvN W)`, `true`, `false` 를 받고 **폭이 정확히 맞아야** 합니다. 32비트 입력에 8비트 리터럴이 오면 zero-extend 로 추측하지 않고 `QL_STATUS_PARSE_ERROR` 입니다. query 가 선언한 입력이 하나라도 model 에 없으면 역시 오류입니다.
- `relation_violated` 는 `src/product.c` 의 `encode_violation` 을 **구체값 위에서 그대로 다시 씁니다.** 두 경로가 어긋나면 `violated == 0` 이 되어 `UNKNOWN` 으로 떨어집니다. 이것이 이 replay 의 목적입니다.
- **precondition 을 구체적으로 다시 평가합니다.** SMT 쪽 precondition 인코딩이 틀리면 solver 가 문제가 허용하지 않은 witness 를 돌려줄 수 있는데, 그것을 여기서 잡습니다. 평가 못 하면 `precondition_evaluated == 0`, 성립하지 않으면 `precondition_holds == 0`, 둘 중 하나면 `conclusive == 0` 입니다.
- interpreter 가 `UNSUPPORTED`, `STEP_LIMIT`, `ASSUMPTION_VIOLATED`, `TERMINATE` 를 내면 그것은 함수에 대한 진술이 아니므로 `conclusive == 0` 입니다.
- `ql_replay_counterexample_artifact_create` 는 `violated && conclusive` 가 아니면 **직렬화를 거부합니다**. replay 안 된 SAT model 이 counterexample artifact 가 되는 경로가 코드에 없습니다.
- 시험이 고정하는 것: 실제 Bitwuzla model 을 decode 해서 replay 로 확인, 재현되지 않는 model 은 `violated == 0` 이고 artifact 생성 거부, precondition 밖 witness 는 `conclusive == 0`, 잘못된 폭과 누락 입력은 parse error.

## 소유 밖 파일을 고친 것

- `ARCHITECTURE.md` 의 "Input precondition schema" 절 마지막 문단이 problem v2 이후 사실과 어긋나서 그 문단만 고쳤습니다(v2 가 무엇을 묶는지, gate 가 무엇인지, source signature 가 왜 별도 artifact 인지). 워크스트림 소유 표에 없는 파일이라 조율자께 보고합니다.
- `include/quodlibet/quodlibet.h` 우산 헤더에 `signature.h`, `product.h`, `replay.h` 를 알파벳 순서로 넣었습니다.
- `SOLVERS.md` 의 "Deterministic SMT-LIB subset" 절에 `define-fun` 두 형태를 더한 사실과 그 이유를 적었습니다.

## 관찰한 W1 인터페이스 제약

- 현재 `src/c_lower.c` 슬라이스는 **cast expression 을 전혀 지원하지 않습니다**(`cast_expression` 처리 없음). `(int)x` 가 들어가면 `UNKNOWN` 입니다. W2 시험은 암시적 변환만 쓰도록 작성했습니다. 필요 사항이 아니라 관찰 기록입니다.
- 지원 범위: loop-free `if`/`return`, 정수와 `_Bool`, 지역 변수, 산술/비교/논리, `UB_GUARD`. 포인터, 호출, 루프, `switch`/`goto`, `volatile`/`_Atomic` 은 `UNKNOWN`.

## 막힌 것

없음.

## 조율자에게 요청할 것

- (WU5 예정) `src/builtins.c` 에 `prove.smt-product` 등록. `quodlibet methods` 에 나오게 하려면 필요합니다. `src/builtins.c` 는 W2 소유가 아니므로 등록 진입점만 만들고 요청하겠습니다.

## 다음에 할 것

WU5 착수.
