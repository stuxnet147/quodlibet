# W5 진행 기록

브랜치: `stuxnet147/w5-miter-memory`
지시서: `docs/workstreams/W5.md`

이 파일은 세션이 끊겼을 때 다음 세션이 읽는 유일한 기록입니다. 작업 단위를 커밋할 때 같이 커밋합니다.

## 지금 하는 것

2단계 완료. miter, replay, 관찰 축이 메모리까지 넓어졌고 포인터와 struct 함수가 실제 판정을 받습니다. 다음은 val 코퍼스 측정입니다.

## 기준선

- 시작 커밋 `4a8bf3c` (`main` 의 W5 지시서 커밋)
- 시작 시점 `ctest --preset windows-clang`: 262/262 통과

## 조사 결과: 지금 miter 가 메모리에서 막히는 지점

`src/product.c` 가 loop-free scalar 조각만 받습니다. 막는 곳이 다섯 군데입니다.

| 위치 | 무엇을 거부하는가 |
|---|---|
| `check_ir_fragment` 타입 검사 | `QL_IR_TYPE_POINTER`, `QL_IR_TYPE_MEMORY` 를 포함한 비스칼라 타입 전부 |
| `opcode_is_supported` | `PTR_ADD` `PTR_TO_BV` `BV_TO_PTR` `LOAD` `STORE` `ASSUME` |
| `check_ir_fragment` effect 검사 | `QL_IR_EFFECT_MEMORY` |
| `check_ir_fragment` terminator 검사 | `terminator.memory != INVALID` |
| `declare_inputs` | `QL_SOURCE_TYPE_POINTER` 인자 |

`src/replay.c` 는 `ql_ir_interp_run` 에 object 표를 주지 않고(`ql_ir_interp_options_init` 기본값), 파라미터 개수가 witness 개수와 정확히 같기를 요구합니다. 포인터 함수는 `__memory` 와 base/size 파라미터가 더 붙으므로 지금 형태로는 실행 자체가 시작되지 않습니다.

## 설계 결정

### D1. 메모리는 `(Array (_ BitVec 64) (_ BitVec 8))` 이고 좌우가 초기 메모리를 공유한다

바이트 배열 하나입니다. W 바이트 접근은 select/store 를 W 번 엮습니다. 바이트 순서는 little-endian 이고 이것은 `src/ir_interp.c` 의 `LOAD`/`STORE` 구현과 같은 규약입니다. 폭이 8의 배수가 아닌 접근은 인코딩하지 않고 `UNKNOWN` 으로 남깁니다.

초기 메모리 심볼은 좌우 공용 `mem0` 하나입니다. 두 함수는 같은 메모리 상태에서 출발합니다.

logic 은 `QF_BV` 에서 `QF_ABV` 로 올라갑니다. 메모리를 쓰지 않는 문제는 예전과 같이 `QF_BV` 를 유지합니다.

### D2. object base/size 는 좌우 공용 심볼이고 대응은 인자 대응에서 유도한다

W1 이 확정한 IR 파라미터 배치는 `[C 파라미터][__memory][<이름>.__base, <이름>.__size 포인터 파라미터마다]` 입니다. 따라서 k 번째 object 는 k 번째 포인터 파라미터입니다.

좌우 인자 순서가 다를 수 있으므로(problem v2 의 인자 대응은 전단사이지 항등이 아닙니다) object 번호를 그대로 맞추지 않습니다. 공유 입력 i 가 포인터이면

- 왼쪽 object 번호 = `left_parameter` 보다 앞선 왼쪽 포인터 인자의 수
- 오른쪽 object 번호 = `right_parameter` 보다 앞선 오른쪽 포인터 인자의 수

로 계산하고, 양쪽 모두 같은 공용 심볼 `obj<i>_base` / `obj<i>_size` 에 묶습니다. 그래서 좌우가 아무것도 주고받지 않아도 같은 object 표를 받습니다.

disjointness, 첫 페이지, wrap 없음 세 제약은 **다시 만들지 않습니다.** IR 의 `ASSUME` 을 그대로 인코딩하면 됩니다. 접근 정의성 술어도 마찬가지로 IR 안에 평범한 산술로 이미 있습니다.

### D3. `ASSUME` 은 도달성 조건부로 모으고 domain 과 violation 양쪽에 건다

`(=> <블록 도달> <술어>)` 의 논리곱을 `quodlibet_assumptions` 로 정의합니다. violation 에만 걸면 UNSAT 이 vacuous 인지 알 수 없으므로 domain 에도 같이 겁니다.

### D4. 최종 메모리 비교는 자유 주소 상수 하나로 진술한다

좌우가 같은 object 집합을 받으므로 관찰은 "object 안의 모든 주소에서 최종 바이트가 같다" 입니다. 전칭 한정사를 쓰지 않고 자유 상수 `mem_probe` 하나를 선언합니다.

- violation(SAT 방향): 자유 상수는 존재 한정과 같으므로 "어떤 주소에서 다르다" 를 정확히 표현합니다
- proof(UNSAT 방향): 자유 상수는 전칭 한정과 같으므로 "모든 주소에서 같다" 를 정확히 표현합니다

즉 한 상수로 양방향이 모두 정확합니다. 비교는 `mem_probe` 가 어떤 object 안에 있을 때로 제한하고, 양쪽이 모두 종료할 때만 겁니다. 발산하는 쪽에는 최종 메모리가 없습니다.

`memory_observation` 이 `FINAL_REACHABLE_STATE` 가 아닌 `ORDERED_WRITES` 나 `FULL_TRACE` 이면 인코딩하지 않고 거부합니다. 좁게 해석해서 통과시키지 않습니다.

### D5. (폐기) replay 가 큰 object 를 줄여서 실행한다

건전하지만 쓸모가 없었습니다. 아래 D6 이 대체합니다.

## 완료한 작업 단위

### 1. SMT-LIB builder 의 array 지원

커밋: `4db9068`

`ql_smt2_builder` 에 `(Array ...)` 를 선언하고 정의하는 함수가 없었습니다(`declare_bool`, `declare_bv`, `define_bool`, `define_bv` 뿐). 메모리를 배열로 인코딩하려면 prefix 안에 `(declare-const mem0 (Array (_ BitVec 64) (_ BitVec 8)))` 이 필요하고, 이것은 side 인코딩보다 앞서야 하므로 별도 artifact 로 뗄 수 없습니다.

`src/solver.c` 와 `include/quodlibet/solver.h` 는 W2 소유이므로 조율자에게 물었고, **builder 절에 한해 W5 로 위임**받았습니다. 조건은 다섯이었고 전부 지켰습니다.

1. builder 에 논리를 넣지 않았습니다. `ql_smt2_builder_declare_array` 와 `ql_smt2_builder_define_array` 는 sort 문자열만 씁니다. `select` 와 `store` 는 기존 정의 본문 안에 그대로 들어가고 backend 만 파싱합니다
2. append-only 입니다. 새 `QL_API` 함수 두 개뿐이고 구조체와 기존 시그니처는 그대로입니다
3. transport, snapshot, process 코드는 건드리지 않았습니다
4. `tests/test_solver.cpp` 에 세 시험을 더했습니다. 직렬화 왕복(`SerializesArraySortsDeterministically`), 인자 거부(`RejectsMalformedArrayDeclarations`), **Bitwuzla 0.9.1 실제 QF_ABV solve**(`SolvesRealArrayQueries`, UNSAT 과 SAT 양쪽)
5. `SOLVERS.md` 의 builder 절에 array 를 적었습니다

`ctest` 265/265 통과입니다(기준선 262 + 3).

### 2. miter, replay, 관찰 축의 메모리 확장

커밋: `16f4b15`

D1..D4 를 그대로 구현했고, D5 는 **폐기하고 더 정직한 방식으로 바꿨습니다**(아래 D6).

#### 인코딩 (`src/product.c`)

- 타입 관문이 `POINTER` 와 `MEMORY` 를 받습니다. 포인터 폭이 64가 아니면 거부합니다
- opcode 관문이 `PTR_ADD` `PTR_TO_BV` `BV_TO_PTR` `LOAD` `STORE` `ASSUME` 을 받습니다. `LOAD`/`STORE` 는 `QL_IR_EFFECT_MEMORY` **하나만** 허용하고 volatile/atomic/IO 가 섞이면 거부합니다
- 파라미터 심볼 배정이 `[C 인자][__memory][base,size]*` 를 알고, 뒤쪽 파라미터를 좌우 공용 심볼 `mem0` / `obj<k>_base` / `obj<k>_size` 에 묶습니다. 좌우 포인터 순서가 다르면 인자 대응으로 번호를 옮깁니다
- `ASSUME` 은 `(=> <블록> <술어>)` 로 모아 `quodlibet_assumptions` 가 되고 domain 과 violation 양쪽에 들어갑니다
- 최종 메모리는 return terminator 가 나르는 memory 값의 ite 사슬입니다. 비교는 자유 상수 `mem_probe` 로 진술하고 양쪽 종료를 조건으로 겁니다
- `memory_observation` 이 `FINAL_REACHABLE_STATE` 가 아니면 거부합니다
- 메모리를 쓰는 문제만 logic 이 `QF_ABV` 로 올라갑니다

#### signature bind (`src/signature.c`)

`ql_source_signature_bind_ir` 이 IR 파라미터 수를 signature 인자 수와 정확히 같기를 요구해서 포인터 함수를 전부 막고 있었습니다. 이제 뒤쪽 object 파라미터 꼬리를 **검사합니다**(건너뛰지 않습니다). `__memory` 는 MEMORY 타입에 그 이름이어야 하고, base/size 는 pointer_width 비트 벡터에 `.__base` / `.__size` 로 끝나는 이름이어야 합니다. 로어링이 이 배치를 어기면 miter 가 signature 가 함의하는 것과 다른 object 표를 조용히 받게 되므로, 건너뛰는 것은 답이 아닙니다.

#### replay (`src/replay.c`)

- model 에서 `obj<k>_base`, `obj<k>_size`, 그리고 `mem0` 배열 항을 디코드합니다. Bitwuzla 는 배열을 `(store (store ((as const (Array ...)) <fill>) <i> <v>) ...)` 로 찍고, 바깥 store 가 이깁니다
- object 별 초기 이미지를 fill 과 sparse override 로 만들고, 세 제약(비어있지 않음, 첫 페이지 위, wrap 없음, 서로 disjoint)을 **다시 검사**합니다
- `ql_ir_interp_run` 에 object 표를 주고, memory 파라미터는 바이트 없이 이름만으로 묶습니다
- 메모리를 관찰하면 좌우 최종 이미지를 받아 바이트로 비교하고 그 결과가 관계 판정에 들어갑니다

### D6. 큰 object 는 줄이지 않고 bounded violation 쿼리로 다시 묻는다

D5 는 "model 의 size 가 크면 상한으로 줄여서 실행한다" 였습니다. **실제로 해보니 틀린 선택이었습니다.** Bitwuzla 가 낸 모델은 `obj0_size` 가 약 2^63 이고 포인터 인자가 base 에서 아주 먼 곳을 가리켰습니다. 4096 으로 줄이면 그 접근이 object 밖이 되어 양쪽 다 UB 가 되고, 위반이 재현되지 않아 반례를 놓칩니다. 줄이기는 건전하긴 하지만 쓸모가 없었습니다.

대신 쿼리에 **탐색 전용 terminal** 을 하나 더 넣었습니다. 같은 violation 주장에 object size 상한만 더한 것입니다(`ql_product_query_bounded_violation_artifact`).

- 무제한 violation 이 SAT 인데 model 이 너무 커서 replay 가 결론을 못 내면, bounded 쪽으로 한 번 더 물어 replay 가능한 model 을 얻습니다
- **bounded 의 SAT 은 여전히 진짜 위반**이므로 replay 해서 반례로 낼 수 있습니다
- **bounded 의 UNSAT 은 아무것도 증명하지 않습니다.** proof 경로는 무제한 violation 쿼리만 읽습니다. 이것을 코드와 `METHODS.md` 양쪽에 못박았습니다

replay 는 이제 model 의 배치를 그대로 쓰고, 실체화하기 너무 크면 판정을 내리지 않습니다.

#### 시험

`tests/test_proof_smt_memory.cpp` 7개입니다.

| 시험 | 고정하는 것 |
|---|---|
| `BuildsAnArrayQueryWithSharedObjects` | QF_ABV, 배열 선언, 좌우 공용 object 심볼과 파라미터 서수, assumptions 존재 |
| `ProvesTwoSpellingsOfTheSameWrite` | `*p = *p + 1` 과 `q[0] = q[0] + 1` 이 `PROVED_EQUIVALENT` |
| `ProvesAcrossAStructMemberSpelling` | struct 멤버 접근 쌍이 `PROVED_EQUIVALENT` |
| `AWriteOfADifferentValueIsAReplayedCounterexample` | 같은 바이트에 다른 값을 쓰는 쌍이 replay 확인된 `COUNTEREXAMPLE` |
| `IgnoringMemoryLeavesTheStoredValueDifferenceUnseen` | 같은 쌍이 메모리 관찰을 끄면 `PROVED_EQUIVALENT`. 위 반례가 메모리 축 때문임을 고정 |
| `ADifferentReturnedByteIsACounterexample` | 다른 인덱스를 읽는 쌍이 replay 확인된 `COUNTEREXAMPLE` |
| `RefusesAMemoryObservationFinerThanTheFinalState` | `ORDERED_WRITES` 는 `UNKNOWN` + 진단 |

`ctest` 272/272 통과입니다(265 + 7).

## 막힌 것

- 없음

## 다음에 할 것

1. val 코퍼스에서 로어링된 본문 중 판정까지 간 비율 측정
2. `todo.md` 갱신은 조율자 소유이므로 worker_done 에 수치를 담아 보고
