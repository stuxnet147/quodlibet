# W5 진행 기록

브랜치: `stuxnet147/w5-miter-memory`
지시서: `docs/workstreams/W5.md`

이 파일은 세션이 끊겼을 때 다음 세션이 읽는 유일한 기록입니다. 작업 단위를 커밋할 때 같이 커밋합니다.

## 지금 하는 것

0단계. 코드 조사와 인코딩 설계를 마쳤습니다. 조율자에게 SMT-LIB builder 의 array 확장 허가를 물었습니다.

## 기준선

- 시작 커밋 `4a8bf3c` (`main` 의 W5 지시서 커밋)
- `cmake --preset windows-clang` + build 통과. CTest 기준선은 첫 작업 단위 커밋에서 기록합니다

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

### D5. replay 의 object 표는 model 을 따르되 model 과 같을 필요가 없다

replay 는 "이 구체 입력에서 관계가 깨지는가" 를 독립으로 다시 확인하는 것이므로, 넘겨주는 배치가 모델의 세 제약을 만족하는 적법한 배치이기만 하면 판정이 건전합니다. 그래서 model 의 size 가 replay 가 실체화하기에 너무 크면 상한으로 줄여서 실행합니다. 줄인 배치에서 위반이 재현되면 그것은 진짜 반례이고, 재현되지 않으면 `UNKNOWN` 입니다. 어느 쪽도 없는 사실을 만들지 않습니다.

## 막힌 것

### 조율자 판단 대기: SMT-LIB builder 의 array 지원

`ql_smt2_builder` 에 `(Array ...)` 를 선언하고 정의하는 함수가 없습니다(`declare_bool`, `declare_bv`, `define_bool`, `define_bv` 뿐). 메모리를 배열로 인코딩하려면 prefix 안에 `(declare-const mem0 (Array (_ BitVec 64) (_ BitVec 8)))` 이 필요하고, 이것은 side 인코딩보다 앞서야 하므로 별도 artifact 로 뗄 수 없습니다.

`src/solver.c` 는 W2 소유이고 W2 지시서는 완료 상태입니다. 제안은 append-only 함수 두 개(`ql_smt2_builder_declare_array`, `ql_smt2_builder_define_array`)이고 기존 구조체와 ABI 는 건드리지 않습니다.

## 다음에 할 것

1. product.c 메모리 인코딩 (D1..D4)
2. replay.c object 표 복원과 메모리 replay (D5)
3. 관찰 축과 `tests/test_proof_smt_memory.cpp`
4. val 코퍼스에서 로어링된 본문 중 판정까지 간 비율 측정
