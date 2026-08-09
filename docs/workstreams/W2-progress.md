# W2 진행 기록

브랜치: `stuxnet147/w2-exact-backend`
워크트리: `C:/Users/hkdc/orca/workspaces/quodlibet/w2-exact-backend`
지시서: `docs/workstreams/W2.md`

## 지금 하는 중

WU2. problem schema v2 와 v1 non-null precondition gate.

## 기준선과 통합

- `6c6a34e` 에서 분기했고 조율자 지시에 따라 `cfbbb20` (origin/main) 위로 rebase 했습니다.
- 작업 단위마다 `git fetch origin` 후 `git rebase origin/main`, 자기 브랜치는 force push 합니다.
- 기준선 `ctest` 98/98 통과 확인. WU1 후 105/105.

## 계획한 작업 단위

| 단위 | 내용 | 상태 | 커밋 |
|---|---|---|---|
| WU1 | `quodlibet.source-signature` artifact 와 IR 결합 검사 | 완료 | `b3481a6` |
| WU2 | problem schema v2, v1 non-null precondition gate | 진행 | |
| WU3 | SMT-LIB `define-fun` 직렬화, product/miter (`src/product.c`) | 대기 | |
| WU4 | SAT model decode 와 concrete replay (`src/replay.c`) | 대기 | |
| WU5 | `prove.smt-product` proof method, UNSAT 승격 경계 | 대기 | |
| WU6 | 세 결과 end-to-end 통합 시험 | 대기 | |

## 설계 결정

### WU1. source-signature artifact

- 새 artifact kind `quodlibet.source-signature` schema v1 을 두었습니다. IR bit-vector 타입은 signless 이므로(`ARCHITECTURE.md` IR schema v1 절) signedness, pointer depth, address space, ABI profile 은 이 별도 artifact 만 보존합니다.
- `ql_source_type_kind` 의 1..4 값을 `ql_signature_argument_kind` 와 같게 두고 `_Static_assert` 로 고정했습니다. 그래서 typed precondition 이 이 signature 로 바로 type check 됩니다(`ql_source_signature_precondition_view`).
- C 타입 spelling 표를 `src/c_lower.c` 의 `parse_type_spelling` 과 **일부러 따로** 구현했습니다. `ql_source_signature_bind_ir` 이 두 경로의 결과를 대조하는 실제 교차 검사가 되게 하려는 것입니다. 표에 없는 spelling 은 추측하지 않고 `QL_STATUS_TYPE_MISMATCH` 입니다.
- `precondition_json == NULL` 은 `ql_precondition_parse` 가 `{"schema_version":1,"expression":true}` 로 파싱하므로 null precondition 도 signature 에 묶인 실제 digest 를 갖습니다. WU2 의 problem v2 는 이 성질을 씁니다.

## 관찰한 W1 인터페이스 제약

- 현재 `src/c_lower.c` 슬라이스는 **cast expression 을 전혀 지원하지 않습니다**(`cast_expression` 처리 없음). `(int)x` 가 들어가면 `UNKNOWN` 입니다. W2 시험은 암시적 변환만 쓰도록 작성했습니다. 필요 사항이 아니라 관찰 기록입니다.
- 지원 범위: loop-free `if`/`return`, 정수와 `_Bool`, 지역 변수, 산술/비교/논리, `UB_GUARD`. 포인터, 호출, 루프, `switch`/`goto`, `volatile`/`_Atomic` 은 `UNKNOWN`.

## 막힌 것

없음.

## 조율자에게 요청할 것

- (WU5 예정) `src/builtins.c` 에 `prove.smt-product` 등록. `quodlibet methods` 에 나오게 하려면 필요합니다. `src/builtins.c` 는 W2 소유가 아니므로 등록 진입점만 만들고 요청하겠습니다.

## 다음에 할 것

WU2 착수.
