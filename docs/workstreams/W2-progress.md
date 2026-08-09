# W2 진행 기록

브랜치: `stuxnet147/w2-exact-backend`
워크트리: `C:/Users/hkdc/orca/workspaces/quodlibet/w2-exact-backend`
지시서: `docs/workstreams/W2.md`

## 지금 하는 중

WU3. SMT-LIB 직렬화 확장과 product/miter.

## 기준선과 통합

- `6c6a34e` 에서 분기했고 조율자 지시에 따라 `cfbbb20` (origin/main) 위로 rebase 했습니다.
- 작업 단위마다 `git fetch origin` 후 `git rebase origin/main`, 자기 브랜치는 force push 합니다.
- 기준선 `ctest` 98/98 통과 확인. WU1 후 105/105, WU2 후 113/113.

## 계획한 작업 단위

| 단위 | 내용 | 상태 | 커밋 |
|---|---|---|---|
| WU1 | `quodlibet.source-signature` artifact 와 IR 결합 검사 | 완료 | `530d54b` |
| WU2 | problem schema v2, v1 non-null precondition gate | 완료 | 다음 항목 참고 |
| WU3 | SMT-LIB `define-fun` 직렬화, product/miter (`src/product.c`) | 진행 | |
| WU4 | SAT model decode 와 concrete replay (`src/replay.c`) | 대기 | |
| WU5 | `prove.smt-product` proof method, UNSAT 승격 경계 | 대기 | |
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

## 소유 밖 파일을 고친 것

- `ARCHITECTURE.md` 의 "Input precondition schema" 절 마지막 문단이 problem v2 이후 사실과 어긋나서 그 문단만 고쳤습니다(v2 가 무엇을 묶는지, gate 가 무엇인지, source signature 가 왜 별도 artifact 인지). 워크스트림 소유 표에 없는 파일이라 조율자께 보고합니다.
- `include/quodlibet/quodlibet.h` 우산 헤더에 `signature.h` 한 줄을 알파벳 순서로 넣었습니다.

## 관찰한 W1 인터페이스 제약

- 현재 `src/c_lower.c` 슬라이스는 **cast expression 을 전혀 지원하지 않습니다**(`cast_expression` 처리 없음). `(int)x` 가 들어가면 `UNKNOWN` 입니다. W2 시험은 암시적 변환만 쓰도록 작성했습니다. 필요 사항이 아니라 관찰 기록입니다.
- 지원 범위: loop-free `if`/`return`, 정수와 `_Bool`, 지역 변수, 산술/비교/논리, `UB_GUARD`. 포인터, 호출, 루프, `switch`/`goto`, `volatile`/`_Atomic` 은 `UNKNOWN`.

## 막힌 것

없음.

## 조율자에게 요청할 것

- (WU5 예정) `src/builtins.c` 에 `prove.smt-product` 등록. `quodlibet methods` 에 나오게 하려면 필요합니다. `src/builtins.c` 는 W2 소유가 아니므로 등록 진입점만 만들고 요청하겠습니다.

## 다음에 할 것

WU3 착수.
