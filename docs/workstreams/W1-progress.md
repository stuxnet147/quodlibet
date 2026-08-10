# W1 진행 기록

브랜치: `main`
지시서: `docs/workstreams/W1.md`

이 파일은 세션이 끊겼을 때 다음 세션이 읽는 유일한 기록입니다. 작업 단위를 커밋할 때 같이 커밋합니다.

## 지금 하는 것

G9 로어링 커버리지를 버킷 단위로 좁히는 중입니다. train IR 로어링은 **20,279 / 29,880 (67.87%)**이고 전부 verifier를 통과했으며 status 실패는 0입니다. 최신 큰 첫 차단 버킷은 `unsupported_type` 4,499, `uninitialized_read` 2,448, `type_error` 702입니다. 구조화 루프 버킷은 0이 되었습니다. 다음 단위는 타입과 definite initialization의 메시지별 큰 원인을 닫습니다.

## 기준선

- 시작 커밋 `6c6a34e`, 조율자의 커버리지 기준선 `cfbbb20` 위로 rebase 완료
- 시작 시점 `ctest --preset windows-clang`: 98/98 통과, 9.73초

### 조율자가 준 커버리지 기준선 (`docs/coverage/baseline-20260810.md`)

train 스플릿 서로 다른 C 본문 29,893 기준입니다.

| 단계 | 비율 |
|---|---:|
| 파싱 | 99.96% |
| 프런트엔드 수용 | 99.81% |
| IR 로어링 `SUPPORTED` | **0.18%** (53개) |

첫 차단 사유는 `unsupported_type` 53.86%, `unsupported_pointer` 43.91% 로 둘이 97.77% 입니다. **파서는 이미 G9 관문을 넘었고 남은 것은 전부 IR 로어링입니다.** 그 표는 첫 차단 사유이므로 타입과 포인터를 닫은 뒤 반드시 다시 재야 합니다. 빠른 반복은 val 1,050 본문입니다.

이 기준선이 2단계의 우선순위를 확정했습니다. W1.md 가 예상한 순서(정수 promotion 먼저)와 달리 **타입 폭과 포인터가 먼저**입니다. 측정이 순서를 이깁니다.

## 현재 코드 상태 조사 결과

### `src/ir.c` 가 이미 하는 검사

`graph_validate()` 가 builder `finish` 와 artifact `parse_graph` 양쪽에서 돌고, 다음을 이미 검사합니다.

- 타입표 정합성, pointer element 전방 참조 금지
- value definition kind 별 필드 규칙, 상수 바이트 폭과 canonical 표현
- opcode 별 operand/result arity 와 타입 규칙, terminator 종류별 필드 규칙
- 블록 명령 목록이 명령 전체를 정확히 분할하는지, PHI 가 블록 앞머리에 모여 있는지
- 도달 가능성, v1 acyclic 제약, 지배자 트리와 SSA 지배관계, PHI 의 incoming block 집합

### `src/ir.c` 가 하지 않던 검사 (verifier 가 새로 추가)

- **UB guard 의무**. 부분 연산(`SDIV` `UDIV` `SREM` `UREM` `SHL` `LSHR` `ASHR` `LOAD` `STORE`)에 데이터 의존하는 관찰이 그 사이를 가르는 `UB_GUARD` 없이 일어나는지
- effect 비트와 opcode 부류의 정확한 대응 (`ir.c` 는 `LOAD`/`STORE` 의 MEMORY, `CALL` 의 CALL 만 봄)
- extension opcode 거부

### 현재 로어링의 UB 처리 방식

`src/c_lower.c` 는 UB guard 를 연산 지점에 붙이지 않고 `lower_value.defined` (bool 값) 와 `may_ub` 플래그로 정의성 술어를 전파한 뒤, 관찰 지점에서 `emit_ub_guard()` 로 `UB_GUARD(defined)` 하나를 냅니다. guard 지점은 다섯 곳입니다.

| 위치 | 지점 |
|---|---|
| `c_lower.c:2098` | 대입 |
| `c_lower.c:2288` | 선언 초기화 |
| `c_lower.c:2417` | `if` 조건 |
| `c_lower.c:2540` | `return` |
| `c_lower.c:2586` | expression statement |

따라서 verifier 의 UB 규칙은 "연산 옆에 guard 가 있는가" 가 아니라 **"부분 연산에 데이터 의존하는 관찰 지점과 그 연산 사이를 지배하는 `UB_GUARD` 가 가르는가"** 입니다.

`&&` 와 `||` 는 `lower_logical_expression()` 이 값을 SELECT 로 eager 하게 계산하되 정의성만 `!rhs_needed || right_defined` 로 단락시킵니다. 즉 IR 은 RHS 를 계산하지만 그 정의성은 필요할 때만 요구합니다. interpreter 는 이 모델과 맞아야 하므로 부분 연산이 즉시 trap 하지 않고 **undefined 표시값(poison)** 을 내는 방식이어야 합니다.

## 설계 결정

### D1. verifier 는 builder 검증과 코드를 공유하지 않는다

`src/ir_verify.c` 는 공개 reader API (`ql_ir_get_view`, `ql_ir_block_at`, `ql_ir_instruction_at`, `ql_ir_value_at`, `ql_ir_type_at`) 만 써서 CFG, 지배자, 타입 규칙을 **독립적으로 다시 유도**합니다. `src/ir.c` 의 내부 함수를 재사용하면 한쪽 버그가 다른 쪽을 가려서 검증 가치가 사라집니다. 중복 구현은 의도한 비용입니다.

지배자도 알고리즘을 일부러 다르게 골랐습니다. `ir.c` 는 위상순서 LCA 로 immediate dominator 트리를 만들고, verifier 는 **지배자 집합 비트벡터를 위상순서로 한 번 교집합**해서 만듭니다. schema v1 이 acyclic 이므로 한 패스로 고정점입니다.

비트벡터 비용 때문에 블록 16384 개 상한이 있습니다. 넘으면 **명시적으로 거부**하고 부분 검증하지 않습니다(`QL_IR_VERIFY_MODULE`).

### D2. verifier 는 status 코드를 새로 만들지 않는다

`include/quodlibet/status.h` 는 W1 소유가 아닙니다. `ql_ir_verify()` 는 기존 status 로 매핑하고, 정확한 사유와 위치는 `ql_ir_verify_report_v1` 이 담습니다. 인자 오류(null, ABI)와 검증 실패를 구별하려면 report 의 `code` 를 봅니다. 인자 오류일 때는 `QL_IR_VERIFY_OK` 로 남습니다.

### D3. UB guard 의무는 배치만 검사하고 충분성은 interpreter 가 본다

verifier 는 **guard 의 존재와 배치**를 구조적으로 검사합니다. guard 술어가 실제로 충분한지(`b != 0` 를 빠뜨리지 않았는지)는 SMT 없이 구조 검사로 못 봅니다. 그 부분은 IR interpreter 가 구체 입력마다 담당합니다.

- verifier: 의무가 걸린 곳에 guard 가 있는가 (모든 입력에 대해, 구조적으로)
- interpreter: 그 guard 술어가 맞는가 (구체 입력마다, 실행으로)

### D4. guard 자신의 피연산자는 관찰이 아니다

정의성 술어는 부분 연산 결과를 정당하게 읽습니다. signed left shift 로어링은 넓은 타입 `SHL` 결과를 재확장과 비교해서 좁은 shift 가 정의되는지 판단합니다. guard 피연산자를 관찰로 세면 이 정당한 패턴이 거짓 위반이 됩니다.

### D5. PHI 는 의무의 방출 지점이다

join 블록의 guard 는 어느 분기도 지배하지 못하므로 PHI 로 들어오는 각 값의 의무는 **그 값이 온 선행 블록 끝까지** 해소되어 있어야 합니다. 현재 로어링이 분기 안 대입 지점에서 guard 를 내므로 이 규칙과 맞습니다.

## ir.h 확장 기록

`MEMORY_IMAGE` opcode 와 명령의 `image`/`image_size` payload 를 더하고 artifact schema 를 v2 로 올렸습니다. 바이트를 `symbol` 에 싣지 않은 것은 `symbol` 이 NUL 을 거부하는 텍스트 계약이고 문자열 이미지는 NUL 로 끝나기 때문입니다.

W2 가 `include/quodlibet/ir.h` 를 읽고 있으므로 append-only 확장만 하고 여기에 한 줄씩 남깁니다.

- (아직 없음. `ir_verify.h` 는 새 헤더이고 `ir.h` 자체는 건드리지 않았습니다)

## 공유 파일 접촉

- `include/quodlibet/quodlibet.h` 에 `#include "quodlibet/ir_verify.h"` 한 줄 추가. 이 umbrella 헤더는 누구의 소유도 아니고 W2/W3 도 새 헤더를 넣을 것이므로 충돌 가능성을 여기 적어 둡니다.

## 메모리 모델 계약 (W2 가 miter 에서 그대로 써야 하는 것)

조율자가 flat address space 로 확정했습니다(2026-08-10). `ARCHITECTURE.md` 의 profile 절에 전문이 있습니다. 여기에는 W2 가 바로 쓸 형태로 적습니다.

### 모델

`ASM2C_GNU_V1` 의 메모리는 **평평한 64비트 주소 공간**입니다. 포인터는 주소일 뿐이고 그 이상의 provenance 를 담지 않습니다. 이 프로파일의 원재료가 x86-64 SysV 목적 코드에서 복원한 C 라서 `(char *)p + n` 과 정수 왕복이 일상적이기 때문입니다.

저장 영역은 **object** 로 기술합니다. 각 object 는 base 주소와 크기를 갖고, 세 가지 제약이 항상 겁니다.

1. 서로 다른 살아있는 object 의 바이트 범위는 **disjoint** 합니다
2. 모든 object 는 **첫 페이지보다 위**에 있습니다 (`QL_IR_INTERP_FIRST_OBJECT_ADDRESS` = 0x1000). 주소 0 은 어떤 object 에도 속하지 않으므로 null 역참조가 항상 undefined 입니다
3. object 의 범위는 주소 공간을 **wrap 하지 않습니다**

`ql_ir_interp_run` 이 이 셋을 검사하고, 어기면 실행을 거부합니다. 모델이 인정하지 않는 배치에서 답을 내지 않습니다.

### 접근 정의 조건

주소 `a` 에서 `W` 바이트 접근이 정의되는 조건입니다.

```
defined(a, W)  :=  (어떤 살아있는 object O 에 대해
                      O.base <=u a  and  a - O.base <=u O.size
                      and  O.size - (a - O.base) >=u W)
                   and  a mod natural_alignment(W) == 0
```

`natural_alignment(W)` 는 W 가 16 이하의 2의 거듭제곱이면 W, 아니면 1 입니다. C 스칼라 배치가 아닌 폭에는 정렬을 요구하지 않습니다.

### 이 술어를 두 번 구현하지 않는 방법

조율자가 "ir_interp 와 miter 가 한 글자까지 같은 술어를 써야 한다"고 했습니다. 답은 **구현을 둘로 두지 않는 것**입니다.

- **증명에 쓰이는 술어는 IR 안에 있습니다.** 로어링이 object 의 base 와 size 를 IR 파라미터로 받아, 접근 지점마다 위 조건을 평범한 bit-vector 연산으로 계산하고 `UB_GUARD` 로 감쌉니다. interpreter 는 그냥 평범한 opcode 를 실행하고, W2 의 miter 도 그냥 평범한 opcode 를 인코딩합니다. **술어는 한 곳에만 있습니다.**
- interpreter 가 `LOAD`/`STORE` 의 부분성을 **독립으로** 한 번 더 압니다. 이것은 중복이 아니라 교차 검증입니다. guard 가 빠지거나 약하면 구체 입력에서 `GUARD_INSUFFICIENT` 로 잡힙니다. verifier 와 builder 를 분리한 것과 같은 이유입니다.

**W2 는 base/size 를 따로 만들어 낼 필요가 없습니다.** IR 이 이미 그것을 파라미터로 갖고 있고 guard 도 IR 안에 있으므로, miter 는 위 세 제약(disjoint, 첫 페이지 위, wrap 없음)만 좌우 공통 assumption 으로 걸면 됩니다. 그 형태는 로어링이 붙는 다음 작업 단위에서 확정해 여기 다시 적습니다.

### object 의 IR 파라미터 배치 (확정)

IR 파라미터 목록은 이 순서입니다.

```
[ C 파라미터, source 순서 ]
[ __memory,  메모리를 쓰는 함수마다 한 개 ]
[ __trace,   본문이 호출을 할 때 한 개 ]
[ <이름>.__base 와 <이름>.__size, object 마다 ]
    object 순서: 포인터 파라미터(source 순서), 지역 저장소, 전역, 문자열 리터럴
```

지역 저장소는 주소를 잡은 스칼라와 배열과 집합체입니다. 배열과 집합체는 값으로 살 수 없으므로 주소를 적어 두지 않아도 object 를 받습니다.

근거입니다.

- **앞의 N개가 C 인자와 그대로 대응**합니다. problem v2 의 인자 대응이 좌우 IR 파라미터 인덱스를 그냥 쓰면 됩니다
- object 표는 **포인터 파라미터에서 유도**되므로 좌우가 아무것도 주고받지 않아도 같은 표를 얻습니다. 같은 signature 면 같은 표입니다
- 포인터 파라미터가 없는 함수는 예전과 **완전히 같은 IR** 을 냅니다. 쓰지도 않을 memory 파라미터가 붙지 않습니다
- base/size 는 `bv64` 이고 `__memory` 는 유일한 MEMORY 타입 값이라 이름과 타입 둘 다로 찾을 수 있습니다

entry 블록이 모델의 세 제약을 `ASSUME` 으로 겁니다. object 마다 `FIRST <= base`, `size != 0`, `base <= base + size`(wrap 없음)이고, 서로 다른 object 쌍마다 `base_i + size_i <= base_j 또는 base_j + size_j <= base_i`(disjoint)입니다. **W2 는 이것을 다시 만들 필요가 없습니다. IR 안에 이미 있습니다.**

접근 guard 도 IR 안에 있습니다. 접근마다 `UB_GUARD(OR_i in_object_i(a, W) AND aligned(a, W))` 이고, `in_object_i` 는 `base_i <= a`, `a - base_i <= size_i`, `W <= size_i - (a - base_i)` 입니다. interpreter 의 `interp_access_defined` 와 같은 형태이고, miter 는 그냥 opcode 를 인코딩하면 됩니다.

## W2 가 볼 인터페이스

### `ql_ir_verify` (`include/quodlibet/ir_verify.h`)

```c
ql_status ql_ir_verify(const ql_allocator *allocator, const ql_ir *ir,
                       ql_ir_verify_report_v1 *report, ql_error *error);
```

`QL_STATUS_OK` 는 검증 통과입니다. 실패는 매핑된 status 와 `report.code`(`ql_ir_verify_code`), `report.block/instruction/value`, `report.message` 로 옵니다. **W2 는 product program 을 만들기 전에 좌우 IR 각각에 이것을 걸어도 됩니다.**

### `ql_ir_interp_run` (`include/quodlibet/ir_interp.h`)

**이것이 W2 의 counterexample replay 부품입니다.** SAT model 을 typed input 으로 decode 한 뒤 좌우 IR 에 각각 이것을 돌려서 관찰값을 비교하면 replay 가 됩니다.

```c
ql_status ql_ir_interp_run(const ql_allocator *allocator, const ql_ir *ir,
                           const ql_ir_interp_input_v1 *inputs,
                           size_t input_count,
                           const ql_ir_interp_options_v1 *options,
                           ql_ir_interp_result_v1 *result, ql_error *error);
```

계약입니다.

- 입력 바이트 인코딩은 **IR 상수와 완전히 같습니다.** 정확한 폭, little-endian, 마지막 바이트의 남는 상위 비트는 0, `_Bool` 은 1바이트 0 또는 1
- 모듈의 **모든 parameter 를 정확히 한 번씩** 묶어야 합니다. 빠지거나 겹치면 status 실패입니다
- status 실패는 "실행을 시작하지 못했다"이지 함수에 대한 판정이 아닙니다
- 결과 `outcome` 은 `RETURN` `TRAP` `TERMINATE` `UNDEFINED_BEHAVIOR` `DIVERGE` `ASSUMPTION_VIOLATED` `UNSUPPORTED` `STEP_LIMIT` 입니다
- **`UNSUPPORTED` 는 판정이 아닙니다.** 포인터, 메모리, 호출, event trace, 부동소수는 아직 모델이 없습니다. 어떤 proof method 도 이것을 함수에 대한 진술로 읽으면 안 됩니다
- `UNDEFINED_BEHAVIOR` 일 때 `ub_reason` 이 `GUARD_FAILED` `GUARD_UNDEFINED` `GUARD_INSUFFICIENT` `TERMINATOR` 중 하나를 줍니다
- 반환값은 `has_value`, `value_type`, `value[value_size]` 로 옵니다. 부호는 IR 이 갖고 있지 않으므로 호출자가 source signature 로 해석합니다
- bit-vector 폭 상한은 256 비트입니다. 64비트 signed 곱셈이 128비트 중간값을 쓰므로 여유를 둔 값입니다
- **메모리를 쓰는 모듈은 `options.objects` 로 object 표를 줍니다.** MEMORY 타입 파라미터는 바이트를 받지 않고 이름만으로 묶입니다(초기 이미지는 object 표가 줍니다). `object.final_image` 를 주면 실행이 끝난 메모리 이미지를 거기에 씁니다. differential 시험이 메모리 관찰을 비교할 때 쓰는 통로입니다
- 메모리 값은 SSA 라 **오래된 버전도 계속 읽힙니다.** store 뒤에 이전 memory 값을 대상으로 load 하는 IR 이 적법하므로, 버전을 write 사슬로 들고 있습니다
- undefined 값을 store 하면 결과 메모리 전체가 undefined 가 됩니다. 바이트 단위 추적보다 보수적인 쪽이고, 그것을 읽는 관찰이 여전히 guard 부족을 보고합니다

undefined 전파 규칙은 로어링과 맞물려 있습니다.

- 부분 연산은 즉시 멈추지 않고 **undefined 표시값**을 냅니다. 순수 연산을 타고 전파됩니다
- `SELECT` 만 예외로 **조건과 선택된 쪽에서만** 전파합니다. 로어링이 `&&` 와 `||` 의 양쪽을 eager 하게 계산하고 정의성만 단락시키기 때문입니다. 선택되지 않은 쪽에서 전파하면 C 에 없는 UB 를 만들어 냅니다
- undefined 가 **관찰될 때만** 실행이 UB 가 됩니다. `UB_GUARD` 술어가 거짓이거나, 분기 조건이나 반환값이 undefined 이거나, UB terminator 입니다

## 완료한 작업 단위

### 1. IR verifier (`src/ir_verify.c`, `include/quodlibet/ir_verify.h`)

커밋: `9c07bb2`

- 공개 reader API 만으로 타입표, 값표, 참조, 블록 소유, terminator, CFG 도달성, v1 acyclic, 지배자, SSA 지배관계, PHI 선행 블록 대응, effect 규칙, UB guard 의무를 검사
- `tests/test_ir_verify.cpp` 8개 시험
- `tests/test_c_lower.cpp` 의 `ResultView()` 에 verifier 관문을 넣어 **로어링 시험 전부**가 자동으로 검증을 거치게 함
- `ctest` 106/106 통과 (기준선 98 + 8)

### 2. IR concrete interpreter 와 differential 시험

커밋: `82424ba`

- `src/ir_interp.c`, `include/quodlibet/ir_interp.h`. 320비트 내부 표현의 고정 용량 bit-vector 산술(add, sub, mul, udiv/sdiv/urem/srem, shl/lshr/ashr, 비교, zext/sext/trunc)을 직접 구현했습니다. `__int128` 같은 컴파일러 확장에 기대지 않습니다
- 나눗셈은 restoring shift-subtract 이고 곱셈은 32비트 limb schoolbook 이라 64비트 중간 타입만 씁니다
- `tests/test_ir_interp.cpp` 10개 시험
- `tests/test_ir_differential.cpp` 3개 시험, 20개 C 함수

#### differential 시험 설계

**참조는 C 의 모델이 아니라 같은 C 함수를 이 시험 바이너리를 만드는 그 컴파일러가 컴파일해서 실제로 돌린 것입니다.** `QL_DIFF_FUNCTION` 매크로가 함수 정의와 로어링에 넘길 소스 문자열을 **한 텍스트에서** 만들기 때문에 둘이 갈라질 수 없습니다.

이 방식이 "clang 이 없는 환경" 문제를 없앱니다. 이 시험 바이너리가 존재한다는 것이 참조가 컴파일되었다는 뜻이므로 비교는 항상 실행됩니다. 미실행 구간이 없습니다.

두 가지 제약이 따라옵니다.

- **타겟 ABI 는 x86_64 Linux LP64 라 `long` 이 64비트인데 Windows 는 32비트입니다.** 그래서 시험은 타겟과 두 호스트가 폭에 동의하는 타입만 씁니다. `char`, `short`, `int`, `unsigned`, `long long`. `long` 은 일부러 뺐습니다. 그것을 쓰면 서로 다른 두 함수를 비교하게 됩니다
- **참조를 UB 입력으로 부르지 않습니다.** UB 를 실제로 실행하면 비교 자체가 무의미해집니다. 각 사례가 어떤 입력이 정의되는지 말하고, overflow 정의성은 손으로 다시 쓴 규칙이 아니라 컴파일러의 `__builtin_*_overflow` 에서 옵니다

검사는 양쪽입니다.

- 정의된 입력: interpreter 가 `RETURN` 을 내고 값이 참조와 **정확히** 같아야 합니다
- 정의되지 않은 입력: interpreter 가 값이 아니라 `UNDEFINED_BEHAVIOR` 를 내야 합니다

`ExercisesBothSidesOfEveryPartialOperation` 이 부분 연산 사례마다 UB 입력에 실제로 닿았는지를 셉니다. **닿지 않으면 조용히 통과하지 않고 실패합니다.** 그렇게 하지 않으면 guard 를 하나도 검사하지 않고 통과하는 시험이 됩니다.

입력은 경계값 24개의 교차곱(사례당 576쌍)과 splitmix64 결정적 난수 2,000쌍입니다.

#### 이 과정에서 확인한 것

- 부호 있는 곱셈의 128비트 중간 경로가 맞습니다 (`3037000500^2` 가 UB 로 잡힙니다)
- `b != 0 && a / b > 1` 이 `b == 0` 에서 UB 가 아니라 0 을 냅니다. `SELECT` 전파 규칙이 없으면 여기서 거짓 UB 가 났을 것입니다
- 나눗셈, 나머지의 0 방향 절단과 나머지 부호가 clang 과 일치합니다

### 3. 퍼저

커밋: `948e7c3`

`tests/test_fuzz.cpp` 하나가 두 역할을 합니다. `QL_FUZZ_LIBFUZZER` 를 정의하고 컴파일하면 `LLVMFuzzerTestOneInput` 만 남고, 정의하지 않으면 같은 타깃을 결정적으로 도는 GoogleTest 캠페인이 됩니다. 그래서 지금 당장 매 `ctest` 마다 돌면서, 나중에 libFuzzer 실행 파일을 붙일 때 타깃을 다시 쓸 필요가 없습니다.

크래시 0 은 이 시험들이 가장 약하게 검사하는 성질입니다. 같이 거는 불변식이 본론입니다.

- 디코더가 받아들인 모듈은 verifier 를 **크래시 없이** 통과하거나 거부당해야 합니다
- **로어링이 `SUPPORTED` 라고 하면 그 IR 은 반드시 검증을 통과해야 합니다.** 변이된 소스도 소스이므로 G8 의 "verifier 가 로어링 출력 전부에 대해 통과한다"가 여기에 그대로 걸립니다
- 검증을 통과한 모듈은 interpreter 에서 크래시 없이 돌아야 합니다

크래시만 보는 퍼저는 로어링이 정당화할 수 없는 IR 을 내는 동안에도 초록으로 남습니다.

#### 캠페인이 헛돌지 않는지 세어 둔다

도달 횟수를 세서 로그에 찍고, 0 이면 시험이 실패합니다. 현재 수치입니다.

| 캠페인 | 반복 | 도달 |
|---|---:|---|
| 파서 (강한 변이) | 30,000 | 파싱 성공 2,708 |
| 로어링 (약한 변이 3/4) | 30,000 | `SUPPORTED` 417, 전부 검증 통과 |
| IR 디코더 (바이트 변이) | 60,000 | 디코드 1,038, 검증 통과 934 |
| 임의 바이트 | 30,000 | 파싱 성공 70 |

처음에는 로어링 도달이 4,000회 중 40회였습니다. 강한 변이가 파서 오류 복구에는 닿지만 로어링까지 살아남는 소스를 거의 남기지 않아서입니다. 약한 변이 모드를 넣어 417 로 올렸습니다. 두 모드는 서로 다른 타깃을 노립니다.

바이트 변이는 단일 비트 뒤집기 외에 **32비트 필드 통째 교체**를 넣었습니다. 개수와 오프셋이 little-endian 32비트라 비트 하나 뒤집기로는 표 범위 경계에 잘 닿지 않습니다.

#### 남은 것

`CMakeLists.txt` 와 `CMakePresets.json` 이 조율자 소유라 **libFuzzer 실행 파일과 Linux sanitize 프리셋은 아직 없습니다.** 아래 "조율자에게 요청할 것" 에 적었습니다.

### 4. 타입 폭: void, typedef, cast

커밋: `4880e40`

조율자의 기준선이 `unsupported_type` 53.86% 와 `unsupported_pointer` 43.91% 를 지목했고, val 1,050 본문에서 `unsupported_type` 694건의 서명을 직접 뜯어보니 두 가지가 지배적이었습니다.

- 366건이 `void` 반환입니다. 로어링이 `void` 를 타입 어휘에 아예 갖고 있지 않았습니다
- 286건이 포인터도 구조체 태그도 없이 **typedef 이름만으로** 막혔습니다. 코퍼스는 익명화된 디컴파일러 C 라 `typedef int TYP_0;` 같은 사슬이 도처에 있습니다

#### `src/c_types.c` 와 `src/c_types.h`

스칼라 C 타입 모델을 로어링에서 떼어냈습니다. 철자 어휘, C11 6.3.1.1 정수 promotion, 6.3.1.8 usual arithmetic conversion, 표현 가능성 판정이 여기 있고 IR 을 전혀 모릅니다. 2,900줄짜리 `c_lower.c` 안에서 트리 순회와 뒤섞여 있던 규칙들을 따로 읽을 수 있게 하는 것이 목적입니다.

폭은 **타겟 ABI(x86_64 Linux LP64)** 기준이라 `long` 은 64비트입니다. 호스트가 아닙니다.

헤더가 `src/` 에 있는 비공개 헤더라 시험이 직접 링크하지 못합니다. 그래서 `tests/test_c_lower_types.cpp` 가 로어링의 관찰 가능한 동작으로 검사합니다. 어차피 계약은 "로어링이 무엇을 받고 무엇을 거부하는가" 쪽입니다.

#### void

반환 타입에서만 `void` 를 받습니다(C 가 불완전 타입을 허용하는 유일한 자리). 매개변수나 지역 변수의 `void` 는 타입 오류입니다. `return;` 과 본문 끝에서 떨어지는 암묵적 반환이 같은 terminator 를 씁니다.

#### typedef 해석

`type_definition` 노드를 훑어 이름 표를 만들고, 철자가 어휘에 없으면 표를 따라 해석합니다. 세 가지를 지켰습니다.

- **이 unit 이 실제로 선언한 이름만 해석합니다.** `scalar_t__` 처럼 선언 없이 쓰인 이름에 뜻을 붙이면 그것은 추측이고, 타입에 대한 틀린 추측은 함수에 대한 틀린 답입니다
- typedef 가 포인터/배열/함수를 가리키면 **`unsupported_pointer`** 로 보고합니다. `unsupported_type` 으로 뭉뚱그리면 커버리지 표가 진짜 장애물을 가립니다
- 사슬 길이를 64로 묶습니다. `typedef A B; typedef B A;` 는 tree-sitter 가 받아들이는 번역 단위이므로 순환하면 안 됩니다

#### cast expression 과 tree-sitter 의 모호성

`(T)(e)` 는 T 가 타입 이름이면 cast 이고 호출 가능한 것이면 call 인데, 문법만으로는 구별되지 않습니다. tree-sitter 는 call 쪽으로 해소합니다. 그래서 **callee 가 괄호에 싸인 식별자이고 그 이름을 이 unit 이 typedef 로 선언했으며 같은 이름의 객체가 가리지 않을 때** 로어링이 cast 로 되돌립니다. 이것을 안 하면 코퍼스의 cast 들이 `unsupported_call` 칸에 쌓여서 표가 거짓말을 합니다.

가시적인 객체가 같은 이름을 가리면 진짜 call 입니다. C 의 유효 범위 규칙 그대로이고 시험이 고정합니다.

#### 결과 (val 1,050 본문)

| 첫 차단 사유 | 이전 | 이후 |
|---|---:|---:|
| (로어링 성공) | 0 | 2 |
| `unsupported_type` | 694 | **103** |
| `unsupported_pointer` | 334 | **832** |
| `unsupported_call` | 4 | 55 |
| `undeclared_identifier` | 5 | 31 |
| `unsupported_control_flow` | 0 | 8 |
| `unsupported_expression` | 4 | 8 |
| `unsupported_loop` | 0 | 2 |
| `frontend_unsupported` | 9 | 9 |

`unsupported_type` 이 85% 줄었고 **첫 차단 사유의 79% 가 포인터**로 드러났습니다. 이것이 조율자가 말한 "둘을 닫고 다시 재라" 의 결과입니다. 타입은 닫혔고 남은 103건은 struct/union/enum 과 미선언 이름입니다.

`undeclared_identifier` 31건은 `GLB_0` 같은 전역 변수입니다. 정적 저장 기간이 다음 순위 후보입니다.

#### 정확성을 같이 끌고 갔다

수용이 늘 때마다 differential 사례도 같이 늘렸습니다. `cast_narrow`, `cast_unsigned`, `cast_byte`, `named`, `named_signed` 5개가 추가되어 25개 함수가 실제 컴파일 실행과 대조됩니다. `ctest` 167/167 통과입니다.

### 5. 인터프리터 메모리 모델과 퍼저 이전

커밋: `cd629a2`

#### 퍼저를 `tests/fuzz/` 로

조율자가 `tests/fuzz/fuzz_*.c` 자동 타깃 규칙과 `linux-sanitize`, `linux-fuzz` 프리셋을 넣어 주었습니다. 드라이버 셋(`fuzz_c_frontend.c`, `fuzz_c_lower.c`, `fuzz_ir_decoder.c`)을 만들되 **타깃 본체는 `tests/fuzz/fuzz_targets.h`** 에 두어 libFuzzer 실행 파일과 매 `ctest` 마다 도는 결정적 캠페인이 같은 코드를 씁니다. 둘이 갈라지면 무엇이 시험되었는지에 대해 서로 다른 말을 하게 됩니다.

층마다 드라이버를 나눈 것은 libFuzzer 가 타깃별 코퍼스를 갖게 하려는 것입니다. dispatch 바이트 하나를 다시 발견하게 만들 이유가 없습니다.

#### 메모리 모델

위 "메모리 모델 계약" 절이 전문입니다. 구현은 이렇습니다.

- object 표는 `ql_ir_interp_options_v1` 이 받습니다. 세 제약을 `ql_ir_interp_run` 이 검사하고 어기면 실행을 거부합니다
- 메모리 버전은 **write 사슬**입니다. object 초기 이미지 위에 (주소, 폭, 바이트) 기록이 쌓이고, load 는 바이트마다 가장 최근 기록을 찾습니다. 스냅샷 복사가 없고 오래된 버전이 그대로 읽힙니다
- `LOAD`, `STORE`, `PTR_ADD`, `PTR_TO_BV`, `BV_TO_PTR` 를 실행합니다
- `PTR_ADD` 의 offset 이 포인터 폭과 다르면 **`UNSUPPORTED`** 입니다. schema v1 은 offset 이 bit-vector 라고만 하므로, 여기서 확장 규칙을 지어내면 SMT 쪽과 의미가 갈라집니다

`tests/test_ir_interp_memory.cpp` 7개 시험이 초기 이미지 읽기, null 역참조, object 밖 접근과 걸치는 접근, 정렬, store 가시성과 오래된 버전 보존, 거부되는 배치, 모델 밖 구문을 고정합니다. guard 를 일부러 `UB_GUARD(true)` 로 약하게 두어서, null 역참조가 `GUARD_INSUFFICIENT` 로 잡히는 것을 시험이 보입니다.

`ctest` 224/224 통과입니다.

### 6. 포인터 로어링

커밋: `6995fa4`

`c_lower.c` 가 포인터를 냅니다. 파라미터 타입(`int *p`), 메모리 스레딩, `*p`, `p[i]`, `*(p + i)`, `*p = v`, `p[i] = v`, 포인터 산술과 비교, null 비교입니다. object 파라미터 배치는 위 절에 확정해 적었습니다.

#### guard 는 접근 *앞*에 선다

verifier 규칙을 하나 나눠야 했습니다. 기존 규칙은 "부분 연산과 그것을 관찰하는 지점 사이를 guard 가 가른다"였는데, 메모리 접근에는 그대로 쓸 수 없습니다. **역참조를 먼저 하고 나중에 검사할 수는 없기 때문입니다.**

그래서 부분 연산을 둘로 나눴습니다.

| 부류 | 정의성이 | guard 위치 |
|---|---|---|
| `SDIV` `UDIV` `SREM` `UREM` `SHL` `LSHR` `ASHR` | 피연산자에 대한 사후 사실 | 연산과 관찰 사이 |
| `LOAD` `STORE` | 주소에 대한 **사전 조건** | 접근 **앞** |

임의 구분이 아닙니다. 술어가 연산의 사전 조건이냐 결과에 대한 사실이냐의 차이이고, 각 경우에 의무가 실제로 해소되는 지점이 다릅니다. 메모리 접근은 앞의 guard 가 의무를 해소하므로 결과가 pending 을 물려주지 않습니다.

interpreter 는 여전히 `LOAD`/`STORE` 의 부분성을 독립으로 알고 있으므로, guard 가 약하면 구체 입력에서 잡힙니다.

#### 결과 (val 1,050 본문)

| 첫 차단 사유 | 타입 작업 후 | 포인터 작업 후 |
|---|---:|---:|
| (로어링 성공) | 2 | **5** |
| `unsupported_pointer` | 832 | **241** |
| `unsupported_type` | 103 | **638** |
| `unsupported_call` | 55 | 90 |
| `undeclared_identifier` | 31 | 40 |
| `unsupported_expression` | 8 | 11 |
| `unsupported_control_flow` | 8 | 10 |
| `unsupported_loop` | 2 | 4 |
| `frontend_unsupported` | 9 | 9 |

**포인터가 71% 줄었고 차단이 타입으로 되돌아갔습니다.** 되돌아간 것이 아니라 옮겨간 것입니다. `struct TYP_0 *ARG_0` 같은 서명이 이제 포인터 관문을 통과해서 **가리키는 대상의 타입**에서 걸립니다. 코퍼스의 지배적인 모양이 struct 포인터이므로 예상된 이동이고, 다음 순위는 struct/union/enum 입니다.

#### differential 이 포인터 함수를 잰다

`tests/test_c_lower_pointers.cpp` 의 `MatchesCompiledExecutionOnPointerFunctions` 가 6개 포인터 함수를 실제 컴파일 실행과 대조합니다. 사례마다 무작위 배열 내용 256회이고, **반환값뿐 아니라 최종 메모리 이미지도 비교**합니다. 메모리가 관찰 대상이므로 그것을 비교하지 않으면 store 가 맞는지 아무것도 말하지 않습니다.

IR 은 `QL_IR_INTERP_FIRST_OBJECT_ADDRESS` 의 상징적 object 를 보고 참조는 진짜 배열을 봅니다. 이 사례들 중 주소를 반환하는 것이 없으므로 비교를 건너는 것은 데이터뿐이라 건전합니다.

`ctest` 253/253 통과입니다.

#### 아직 아닌 것

- **`&x`**: 지역 변수의 주소를 잡으려면 이 slice 가 아직 만들지 않는 object 가 필요합니다. `unsupported_pointer` 로 남습니다
- 지역 포인터 변수 선언(`int *p = ...;`)은 declarator 경로가 아직 막습니다
- `p - q`(포인터 차이), 이중 포인터, typedef 가 가리키는 포인터
- `struct`/`union`/`enum` 과 `->`, `.`

### 7. 집합 타입: struct, union, enum

커밋: `0f98c10`

#### 비트필드는 제외가 맞습니다 (측정으로 확인)

지시가 빈도를 보고 판단하라 했으므로 쟀습니다. **train 29,893 본문과 val 1,050 본문 모두에서 비트필드 구문이 0건입니다.** 익명화기가 정규화해 없앤 것으로 보입니다. `UNKNOWN` 유지가 맞고, 이 단위에서 손대지 않았습니다.

같은 측정에서 struct 는 train 의 74%(22,213), union 은 0.9%(256), enum 은 2.7%(803) 였습니다. val 의 `unsupported_type` 638건 중 **87%가 struct 를 선언**하고 **73%가 본문에서 멤버 접근**을 씁니다. 그래서 이 단위의 무게는 전부 struct 입니다.

#### 들어간 것

- struct 와 union 정의 수집, **x86-64 SysV 레이아웃**(멤버 정렬로 offset 올림, 레코드 정렬은 최대 멤버 정렬, 전체 크기는 정렬로 올림). union 은 전 멤버 offset 0
- `p->f`, `(*p).f`, `p->a.b` 중첩, 괄호 designator
- 멤버로의 store
- enum 타입은 `int`, enumerator 는 이름 붙은 정수 상수. 계산식 enumerator 는 넣지 않아 식별자가 미해결로 남습니다(틀린 값보다 낫습니다)
- typedef 가 aggregate 를 가리키는 경우 해석

레코드에는 IR 타입이 없습니다. **레코드 포인터는 바이트 주소**(`pointer(bv8)`)이고 멤버 접근이 그 주소에 offset 을 더한 뒤 멤버 타입으로 재해석합니다. 평평한 주소 공간 모델과 그대로 맞물립니다.

#### 포인터에 provenance 를 붙였다

`p->next->head` 를 만들면서 드러난 것입니다. object 표는 **파라미터에서 유도**되므로, 메모리에서 읽은 포인터에는 guard 가 이름 붙일 object 가 없습니다. 그대로 두면 로어링이 성공하지만 **모든 입력에서 UB 인 IR** 이 나옵니다. 그것은 로어링된 것처럼 보이면서 아무것도 증명하지 못하므로 `UNKNOWN` 보다 나쁩니다.

그래서 `lower_value` 에 `has_object` 를 두고 파라미터에서 유래한 포인터에만 세웁니다. 산술과 멤버 주소는 물려받고, load 로 얻은 포인터는 갖지 못합니다. 그런 포인터를 역참조하면 `unsupported_pointer` 로 거부합니다.

#### 결과 (val 1,050 본문)

| 첫 차단 사유 | 포인터 단위 후 | 이번 단위 후 |
|---|---:|---:|
| (로어링 성공) | 5 | **11** |
| `unsupported_type` | 638 | **460** |
| `unsupported_pointer` | 241 | 300 |
| `unsupported_call` | 90 | 157 |
| `undeclared_identifier` | 40 | 61 |
| `unsupported_control_flow` | 10 | 22 |
| `unsupported_expression` | 11 | 19 |

status 실패는 **0** 입니다. 중간에 struct 를 값으로 받는 함수가 `ensure_ir_type` 까지 내려가 `INTERNAL_ERROR` 를 내는 버그가 38건 있었고, 계약대로 `UNKNOWN` 으로 고쳤습니다. 의미 한계는 언제나 status 실패가 아니라 `UNKNOWN` 입니다.

`unsupported_type` 460 은 이제 **값으로 오가는 struct**(`TYP_4 ARG_0`)와 struct 지역 변수입니다. 둘 다 aggregate object 가 있어야 열립니다.

#### differential 이 struct 함수를 잰다

`tests/test_c_lower_records.cpp` 가 레코드 함수를 실제 컴파일 실행과 대조합니다. 매크로 하나가 **참조가 쓰는 struct 와 로어링이 읽는 struct 를 같은 텍스트에서** 만들기 때문에 레이아웃 차이가 두 선언 뒤에 숨을 수 없습니다.

레이아웃이 이 단위의 전부이므로 사례를 그쪽으로 골랐습니다. `char/int/long long/short` 혼합(모든 padding 결정이 답에 드러납니다), 중첩 레코드, union 의 겹치는 바이트, 멤버 store 후 최종 이미지 비교입니다. 무작위 바이트로 채운 구조체 128회씩입니다.

참조 함수는 **unsigned 로 누산**합니다. 무작위 필드 값을 부호 있는 덧셈으로 더하면 오버플로가 나서 로어링이 정당하게 UB 를 보고하고, 그러면 레이아웃이 아니라 오버플로를 재게 됩니다.

#### 이 단위에서 하지 못한 것

지시에 있었으나 넣지 못했습니다.

- **`&x` 와 지역 포인터 선언**: 지역 object 가 필요합니다. struct 를 값으로 받는 것과 같은 부품이라 다음 단위에서 함께 여는 것이 맞습니다
- **포인터 차 `p - q`**, 이중 포인터
- 구조체 안 배열 멤버(`int FLD_0[4]`)

### 8. 넓어진 포인터 표면

커밋: `9367dd1`

지시가 묶어 준 다섯 가지 중 넷을 넣었습니다.

- **지역 포인터 선언** (`int *cursor = p + i;`). declarator 마다 타입을 따로 만들도록 고쳤습니다. 예전에는 선언 하나가 타입 하나였는데 `int *p, q;` 는 그렇지 않습니다
- **이중 포인터**. `lower_type` 에 `indirection` 을 두어 `T`, `T *`, `T **` 를 구별합니다
- **포인터 차** `p - q`. 바이트 차를 원소 크기로 나눕니다. 나누는 수가 0 이 아닌 상수라 guard 는 그 자리에서 만족되지만, verifier 가 요구하는 위치에는 여전히 서 있습니다
- **typedef-to-pointer**. typedef 기록이 별 개수를 세도록 바꿔서 `typedef int *T` 가 거부 사유가 아니라 포인터 타입이 됩니다
- **`&<designator>`**. `&p->f`, `&a[i]`, `&*p` 는 이미 주소가 있는 것이라 실어 나르기만 하면 됩니다

#### IR 타입 캐시를 하나로 합쳤다

폭별 슬롯(`bv_types[129]`, `pointer_types[129]`, ...)으로는 중첩 포인터를 표현할 수 없어서 캐시를 하나로 합쳤습니다. **키는 C 타입이 아니라 IR 모양**(kind, bit_width, element_type)입니다. IR bit-vector 는 signless 라 `char` 와 `unsigned char` 는 같은 IR 타입이어야 하고, load 는 포인터의 element type 이 결과 타입과 **같은 식별자**일 것을 요구하므로 같은 모양에 식별자가 둘이면 깨집니다. 처음에 C 타입으로 키를 잡았다가 바로 이 규칙 위반으로 잡혔습니다.

#### 지역 포인터도 provenance 를 물려받는다

`int *cursor = p + i; return *cursor;` 가 거부되는 것으로 드러났습니다. `has_object` 를 파라미터에만 두었는데, 파라미터에서 유래한 값을 담은 **지역 변수**도 물려받아야 합니다. 대입과 선언 초기화에서 물려받고, 분기 병합에서 PHI 가 생기면 **떨어뜨립니다**. 두 분기가 다른 값을 주면 어느 한쪽의 object 로 둘 다를 부를 수 없기 때문입니다.

#### 결과 (val 1,050 본문)

| 첫 차단 사유 | 집합 타입 단위 후 | 이번 단위 후 |
|---|---:|---:|
| (로어링 성공) | 11 | **14** |
| `unsupported_pointer` | 300 | **69** |
| `unsupported_type` | 460 | 577 |
| `unsupported_call` | 157 | 227 |
| `undeclared_identifier` | 61 | 87 |

status 실패 0 입니다. 중간에 이중 포인터가 `ensure_ir_type` 까지 내려가 `INTERNAL_ERROR` 를 내는 것을 두 번 잡았습니다.

**포인터는 첫 차단의 6.6% 로 내려왔습니다.** 남은 69 의 대부분이 아래 "하지 못한 것" 입니다.

#### 하지 못한 것: 지역 object

이 단위의 표제였는데 넣지 못했습니다. `&<식별자>` 는 지역 변수에 주소를 주는 일이고, 그러려면 **함수가 스스로 object 를 만들어야** 합니다. 지금은 지역 변수가 SSA 값이라 주소가 없습니다.

임시로 얼버무리지 않고 `unsupported_pointer` 로 정직하게 보고합니다("taking the address of a local needs an object this slice does not create"). 커버리지 표가 빠진 부품을 정확히 가리킵니다.

이 부품 하나가 열면 같이 열리는 것들입니다.

- `&x`
- struct 지역 변수, 값으로 오가는 struct (지금 `unsupported_type` 577 의 대부분)
- 배열 지역 변수

### 9. 지역 object

커밋: `5ad4707`

함수가 스스로 만드는 스택 object 를 넣었습니다. 이제 `&x`, 지역 포인터 경유 읽기와 쓰기, 분기를 건너는 지역 저장이 됩니다.

#### 파라미터는 첫 명령보다 앞서야 한다

설계를 한 번 바꿔야 했습니다. 처음에는 선언을 만나는 자리에서 object 를 만들려 했는데, `ql_ir_builder` 가 **모든 파라미터가 첫 상수와 명령보다 앞설 것**을 요구합니다(`parameters_are_sealed`). object 의 base 와 size 는 파라미터이므로 본문 도중에 만들 수 없습니다.

그래서 둘로 나눴습니다.

- **파라미터 생성**은 진입 블록 전에 전부 끝냅니다. 그러려면 주소를 취하는 이름들의 **타입을 미리 알아야** 해서, 본문에서 그 선언을 찾아 타입을 푸는 사전 조사를 합니다
- **가정과 주소 값**은 진입 블록 안에서 냅니다. 지역 변수는 선언될 때 이미 만들어져 있는 슬롯에 묶이기만 합니다

#### 슬롯 크기는 선언된 타입에서 온다

`short v` 의 슬롯은 2바이트입니다. 전부 기계어 워드로 올림하면 범위를 벗어난 접근이 합법으로 보이게 됩니다. 시험이 이것을 고정합니다. 2바이트 슬롯에 4를 주면 로어링이 건 크기 고정 가정과 어긋나서 `ASSUMPTION_VIOLATED` 가 나옵니다.

#### 초기화 없는 저장소는 거부한다

`int v; int *p = &v; return *p;` 는 거부합니다. 초기화 없는 저장소는 불확정 값을 담고, C 는 그것을 읽는 것을 허용하지 않습니다. 값을 지어내는 것보다 거부가 낫습니다.

#### 저장소에 사는 변수는 분기 병합에서 PHI 를 만들지 않는다

값이 메모리에 있으므로 메모리 PHI 가 이미 병합합니다. 변수 쪽에 또 PHI 를 만들면 그 변수가 갖고 있지도 않은 값을 병합하게 됩니다.

#### 결과 (val 1,050 본문)

| 첫 차단 사유 | 이전 | 이후 |
|---|---:|---:|
| `unsupported_type` | 577 | 556 |
| `unsupported_call` | 227 | **197** |
| `undeclared_identifier` | 87 | 130 |
| `unsupported_pointer` | 69 | **64** |
| `uninitialized_read` | 0 | 18 |

status 실패 0, `ctest` 295/295 입니다.

### 10. `scalar_t__` 는 추측이 아니라 코퍼스의 사실이었다

커밋: `35e0b76`

지난 단위에서 "프로파일이 크기를 정해야 한다" 고 올린 항목입니다. 조율자가 원문을 지목했고 직접 확인했습니다.

```
typedef long scalar_t__;  // Either arithmetic or pointer type.
```

`datasets/raw/AnghaBench` 의 **모든** 소스가 이 줄을 서두에 담고 있습니다. 4,000개 표본에서 정확히 이 철자가 3,998번 나오고 다른 철자는 없으며, 언급하면서 정의하지 않는 파일은 600개 중 0개였습니다.

즉 **정할 것이 없었습니다.** 실제 컴파일이 쓴 정의가 LP64 signed long, 8바이트입니다. 레코드 추출이 함수 주변의 타입과 콜리 문맥만 남기고 서두를 버리기 때문에 추출된 unit 에서만 사라져 있었을 뿐입니다.

`src/c_types.c` 에 **코퍼스 서두 typedef 표**를 두고 `parse_type_spelling` 이 마지막 단계로 찾습니다. 순서가 중요합니다.

1. 프로파일 내장 스칼라 어휘
2. `struct` / `union` / `enum`
3. **이 unit 이 선언한 typedef**
4. 코퍼스 서두 typedef 표 ← 새로 추가
5. 거부

unit 이 스스로 선언하면 그쪽이 이깁니다. 표는 추출이 지운 서두를 되살리는 fallback 이지 덮어쓰기가 아닙니다. 표에 없는 미해석 이름은 예전 그대로 거부합니다. 시험이 세 가지를 다 고정합니다.

프런트엔드는 손대지 않았습니다. 미해석 타입 이름으로 거부하는 곳이 프런트엔드에는 없고(수용률 99.81%), 막고 있던 것은 로어링의 레코드 레이아웃이었습니다.

#### 결과 (val 1,050 본문)

| 첫 차단 사유 | 이전 | 이후 |
|---|---:|---:|
| (로어링 성공) | 14 | **17** |
| `unsupported_type` | 556 | **282** |
| `unsupported_call` | 197 | 303 |
| `undeclared_identifier` | 130 | 228 |
| `unsupported_pointer` | 64 | 97 |

`scalar_t__` 로 막혀 있던 457건이 이렇게 흩어졌습니다. 274건이 타입 관문을 통과했고(호출 106, 전역 98, 포인터 33, 그 외), 183건은 여전히 타입에서 걸리지만 **이유가 바뀌었습니다.** 남은 282건의 구성은 값으로 오가는 aggregate 43.6%, 배열 declarator 35.5%, 부동소수점 1.4% 입니다.

status 실패 0, `ctest` 319/319 입니다.

### 11. W8 을 위한 재사용 API 두 개

커밋: `f1528c5`

둘 다 append-only 추가입니다. 기존 함수의 서명도 구조체도 건드리지 않았고, 기존 함수는 새 함수에 NULL 을 넘기는 한 줄이 되었습니다. **구현이 하나뿐이므로 빠른 경로가 자기만의 버그를 갖는 일이 없습니다.**

```c
ql_status ql_c_frontend_analyze_with_parser(
    const ql_allocator *allocator, ql_c_parser *parser,
    const char *source, size_t source_size,
    ql_c_frontend_unit **output, ql_error *error);

ql_status ql_c_lower_selected_function_with_tree(
    const ql_allocator *allocator, const char *source, size_t source_size,
    const ql_c_frontend_unit *unit, const ql_c_function_view *function,
    ql_c_syntax_tree *tree, ql_c_lower_result **output, ql_error *error);
```

계약입니다.

- `parser` 와 `tree` 는 **빌려 쓸 뿐** 파괴하지 않습니다. 호출 뒤에도 호출자의 것이고 계속 쓸 수 있습니다. 시험이 그것을 고정합니다
- 둘 다 NULL 을 주면 기존 함수와 **완전히 같습니다**
- `tree` 는 **바로 그 source 의 파스**여야 합니다. 직접 대조하지는 않지만, 로어링이 이미 선택된 함수의 range 와 body range 가 정확히 일치할 것을 요구하므로 다른 텍스트의 트리는 거기서 걸립니다. 틀린 프로그램을 내리는 대신 거부합니다. 시험이 이 경우를 고정합니다
- parser 는 호출 중에 다른 스레드에서 쓰면 안 됩니다

무엇을 아끼는가입니다. tree-sitter parser 생성은 짧은 함수 하나를 파싱하는 것보다 비쌉니다. 그리고 지금까지는 frontend 가 한 번, lowering 이 또 한 번, 같은 소스를 **두 번** 파싱했습니다. unit 하나에 함수가 여럿이면 그만큼 곱해집니다.

`ctest` 323/323 이고 val 판정 수는 그대로입니다(17, status 실패 0). 동작을 바꾸지 않는 변경이라는 것이 그것으로 확인됩니다.

### 12. 함수 호출

커밋: `6796143`

측정 1순위였습니다. 선언된 콜리 호출을 내립니다.

#### IR 이 담는 것

외부 콜리는 uninterpreted 입니다. 메모리를 쓸 수도 있고 그 자체가 관찰 대상이므로, CALL 이 **메모리 상태와 event trace 를 둘 다 받아서 둘 다 내놓습니다.**

```
%trace1, %mem1, %value = CALL "FUN_1" (%trace0, %mem0, %arg0, ...)
    effects = CALL | MEMORY | IO
```

void 콜리면 `%value` 가 없습니다. 분기 병합에서는 메모리와 마찬가지로 trace 도 PHI 로 합쳐지고, `return` 이 둘 다 실어 관찰 가능하게 만듭니다.

**계약에 따라 IR 을 다르게 내지 않았습니다.** trace 를 비교할지 말지는 계약의 몫이고, 이것은 `return` 이 이미 메모리를 싣되 그것을 비교할지는 계약이 정하는 것과 같은 층위입니다. `ORDERED_TRACE` 계약은 최종 trace 를 비교하고 `IGNORE` 계약은 그것을 무시한 채 반환값만 봅니다. IR 은 프로그램을 기술하고 계약은 무엇을 비교할지를 기술합니다.

#### 인자 순서

C 는 인자 평가 순서를 규정하지 않습니다. 이 slice 에서 부수 효과를 내는 부분식은 호출뿐이므로, 인자를 왼쪽에서 오른쪽으로 평가하되 **인자 안에 호출이 또 있으면**(그때만 순서가 관찰 가능해집니다) 지금은 그것도 그대로 왼쪽부터 내립니다. 순서가 관찰 가능한 경우를 정확히 다루는 것은 열어 둡니다.

#### 사전 조사가 위장 cast 를 세지 않게 하는 데 한 번 걸렸다

`__memory` 와 `__trace` 파라미터는 첫 명령보다 앞서야 하므로 본문에 호출이 있는지 미리 봅니다. 처음에는 `call_expression` 노드를 그냥 셌는데, tree-sitter 가 `(TYP_0)(a + b)` 를 call 로 해소하므로 **cast 뿐인 함수에도 메모리와 trace 파라미터가 붙었습니다.** 파라미터 목록이 달라져서 시험이 바로 잡아냈습니다. 콜리가 식별자이고 그 이름을 이 unit 이 선언했을 때만 세도록 좁혔습니다.

#### 인터프리터는 콜리를 지어내지 않는다

IR 이 콜리를 uninterpreted 로 두므로 인터프리터도 결과를 알 수 없습니다. 호출자가 **명세를 주지 않으면 `UNSUPPORTED`** 입니다. 결과를 지어내는 것은 답을 지어내는 것입니다.

명세는 콜백 하나(`ql_ir_interp_callees_v1`)입니다. 0 을 돌려주면 그 호출을 거부하고 실행이 `UNSUPPORTED` 가 됩니다. 0 이 아닌 값을 돌려주는 것은 그 콜리에 대해 **세 가지를 주장**하는 것이고, 인터프리터는 그것을 모델링할 뿐 검사할 수 없습니다. 메모리를 쓰지 않는다, trap 하지 않는다, 결과가 주어진 인자에만 의존한다. IR 자체는 보수적으로 메모리 쓰기를 허용한 채로 두고, 이 좁은 모델은 **호출자가 준 명세의 성질**이지 IR 의 성질이 아닙니다. 헤더에 그대로 적었습니다.

#### verifier

CALL 이 **받지 않은 관찰 상태를 내놓는 것**을 거부합니다. 없던 역사를 이어받은 것처럼 읽히는 상태를 만들어 내는 일이기 때문입니다. 메모리를 이어 넘기면서 MEMORY effect 를 선언하지 않는 것도 거부합니다.

#### differential 이 호출 순서까지 잰다

`tests/test_c_lower_calls.cpp` 가 호출하는 콜리는 참조가 부르는 **바로 그 C 함수**입니다. 양쪽이 같은 콜리를 받고 로어링만 다르다는 것이 비교를 의미 있게 만듭니다. 명세가 자기가 불린 순서를 기록하므로 **반환값뿐 아니라 관찰된 호출 순서**를 비교합니다. 분기가 실제로 실행한 호출만 일어나는지도 봅니다.

#### 결과 (val 1,050 본문)

| 첫 차단 사유 | 이전 | 이후 |
|---|---:|---:|
| (로어링 성공) | 17 | **75** |
| `unsupported_call` | 303 | **62** |
| `undeclared_identifier` | 228 | 347 |
| `unsupported_type` | 282 | 284 |
| `unsupported_pointer` | 97 | 122 |

**판정 대상이 4.4배가 되었습니다.** 차단은 이제 전역 변수(347)가 1순위입니다.

status 실패 0, `ctest` 327/327 입니다.

### 13. 전역 변수와 정적 저장 기간

커밋: `602198e`

측정 1순위였습니다(val 첫 차단 347). 전역과 정적 저장 기간을 flat memory model 의 object 로 내립니다.

#### 전역은 새로운 종류가 아니라 같은 object 다

전역에 별도의 장치를 만들지 않았습니다. 포인터 파라미터의 영역이 받는 규율을 **한 글자도 다르지 않게** 받습니다. `<이름>.__base` 와 `<이름>.__size` 파라미터, 모델의 세 표준 제약, 크기를 못 박는 `ASSUME`, 그리고 접근마다 서는 guard 입니다.

이것이 이번 단위의 요구 하나를 **저절로** 만족시킵니다. 읽기는 load 이고 쓰기는 store 이므로 전역 쓰기와 호출이 **둘 다 메모리 상태를 이어받아 내놓습니다.** 어느 쪽도 다른 쪽을 지나쳐 떠다닐 수 없으므로 순서가 IR 에 그대로 남습니다. 순서를 지키는 장치를 따로 만들 필요가 없었고, 만들었다면 그것이 새로운 신뢰 대상이 되었을 것입니다.

`externally reachable final memory` 관찰도 같은 이유로 저절로 됩니다. 전역이 object 표에 들어 있으므로 계약이 최종 메모리를 볼 때 전역 상태가 그 안에 있습니다.

#### 초기값은 선언이 말한 것만 싣는다

**초기화자가 없는 전역을 0 으로 가정하지 않았습니다.** ISO C 에서 file scope 의 초기화자 없는 정의는 tentative definition 이고 0 으로 초기화되지만, 이 코퍼스의 `int GLB_0;` 은 그런 정의가 아닙니다. 원문 AnghaBench 의 `/* Variables and functions */` 절을 보면 추출기가 **다른 translation unit 에 있는 정의를 대신해 합성한 자리표시자**입니다. 실제 프로그램의 그 전역은 어떤 값이든 가질 수 있었습니다. 0 을 가정하는 것은 프로그램이 약속한 적 없는 값을 가정하는 것입니다.

그래서 초기화자가 없으면 **초기 내용을 미지로 둡니다.** caller 가 주는(혹은 solver 가 고르는) 초기 이미지가 그대로 시작 상태입니다. 이것은 포인터 파라미터가 가리키는 영역을 다루는 방식과 정확히 같고, 동치 증명에서는 양쪽이 같은 기호적 초기 메모리를 받으므로 옳습니다.

초기화자가 **있으면** 그 값을 entry 블록에서 store 로 씁니다. 그러면 본문이 시작하는 메모리는 caller 가 무엇을 주었든 선언이 약속한 것이 됩니다. `tests/test_c_lower_globals.cpp` 가 object 에 7 이 아닌 바이트를 주고 7 이 나오는지로 이것을 잽니다.

함수 안의 `static` 은 아직 내리지 않습니다. 초기화자가 호출 전체에 걸쳐 **한 번만** 실행되므로 entry 에서 store 하는 것이 두 번째 호출에 대해 틀립니다. 열어 둡니다.

#### 두 개의 정합성 구멍을 먼저 막았다

전역을 object 로 만들면서 곧바로 문제가 되는 두 가지가 있었고, 시험으로 못 박았습니다.

1. **한 이름을 두 번 선언하면 object 도 두 개가 됩니다.** `extern int x;` 뒤에 `int x = 3;` 은 하나의 저장소인데, 항목이 둘이면 모델이 그 둘을 disjoint 로 가정하므로 **한쪽으로 쓴 값이 다른 쪽 읽기에 보이지 않습니다.** 이름으로 합치고, 값을 말하는 선언(정의)이 이깁니다.
2. **같은 이름의 파라미터가 전역을 가립니다.** 변수 표를 뒤에서부터 찾는데 전역이 파라미터보다 나중에 들어가면 오히려 전역이 파라미터를 가립니다. 본문이 도달하지 못하는 전역은 object 를 아예 만들지 않습니다.

본문이 이름을 부르는 전역만 object 를 얻습니다. unit 이 선언한 것을 전부 만들면 쓰지도 않는 저장소에 파라미터와 제곱개의 disjointness 가정이 붙습니다.

#### 선행 결함 둘이 드러났다

전역이 앞을 막지 않게 되자 그 뒤에 있던 것이 보였습니다. 둘 다 **의미론적 한계를 status 실패로 냈고**, 이것은 계약 위반입니다. 한계는 언제나 `UNKNOWN` 이고 status 는 API 나 할당 결함을 위한 것입니다.

- `lower_designator_load` 가 **이미 IR 포인터인 load 결과에 `BV_TO_PTR` 을 한 번 더 씌웠습니다.** 레코드 멤버는 바이트 offset 으로 주소를 잡으므로 포인터 멤버를 읽으면 주소가 나와 변환이 필요하지만, 포인터의 포인터를 통한 load 는 이미 포인터입니다. 뒤엣것에 변환을 걸면 포인터를 주소인 양 읽는 것이고 IR 이 그대로 거부합니다. loaded 값이 이미 포인터인지로 갈랐습니다.
- **두 단계 포인터의 주소를 잡으면 세 단계가 필요합니다.** 이 slice 는 두 단계까지입니다. 슬롯을 만들 때 거부합니다.

train 29,893 본문의 status 실패가 4 에서 **0** 이 되었습니다.

#### differential 이 순서와 최종 상태를 잰다

`tests/test_c_lower_globals.cpp` 는 참조가 쓰는 **그 전역** 을 읽고 씁니다. object 의 초기 이미지를 참조의 시작값과 같게 주고, 최종 이미지를 참조 실행 뒤의 전역과 비교합니다. 순서는 콜리가 본 인자로 잽니다. `GLB = x; GLB = f(GLB); return f(GLB);` 에서 콜리가 본 인자는 `[x, 2x]` 여야 하고, store 를 어느 호출 너머로든 옮기면 이 값이 달라집니다. verifier 는 이 시험이 만드는 모든 로어링에 대해 돕니다.

#### 결과

| | 이전 | 이후 |
|---|---:|---:|
| val 로어링 성공 (1,050) | 75 | **173** |
| val `undeclared_identifier` | 347 | **55** |
| train 로어링 성공 (29,893) | 53 (기준선) | **3,925** |
| train status 실패 | 4 | **0** |

train 처리량은 14.4초(본문당 0.48ms)로 기준선과 같습니다.

val 첫 차단은 이제 `unsupported_type` 351, `unsupported_pointer` 160, `unsupported_expression` 98, `unsupported_call` 77 입니다.

`ctest` 396/396 입니다.

### 14. 배열과 지역 집합체, 그리고 W8 의 트리 접근자

커밋: `b25985d`

#### 먼저 쟀더니 과제가 달랐다

지시는 "값으로 전달/반환되는 struct 와 union, 그리고 배열"이었습니다. 재 보니 코퍼스에서 값 전달은 사실상 없습니다.

| 형태 | val 1,050 | train 29,893 |
|---|---:|---:|
| 지역 배열 선언 | 48 | 3,333 |
| 지역 집합체 선언 | 62 | 1,482 |
| 배열 파라미터 | 20 | 411 |
| **값으로 받는 집합체** | **0** | **62** |
| **값으로 반환하는 집합체** | **0** | **29** |

그래서 **지역 배열과 지역 집합체와 배열 파라미터**를 닫고, 값 전달 파라미터와 반환은 근거를 적어 `UNKNOWN` 으로 남겼습니다. 0.21 퍼센트와 0.10 퍼센트짜리에 SysV 분류 규칙을 흉내내는 것은 이 프로젝트가 ABI 시뮬레이터가 아니라는 지시와도 어긋납니다. 비트필드와 부동소수점을 근거로 미뤘던 것과 같은 판단입니다.

#### 배열도 집합체도 값이 아니라 저장소다

둘 다 flat memory model 의 object 입니다. 배열은 이름이 나오는 자리마다 첫 원소를 가리키는 포인터로 decay 하고, 집합체는 멤버를 통해서만 닿습니다. **어느 쪽도 통째로 load 되거나 store 되지 않으므로 IR 타입 자체가 없습니다.** `lower_type` 에 `array_length` 를 넣고, IR 타입을 요구하는 자리에는 애초에 도달하지 않게 decay 지점을 정리했습니다.

그 결과 기존 장치가 그대로 일합니다. `a[i]` 는 포인터 산술이고, `v.f` 는 이미 있던 designator 주소 계산이며, 크기는 못 박히고, 접근마다 guard 가 섭니다. `&a` 와 `a` 가 같은 주소를 주는 것도 저절로 됩니다.

주소를 적어 두지 않아도 배열과 집합체 지역은 object 를 받습니다. 값으로 살 수 있는 대안이 없기 때문입니다.

#### 초기화자 없는 집합체를 어떻게 볼 것인가 (판단이 필요한 부분)

초기화자 없는 배열/집합체 지역의 초기 바이트를 **미지로 두고 caller 가 주는 이미지를 그대로 씁니다.** ISO C 는 그 바이트를 indeterminate 라 하고 두 실행이 서로 다른 값을 볼 수 있다고 합니다. 이 프로파일은 그러지 않고 양쪽에 같은 이미지를 주고 그것을 전칭합니다.

이것은 이 모델의 다른 모든 object 에 이미 적용한 결정과 같은 것이고, asm2c 의 상황(한 함수의 두 번역이 한 기계 상태 위에서 돈다)에서는 옳습니다. 다만 **C 에 대한 사실이 아니라 가정**이므로 `ARCHITECTURE.md` 프로파일 절에 적었습니다. **조율자가 뒤집을 수 있게 여기에 표시합니다.** 주소를 잡은 **스칼라** 지역은 지금처럼 계속 거부하므로 이 확장은 통째로 읽지 않는 저장소에만 닿습니다.

#### 문자열 리터럴이 최대 차단이었고, 그것도 배열이었다

전역/지역을 열고 다시 재니 첫 차단이 `string_literal` 248 건이었습니다. 코퍼스의 형태는 파일 스코프의 `const char STR_0[] = "...";` 이고, 이것은 **초기화자가 길이를 말하는 char 배열**입니다. 배열 작업의 일부이지 별개가 아니었습니다.

리터럴은 바이트를 프로그램이 말하므로 entry 블록에서 그대로 씁니다. 초기화자 있는 전역과 같은 규율입니다. 인코딩 접두사, 해독 못 하는 escape, 이 slice 가 쓰는 것보다 긴 리터럴은 거부합니다. 바이트가 틀린 object 는 없는 object 보다 나쁩니다.

#### 재는 김에 드러난 것들

같이 닫았습니다.

- `T a[]` 와 `T a[N]` **파라미터는 포인터** 입니다. C 가 그렇게 조정하고, 있는 bound 는 callee 가 기댈 근거가 못 됩니다
- **배열 멤버** (`struct { int head; int slots[4]; }`). 배열의 정렬은 원소 하나의 정렬이라는 것을 `type_alignment` 가 몰라서 뒤따르는 멤버가 과정렬되고 있었습니다. differential 이 `sizeof` 로 잡았습니다
- flexible array member 는 record 에 크기를 남기지 않으므로 거부합니다
- `struct`/`union` 지역 타입을 `parse_local_type` 이 통째로 거부하고 있었습니다. 이미 있던 `resolve_type_node` 로 넘깁니다

#### differential 이 재는 것

`tests/test_c_lower_aggregates.cpp` 는 두 가지를 비교합니다. 반환값은 **컴파일된 함수**와 비교합니다. 멤버 offset 과 원소 폭이 맞는지는 그것만이 말할 수 있고, 로어링 자신의 산술로 쓴 시험은 자기 자신과 일치할 뿐입니다. 그리고 시험이 대는 object 크기는 **컴파일러의 `sizeof`** 이고, 모듈이 못 박은 크기가 그것과 다르면 `ASSUME` 이 실행을 거부합니다. 크기를 다시 적는 것이 아니라 대조하는 것입니다.

범위 밖 첨자가 `GUARD_FAILED` 가 되는지도 봅니다. 크기가 틀렸다면 값이 돌아왔을 것입니다.

#### W8 의 트리 접근자

`ql_c_frontend_unit_borrow_tree` 를 append-only 로 더했습니다. unit 이 자기가 만들어진 파스를 들고 있다가 빌려줍니다. 파괴는 unit 의 몫이고, 소비자는 그것을 `ql_c_lower_selected_function_with_tree` 에 넘기면 됩니다. 유닛당 파스가 2회에서 1회가 됩니다. `analyze` 가 트리를 같이 돌려주는 변형 대신 이쪽을 고른 것은 수명이 하나로 묶여서 소비자가 틀리기 어렵기 때문입니다.

#### 결과

| | 이전 | 이후 |
|---|---:|---:|
| val 로어링 (1,050) | 173 | **209** |
| train 로어링 (29,880) | 3,925 | **5,187** |
| status 실패 | 0 | **0** |
| train 처리량 | 14.4초 | **50.4초** |

val 첫 차단은 이제 `unsupported_pointer` 183, `unsupported_type` 182, `unsupported_expression` 158, `unsupported_call` 116 입니다. 메시지로 보면 포인터/배열/함수 타입으로의 cast 133, 해석 못 한 타입 철자 116, 선언 없는 콜리 105 입니다.

`ctest` 442/442 (직렬) 입니다.

#### 처리량 회귀를 그대로 보고합니다

train 이 14.4초에서 **50.4초** (본문당 0.48ms 에서 1.69ms) 가 되었습니다. 원인은 하나입니다. 리터럴 바이트를 **한 바이트에 store 하나씩** 쓰고, 그 store 마다 모든 object 에 대한 disjunction guard 가 섭니다. 리터럴 길이 중앙값이 19 바이트, p90 이 48 바이트입니다.

측정으로 확인했습니다. 리터럴 기록을 끄면 train 이 **14.5초**로 돌아가고 로어링은 5,187 에서 4,319 로 떨어집니다. 즉 **+868 본문의 대가가 +36초** 이고, 회귀 전부가 이 한 가지입니다.

**옳은 해법은 object 가 초기 바이트를 IR 안에서 직접 말할 수 있게 하는 것**입니다. 지금은 프로그램이 이미 아는 상수를 명령 수백 개로 다시 말하고 있습니다. 그것은 IR schema 확장이라 interpreter 와 W5 의 miter 가 같이 움직여야 하므로 이 단위에서 하지 않았습니다. 다음 단위 후보 1순위로 올립니다.

### 15. object 의 초기 바이트를 IR 이 직접 말한다

커밋: `24a624b`

#### 회귀의 원인은 표현이었다

지난 단위에서 리터럴 바이트를 **한 바이트에 store 하나**로 썼습니다. 그 store 마다 주소 계산, 상수, 살아있는 모든 object 에 대한 disjunction guard 가 붙으므로 비용이 바이트 수와 object 수의 **곱**으로 자랐습니다. train 처리량이 14.4초에서 50.4초가 된 것이 전부 이것이었습니다.

더 중요한 것은 그것이 **거짓말**이었다는 점입니다. 그 store 들은 프로그램이 수행하는 쓰기가 아닙니다. 프로그램은 그 바이트가 거기 있다고 *말할* 뿐입니다.

#### `MEMORY_IMAGE`

opcode 하나를 더했습니다.

```
%holds = MEMORY_IMAGE (%mem, %ptr)   image = 바이트들,  effects = 없음
```

"%mem 에서 %ptr 부터 이 바이트들이 서 있는가"를 묻고 bool 을 냅니다. **묻기만 합니다.** 그것이 사실이라고 말하는 것은 `ASSUME(%holds)` 이고, 그래서 서술인지 질문인지를 opcode 가 정하지 않습니다. 결과가 bool 이라는 것 하나로 verifier, interpreter, miter 의 기존 ASSUME 경로가 **그대로** 재사용됩니다.

effect 를 선언하지 않습니다. 선언하면 접근이 되고, 접근에는 guard 가 필요한데 이것은 아무것도 접근하지 않습니다. verifier 가 그 점을 따로 거부합니다.

#### 바이트는 symbol 이 아니다

처음에는 바이트를 `symbol` 에 실으려 했는데 `symbol` 은 **텍스트 계약**이라 NUL 을 거부합니다. 문자열 리터럴의 이미지는 NUL 로 끝납니다. 그래서 명령에 `image`/`image_size` payload 를 append-only 로 더하고 artifact schema 를 v2 로 올렸습니다. 올린 덕분에 `tests/fuzz/fuzz_targets.h` 가 schema 를 `1u` 로 박아 두고 있던 것이 **즉시** 드러났습니다. 버전을 올리는 쪽이 안전한 방향인 이유가 그것입니다. 낡은 artifact 는 오독되지 않고 거부됩니다.

#### 네 곳이 같이 움직였다

- **ir.c**: 모양 검사(memory, pointer -> bool, image 비어 있지 않음, pure)
- **ir_verify.c**: 같은 모양에 더해 effect 를 선언하면 거부
- **ir_interp.c**: 바이트를 실제로 비교해 bool 을 냅니다. 살아있는 object 밖의 이미지는 존재하지 않는 저장소에 대한 주장이라 거짓입니다
- **product.c**: 바이트마다 `(= (select mem0 <addr+i>) (_ bv<N> 8))` 의 논리곱. 배열 이론의 그 구간 초기 내용이 됩니다

#### 커버리지를 판정으로 바꾸는 지점에서 막혀 있었다

SMT 시험을 end-to-end 로 쓰려다 **선행 결함**을 만났습니다. `src/signature.c` 의 IR-시그니처 바인딩이 "IR 파라미터 = C 인자 + (`__memory` + 포인터 인자마다 base/size)" 로 개수를 못 박고 있어서, 로어링이 스스로 만든 object(전역, 지역 배열/집합체, 리터럴)가 하나라도 있으면 miter 가 쌍을 만들지도 못했습니다. 지난 두 단위의 커버리지 증가분이 판정으로 전혀 전환되지 않고 있었다는 뜻입니다.

조율자 승인을 받아 이 변경에 한해 `signature.c` 도 열고 최소 일반화를 했습니다. **느슨해지지 않게** 한 것이 핵심입니다. 추가 (base,size) 쌍을 허용하되 여전히 하나하나가 pointer-width bv 인지, 이름이 `.__base`/`.__size` 인지, 쌍으로 오는지를 봅니다. 그 외의 파라미터는 계속 거부합니다. 인자 object 개수는 여전히 요구합니다. 포인터 인자의 object 를 빠뜨린 로어링이 자기 object 하나를 그 인자의 것이라 부르며 통과하면 안 되기 때문입니다. W2 가 이 표를 c_lower 와 독립으로 쓴 교차 검증 가치를 죽이지 않는 것이 조건이었습니다.

`product.c` 의 object 표도 파라미터 목록을 **읽어서** 만들도록 바꿨습니다. 예측은 모든 object 가 포인터 인자의 것일 때만 맞았습니다. 좌우 object 개수가 다르면 서로 다른 저장소를 이야기하는 것이므로 거부합니다.

#### 정적 데이터가 다른 쌍은 판정되지 않는다 (불완전하되 건전)

리터럴 바이트가 서로 다른 쌍은 miter 가 대응 object 에 **하나의 base 와 하나의 배열**을 주므로 두 이미지가 모순되고 도메인이 빕니다. 빈 도메인은 승격되지 않으므로 결과는 "판정 없음"이지 "동치 증명"이 아닙니다. 시험으로 그 사실을 고정했습니다. 결정 가능하게 하려면 로어링이 만든 object 에 **좌우 각자의 정체성**을 줘야 하고, 그것은 이미지가 무엇을 말하느냐가 아니라 miter 가 저장소를 무엇이라 보느냐의 변경입니다.

#### 결과

| | 이전 | 이후 |
|---|---:|---:|
| train 처리량 | 50.4초 | **16.1초** |
| train 로어링 (29,880) | 5,187 | **5,187** |
| val 로어링 (1,050) | 209 | **209** |
| status 실패 | 0 | **0** |
| `ctest` (직렬) | 442 | **471** |

**로어링 결과는 한 건도 바뀌지 않았고 처리량만 돌아왔습니다.** differential 이 그것을 봅니다.

#### val 판정률 (이 단위의 진짜 성과)

로어링된 209 개를 자기 자신과 짝지어 재면 **42 개가 판정에 도달합니다(20.1%)**. 이 변경 전에는 전역이나 집합체를 가진 본문이 쌍조차 만들어지지 않았습니다.

남은 167 개의 차단은 이제 정직하게 한 곳을 가리킵니다.

| 막는 것 | 건수 |
|---|---:|
| **event trace: miter 가 호출을 모델링하지 않음** | **131** |
| `scalar_t__` 가 frozen signature 표에 없음 | 16 |
| 배열/함수 타입이 source-signature schema v1 밖 | 3 |
| 기타 | 17 |

처음에는 이것들이 "object 표를 넘어선 파라미터"로 잘못 보고되고 있었습니다. 호출하는 본문은 `__memory` 옆에 `__trace` 를 끼우는데 miter 가 그것을 object 로 세고 있었기 때문입니다. 이제 무엇이 없는지를 그대로 말합니다.

**다음 관문은 커버리지가 아니라 miter 의 호출 모델링입니다.** 로어링은 호출을 이미 정확히 싣고 있고(단위 12), 판정으로 가지 못하는 것은 miter 쪽입니다.

### 16. miter 가 호출을 모델링한다

커밋: `2e0cd8f`

#### 병목이 커버리지가 아니었다

지난 단위의 측정이 가리킨 곳입니다. 로어링된 209 개 중 판정에 도달한 것이 42 개였고, 못 간 167 중 **131 이 event trace**, 즉 miter 가 호출을 uninterpreted 로 두기만 하고 좌우를 비교하지 못하는 것이었습니다. 프런트엔드는 이미 호출을 정확히 싣고 있었고 막힌 곳은 proof semantics 였습니다.

#### 콜리는 함수다. 그게 전부다

무엇을 계산하는지는 한 마디도 말하지 않습니다. 말하는 것은 하나입니다.

```
같은 콜리 + 같은 history + 같은 memory + 같은 인자
    -> 같은 history + 같은 memory + 같은 반환값
```

호출 결과는 자유 상수로 **선언**하고(정의하지 않습니다), 이 함의를 호출 지점 쌍마다 assumption 으로 진술합니다. 좌우 쌍만이 아니라 **한쪽 안의 쌍**도 넣습니다. 같은 함수를 같은 인자로 두 번 부르면 두 번 다 같은 답이어야 하고, 그렇지 않으면 그 사이를 통과해 좌우를 비교할 수 없습니다.

이것이 전부라는 점이 중요합니다. 콜리가 다르거나 인자가 다르면 **아무 등식도 생기지 않고**, 두 쪽은 자유롭게 달라지며, miter 는 틀린 판정 대신 판정 없음을 냅니다. 시험이 그 두 경우를 각각 못 박습니다.

uninterpreted function 을 SMT 에 선언하지 않았습니다. 필요한 것은 합동성뿐이고, 합동성은 필요한 자리에 함의로 적으면 됩니다. `declare-const` 와 assumption 만으로 되므로 logic 을 UF 로 올릴 필요도, 자유 sort 를 만들 필요도 없습니다.

#### history 는 비트벡터다

event trace 에는 고유한 폭이 없습니다. 이 인코딩이 trace 에 요구하는 것은 **동등성**뿐이고, 두 trace 를 같다고 증명해 주는 것은 폭이 아니라 합동성입니다. 그래서 고정 폭 비트벡터로 나릅니다.

방향이 안전합니다. 서로 다른 실행의 trace 를 solver 가 우연히 같게 골라도, 그것은 이 인코딩이 **찾지 못하는 반례**이지 **받아들이는 증명**이 아닙니다.

#### 계약이 순서를 보는지가 판정을 가른다

`QL_OBSERVE_EXTERNAL_CALLS` 가 켜지고 `ORDERED_TRACE` 면 두 실행의 최종 history 를 비교합니다. 꺼져 있으면 값만 봅니다. 같은 두 콜리를 같은 인자로 **순서만 바꿔** 부르고 같은 값을 돌려주는 쌍이 순서 관찰 off 에서는 PROVED 이고 on 에서는 아닙니다. 두 실행의 차이는 계약뿐입니다. 순서 관찰이 그냥 전부를 거부하는 것이 아니라는 것도 같이 봅니다(같은 순서의 두 철자는 on 에서도 PROVED).

#### miter 가 로어링 결함을 잡았다

`CALLEE_g(CALLEE_f(b))` 가 상쇄되지 않았습니다. 쿼리를 열어 보니 바깥 호출이 **인자를 내리기 전의** trace 와 memory 를 받고 있었습니다. `lower_call_expression` 이 관찰 상태를 인자보다 먼저 읽었기 때문입니다. 인자 안에 호출이 있으면 바깥 호출이 안쪽 호출의 효과를 못 본 IR 이 됩니다.

반환값만 보면 드러나지 않는 결함이라 IR 위에서 직접 못 박는 회귀 시험을 넣었습니다. **증명 층이 로어링을 검사한 것**이고, 이것이 이 층들을 따로 두는 이유입니다.

#### 호출이 있는 SAT 은 반례로 승격하지 않는다

violation 이 SAT 이면 반례 후보이고, 확정하려면 replay 가 IR 을 구체 실행해야 합니다. 호출을 구체 실행하려면 콜리가 있어야 하는데 witness 는 콜리를 나르지 않습니다. `src/replay.c` 는 이번 위임 밖이기도 합니다.

그래서 호출이 있는 SAT 은 **replay 하지 않고 UNKNOWN 으로 둡니다.** replay 하지 않은 model 은 반례가 아니라는 상시 규칙 그대로입니다. 불완전하되 건전하고, 이유를 진단문에 적습니다.

#### 결과

| | 이전 | 이후 |
|---|---:|---:|
| **val 판정률** | 42/209 (20.1%) | **188/209 (89.95%)** |
| proved-equivalent | 42 | **188** |
| event trace 로 막힌 것 | 131 | **0** |
| `ctest` (직렬) | 471 | **497** |

로어링 수치는 209 그대로이고 status 실패 0 입니다. **131 건이 전부 판정으로 왔습니다.**

남은 21 건입니다.

| 막는 것 | 건수 |
|---|---:|
| `scalar_t__` 가 frozen signature 표에 없음 | 16 |
| 배열/함수 타입이 source-signature schema v1 밖 | 3 |
| IR 파라미터 타입이 signature 와 불일치 | 2 |

`scalar_t__` 는 이미 c_lower 쪽에서 코퍼스의 물리적 사실로 닫아 둔 것(`ql_c_scalar_from_corpus_typedef`, 원문 4,000 파일 중 3,998)과 같은 이름입니다. signature 표에도 같은 사실이 필요하며, **표를 복제하지 말고 같은 표를 쓰는 것**이 맞습니다. 사실을 두 벌 두면 어긋납니다. W2 표를 c_lower 와 독립으로 둔 가치는 모델링 선택의 교차 검증이지 코퍼스 사실의 재입력이 아닙니다.

### 17. signature 표의 `scalar_t__`, 그리고 포인터로의 cast

커밋: `382d85a`

#### 사실은 한 벌만 둔다

signature 의 타입 표가 `scalar_t__` 를 몰라 val 판정 16 건이 막혀 있었습니다. 이 이름은 unit 이 선언할 수 없습니다. record 추출이 그것을 선언하는 preamble 을 버리기 때문입니다.

**표를 복제하지 않고 로어링이 읽는 같은 표를 읽게 했습니다.** `scalar_t__` 는 모델링 선택이 아니라 코퍼스에 대한 사실이고, 사실을 두 벌 두면 어긋납니다. signature 의 타입 모델을 로어링과 따로 쓴 가치는 **모델링을 두 번 검사**하는 데 있지 코퍼스 사실을 두 번 적는 데 있지 않습니다. 그래서 그 표가 자기 것으로 계속 들고 있는 것은 위의 모든 철자이고, 공유하는 것은 preamble typedef 하나뿐입니다.

표에 없고 unit 도 선언하지 않은 이름은 여전히 거부합니다.

#### cast 는 재해석이지 계산이 아니다

이 프로파일에서 포인터는 곧 주소입니다. 그러므로 `(T *)e` 는 값을 하나도 바꾸지 않고 타입만 바꿉니다. `convert_across_pointer` 가 이미 정확히 그것을 하고 있었고, `lower_cast_expression` 이 abstract declarator 를 보자마자 거부해서 거기 닿지 못하고 있었을 뿐입니다.

별을 세어 `resolve_type_node` 에 넘기는 것으로 끝났습니다. **주소가 온 object 가 값과 함께 따라가므로** cast 뒤의 역참조가 cast 전보다 덜 알려지지 않습니다. 배열이나 함수 타입으로의 cast(값이 없습니다)와 세 단계 이상의 indirection 은 계속 거부합니다.

#### 정확성을 대가로 내지 않았다는 것을 같은 표본에서 보였다

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 | 5,187 | **6,129** |
| **verifier 통과** | 5,187 / 5,187 | **6,129 / 6,129** |
| differential (직렬 `ctest`) | 497 | **499** |
| status 실패 | 0 | **0** |
| train 처리량 | 12.5초 | **11.9초** |
| val 로어링 | 209 | **251** |
| **val 판정률** | 188/209 (90.0%) | **244/251 (97.2%)** |

cast 에 대한 differential 은 `void *` 를 거쳐 되돌아오는 경우와 `char *` 로 재해석해 바이트 단위로 읽는 경우입니다. 뒤엣것을 넣은 이유는 **cast 타입이 첨자의 보폭을 정한다**는 것을 컴파일된 실행만이 말해 줄 수 있기 때문입니다.

기준선 대비로는 IR 로어링 SUPPORTED 가 53 (0.18%) 에서 6,129 (20.51%) 입니다. `docs/coverage/coverage-20260811.md` 에 적었습니다.

#### 보고

직렬 `ctest` 에서 `quodlibet.python_bindings` 가 두 번 실패하고 세 번째와 단독 실행에서 통과했습니다. 부하 아래의 flaky 로 보입니다. W1 파일이 아니라 손대지 않았습니다.

### 18. preamble 의 나머지, 그리고 포인터를 반환하는 콜리

커밋: (이 단위)

#### 표는 한 이름이 아니라 preamble 전체였다

지난 단위가 `scalar_t__` 를 코퍼스의 사실로 옮겨 적었습니다. 이번에 미해석 타입 철자 3,998 건을 실제로 세어 보니 `size_t` 3,295, `uintptr_t` 43, `intptr_t` 7 이었습니다. 원문을 보면 이유가 분명합니다. AnghaBench 의 서두는 고정 블록입니다.

```
#define NULL ((void*)0)
typedef unsigned long size_t;  // Customize by platform.
typedef long intptr_t; typedef unsigned long uintptr_t;
typedef long scalar_t__;  // Either arithmetic or pointer type.
typedef int bool;
```

record 추출이 이 블록 **전체**를 버리는데 표에는 한 줄만 옮겨져 있었습니다. 2,000 파일 표본에서 `size_t` 를 언급하는 1,981 파일 전부가 저 한 철자로 선언하고 다른 철자는 없으며, `intptr_t` 와 `uintptr_t` 는 같은 1,981 파일의 같은 줄에서 옵니다. 전사이지 추측이 아닙니다.

표를 하나만 두었으므로 `src/signature.c` 도 자동으로 같이 알게 됩니다. 사실을 두 벌 두지 않는다는 지난 단위의 결정이 여기서 이득으로 돌아왔습니다.

#### 선언이 있는데 없다고 말하고 있었다

`unsupported_call` 4,820 중 **4,815** 가 "the callee has no declaration in this unit" 였는데, 추출된 unit 은 콜리를 전부 선언합니다. 원인은 수집기였습니다.

`T *f(...)` 의 파스는 `declaration -> pointer_declarator -> function_declarator` 입니다. `collect_callees` 가 declaration 바로 아래의 `function_declarator` 만 받고 있어서 **포인터를 반환하는 콜리는 하나도 수집되지 않았습니다.** 코퍼스에서 그것이 콜리의 대부분이었습니다.

별을 declarator 사슬에서 세어 `return_pointer_depth` 로 싣고, function_declarator 를 declarator_node 로 둡니다. `resolve_callee` 가 별을 세려고 부르던 `member_declarator_name` 은 function_declarator 를 만나면 언제나 거부하므로 항상 0 을 주고 있었습니다. 그 호출을 지웠습니다. 죽은 코드가 아니라 **틀린 답을 주고 있던 코드**였습니다.

#### `f(void)` 는 파라미터가 없다는 뜻이다

포인터 반환 콜리 시험을 쓰다 드러났습니다. `char *CALLEE_high(void);` 가 `type_error: void is not an object type here` 로 막혔습니다. `resolve_callee` 가 `void` 를 먼저 타입으로 풀려 하고, 파라미터 위치의 `void` 는 객체 타입이 아니므로 오류가 났습니다. 그 뒤에 있던 `parameter.kind == VOID` 분기는 도달할 수 없는 코드였습니다.

타입을 풀기 전에 철자로 알아봅니다. 별이 없는 `void` 파라미터는 파라미터가 아닙니다.

#### 정확성을 같은 표본에서 같이 보였다

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 6,129 (20.51%) | **7,233 (24.21%)** |
| val 로어링 (1,050) | 251 | **288** |
| val 판정률 | 244/251 (97.2%) | **279/288 (96.9%)** |
| status 실패 | 0 | **0** |
| `ctest` (직렬) | 503 | **507** |
| train 처리량 | 11.9초 | 12.9초 |

새 differential 은 **컴파일된 실행만이 말할 수 있는 것**을 고르려고 이렇게 잡았습니다.

- 콜리가 하위 32비트가 0 인 주소를 돌려주고 본문이 null 과 비교합니다. 반환 폭이 32비트였다면 주소가 null 로 잘려 반대 답이 나옵니다
- `size_t` 를 32비트 왼쪽 shift, `intptr_t` 부호 반전, `uintptr_t` 를 32비트 오른쪽 shift 합니다. 폭과 부호가 셋 다 답에 드러납니다

건전성 쪽도 같이 고정했습니다. 콜리가 돌려준 포인터에는 guard 가 이름 붙일 object 가 없으므로 역참조는 계속 거부합니다(`WillNotDereferenceAPointerACalleeReturned`). 선언을 찾게 되었다고 provenance 규칙이 느슨해지지 않았습니다.

#### 조율자에게: `bool` 은 이 코퍼스에서 `int` 입니다

같은 preamble 이 `typedef int bool;` 를 담고 있는데, 지금 세 곳(`src/c_types.c` 의 철자표, `src/c_frontend.c`, `src/signature.c`)이 `bool` 을 `_Bool` 로 봅니다. train 29,880 본문 중 **1,801 개가 bare `bool` 을 쓰고 스스로 선언하는 unit 은 0 개**입니다.

C17 에서 `bool` 은 키워드가 아니라 `<stdbool.h>` 의 매크로이므로, 프로파일의 철자 어휘에 그것이 있는 것 자체가 정확하지 않습니다. 폭이 1비트냐 32비트냐는 `bool` 멤버 뒤에 오는 멤버의 offset 을 바꾸므로 메모리 의미에 닿습니다.

좌우가 같이 움직이므로 지금 **틀린 판정을 내고 있지는 않습니다.** 커버리지 차단도 아닙니다. 그래서 이번 단위에 섞지 않았습니다. 다만 `signature.c` 를 다시 여는 변경이므로 조율자 판단이 필요합니다.

#### 다음 관문은 포인터 provenance 입니다

포인터가 2,548 에서 5,058 로 늘었는데 되돌아간 것이 아니라 옮겨간 것입니다. 그중 **4,308 + 624 = 4,932 가 "메모리에서 읽은 포인터에 object 가 없다"** 한 가지입니다. 단위 7에서 정직하게 거부하기로 한 그 규칙이고, 이제 train 최대 단일 차단입니다.

### 19. 식 어휘의 빈 자리

커밋: (이 단위)

`unsupported_expression` 의 catch-all 에 걸린 노드 종류를 세어 순서를 정했습니다. `sizeof_expression` 1,371, `conditional_expression` 667, `assignment_expression` 433, `update_expression` 403, `comma_expression` 53 이었고, 그 옆에 복합 대입이 첫 차단 1,021 로 따로 서 있었습니다. `sizeof` 를 빼고 나머지를 전부 닫았습니다.

#### 넷은 같은 구조다

복합 대입, 값으로 쓰는 대입, `++`/`--`, 그리고 (왼쪽이 대입일 때의) 쉼표는 전부 **한 객체를 읽고, 합치고, 쓰는** 같은 일입니다. 셋을 따로 쓰면 규칙이 어긋납니다. 그래서 하나로 합쳤습니다.

- `resolve_assignment_target` 이 대상을 **한 번** 확정합니다. `a[i()] += 1` 이 `i` 를 한 번만 부르는 것이 이것 때문입니다
- `write_assignment_target` 이 선언 타입으로 되변환하고 씁니다. 레지스터에 사는 지역과 저장소에 사는 지역이 여기서 갈립니다
- `lower_read_modify_write` 가 셋을 조립합니다. `operator_text` 가 NULL 이면 평범한 대입이고, `yields_old_value` 가 후위형입니다

`x op= v` 는 C 가 `x` 를 한 번만 평가하는 `x = x op v` 로 정의하므로, 합치는 자리는 **평범한 이항 연산 경로**를 그대로 부릅니다. 그래서 `<<=` 의 정의성 규칙이 `<<` 의 것과 같고, `p += n` 이 바이트가 아니라 원소만큼 움직입니다. 규칙을 다시 적지 않았습니다. 이를 위해 `lower_binary_expression` 에서 연산 적용부를 `apply_binary_operator` 로 떼어냈습니다.

#### 조건식의 정의성은 단락한다

`c ? a : b` 의 값은 `SELECT` 로 계산하되 정의성은 `cond_defined && (!cond || then_defined) && (cond || else_defined)` 입니다. 양쪽을 다 요구하면 **C 에 없는 undefined behaviour 를 만들어 냅니다.** `b != 0 ? a / b : a` 가 `b == 0` 에서 UB 가 되어 버립니다. `&&` 와 `||` 에서 이미 내린 것과 같은 결정이고, 시험이 그 한 가지를 따로 못 박습니다.

두 arm 이 포인터면 결과는 주소이고 **object 를 물려주지 않습니다.** 어느 한쪽의 object 로 둘 다를 부를 수 없기 때문입니다. 분기 병합에서 PHI 가 `has_object` 를 떨어뜨리는 것과 같은 이유입니다.

#### status 실패 하나를 도중에 잡았다

`void **` 를 역참조한 자리에 쓰면 대상의 선언 타입이 `void` 인데, 새 경로가 그 타입으로 값을 변환하려 해서 IR 이 "값을 가질 수 없는 타입"을 거부했습니다. 계약대로 `type_error` UNKNOWN 으로 고쳤습니다. 의미 한계는 언제나 status 실패가 아닙니다. 전 코퍼스 재측정이 이것을 잡았고, 이것이 매 이득마다 전 코퍼스를 다시 재는 이유입니다.

#### 결과

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 7,233 (24.21%) | **8,541 (28.58%)** |
| val 로어링 (1,050) | 288 | **349** |
| val 판정률 | 279/288 (96.9%) | **337/349 (96.6%)** |
| `unsupported_expression` | 4,622 | **2,154** |
| status 실패 | 0 | **0** |
| `ctest` (직렬) | 507 | **511** |

`tests/test_c_lower_expressions.cpp` 가 13개 함수를 컴파일된 실행과 대조하고, 정의된 입력에서는 값이 정확히 같은지, 정의되지 않은 입력에서는 값이 아니라 `UNDEFINED_BEHAVIOR` 인지를 둘 다 봅니다. **UB 쪽에 실제로 닿았는지를 세고 닿은 사례가 3개 미만이면 실패합니다.** 포인터 differential 에는 `p[i] += 7` 과 `p[i]++` 를 더했습니다. 최종 메모리 이미지가 주소를 한 번만 계산했는지를 말해 줍니다.

#### 다음

1. **메모리에서 읽은 포인터의 provenance** (pointer 5,614 의 대부분). object 표가 파라미터에서만 유도되지 않아야 하므로 W2 의 miter 와 같이 움직여야 합니다
2. **`sizeof`** (1,371). 피연산자를 평가하지 않고 타입만 알아내야 하므로 emit 하지 않는 정적 타입 질의가 필요합니다. 타입 이름이 4,986, `*VAR_0` 과 `ARG_0->FLD_n` 이 그 다음입니다
3. **loop** 3,020. 닫으면 **W6 의 CHC/PDR proof method 선행이 풀립니다**
4. `uninitialized_read` 2,350

### 20. 부호 있는 정수 리터럴은 부호와 크기를 따로 내린다

커밋: (이 단위)

G9 전 코퍼스의 첫 차단을 메시지까지 다시 세니 `integer_literal_out_of_range` 929건이 실제 64비트 초과가 아니라 `-1`, `-99` 같은 보통 음수였습니다. Tree-sitter C는 선행 부호를 `number_literal` 노드에 포함시키지만 `lower_integer_literal`은 첫 글자부터 숫자라고 가정하여 `-`를 잘못된 자릿수로 거부하고 있었습니다.

C의 의미 순서를 그대로 복구했습니다. 먼저 부호 없는 크기와 접미사로 리터럴 타입을 고르고, 그 다음 그 타입 위에서 단항 부호를 적용합니다. 이 순서가 아니면 `-2147483648`의 리터럴 부분이 64비트 타입을 골라야 하는 사실과 `-1u`가 unsigned wrap이라는 사실을 동시에 지킬 수 없습니다. 실제 컴파일된 C와 대조하는 differential에 signed, unary plus, unsigned negative 사례를 추가했습니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 8,755 (29.30%) | **9,232 (30.90%)** |
| `integer_literal_out_of_range` | 929 | **3** |
| verifier 통과 | 8,755 / 8,755 | **9,232 / 9,232** |
| status 실패 | 0 | **0** |

Windows에서는 native CTest 510/510과 Python binding 37/37을 각각 통과했습니다.

### 21. 주소를 쓰기 전에 직접 초기화한 지역은 저장소로 내린다

커밋: (이 단위)

주소가 사용된 스칼라 지역은 이전까지 선언 초기화자가 없다는 이유만으로 즉시 UNKNOWN이었습니다. 그래서 `int v; v = x; return *&v;`처럼 읽기 전에 값이 반드시 저장되는 본문도 받지 못했습니다.

이제 평범한 직접 대입이 stack slot에 store를 성공한 뒤에만 그 지역을 초기화 완료로 표시합니다. 주소를 그 전에 꺼내는 경우와 복합 대입처럼 기존 값을 먼저 읽는 경우는 계속 `uninitialized_read`입니다. `if` 합류에서는 두 live 경로 모두 초기화했을 때만 완료 상태를 남깁니다. 한 경로만 쓴 경우에는 받지 않습니다.

이 작업 중 이름만으로 stack slot을 미리 잡는 기존 pre-pass의 경계도 드러났습니다. 주소가 사용된 이름을 안쪽 scope가 다시 선언하면 두 선언을 한 object에 묶어 다른 타입의 store를 만들 수 있었습니다. 선언별 object 식별을 아직 모델링하지 않으므로, 보이는 주소 사용 이름을 shadow하는 경우는 `unsupported_pointer` UNKNOWN으로 닫았습니다. 코퍼스에서 발견된 두 사례가 status 실패가 되지 않는 회귀 시험을 추가했습니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 9,232 (30.90%) | **9,263 (31.00%)** |
| verifier 통과 | 9,232 / 9,232 | **9,263 / 9,263** |
| status 실패 | 0 | **0** |

`tests/test_c_lower_locals.cpp`는 직접 대입과 양쪽 분기 초기화를 실제 컴파일된 C와 대조합니다. 무초기화 주소 노출, 한쪽 분기만 초기화, shadow 충돌은 각각 UNKNOWN인지 검사합니다.

### 22. `sizeof(expression)`은 피연산자를 내리지 않고 타입만 묻는다

커밋: (이 단위)

`sizeof(type)`만 받던 경로에 unevaluated designator 전용 정적 타입 질의를 추가했습니다. 식별자와 열거자, 문자열과 문자 리터럴, 괄호, 역참조와 주소 연산, 배열 원소, `.`와 `->` 멤버의 선언 타입을 재귀적으로 구합니다. 값이나 주소를 만들지 않고 load, store, call, UB guard도 내지 않습니다.

이 구분은 정확성 조건입니다. `sizeof(p[1000000])`은 포인터가 작은 object를 가리켜도 유효하고 `p`나 index를 읽지 않습니다. 평범한 로어링을 재사용하면 존재하지 않는 C UB를 만들게 됩니다. 지원하지 않는 식은 평가해 추측하지 않고 계속 UNKNOWN입니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 9,263 (31.00%) | **9,551 (31.96%)** |
| `unsupported_expression` | 1,722 | **811** |
| verifier 통과 | 9,263 / 9,263 | **9,551 / 9,551** |
| status 실패 | 0 | **0** |

`tests/test_c_lower_pointers.cpp`는 범위를 크게 벗어난 subscript도 평가하지 않는지 확인하고, `tests/test_c_lower_records.cpp`는 스칼라와 배열 멤버 크기를 실제 컴파일된 C와 대조합니다.

### 23. void cast는 값만 버리고 평가 결과는 버리지 않는다

커밋: (이 단위)

`(void)e`는 IR에 void 값을 만들지 않습니다. 대신 `e`를 정상 로어링해 호출, 대입, 메모리 효과를 그대로 남기고, 결과의 값 ID만 없앱니다. `defined`와 `may_ub`는 유지하므로 expression statement와 comma의 왼쪽처럼 값을 버리는 관찰 지점도 `e`의 UB guard를 냅니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 9,551 (31.96%) | **9,681 (32.40%)** |
| `unsupported_expression` | 811 | **584** |
| verifier 통과 | 9,551 / 9,551 | **9,681 / 9,681** |
| status 실패 | 0 | **0** |

`tests/test_c_lower_expressions.cpp`는 `(void)(a = b)`의 부작용을 컴파일된 C와 대조하고, `(void)(a / b)`가 0 나눗셈과 signed division overflow를 계속 UB로 보고하는지 확인합니다.

### 24. 작은 위치 기반 aggregate initializer를 저장한다

커밋: (이 단위)

최대 256개 원소의 배열과 레코드 지역에 위치 기반 initializer list를 내립니다. 각 scalar leaf를 object offset에 store하고, 생략된 하위 객체는 0으로 초기화합니다. 중첩 brace와 `{0}`, initializer에서 길이가 정해지는 `T a[] = {...}`도 같은 경로를 씁니다. 포인터 leaf의 0은 정수 store가 아니라 null pointer를 만들어 저장합니다.

지정 initializer는 member/index map이 필요하고, 다른 aggregate를 값으로 복사하는 초기화는 aggregate value 또는 `memcpy` 의미가 필요합니다. 둘 다 이번 범위에서 먼저 store를 내지 않고 UNKNOWN입니다. 위치 목록의 상한도 명시적이며, 상한을 넘기면 부분 초기화하지 않습니다.

이 과정에서 typedef가 두 단계 이상 record를 가리키면 storage pre-pass가 즉시 `is_aggregate` 표지만 보고 지역 object를 놓치는 기존 결함을 찾았습니다. pre-pass도 정식 typedef 해석을 사용하게 바꾸어, 값 ID 0을 포인터처럼 변환하던 status 실패를 막았습니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 9,681 (32.40%) | **9,751 (32.63%)** |
| `unsupported_expression` | 584 | **119** |
| verifier 통과 | 9,681 / 9,681 | **9,751 / 9,751** |
| status 실패 | 0 | **0** |

`tests/test_c_lower_aggregates.cpp`는 inferred array, 중첩 record/array, `{0}`, null pointer member, 두 단계 typedef record를 실제 컴파일된 C와 대조합니다.

### 25. 메모리에서 읽어 접근한 포인터에 유한 객체 슬롯을 붙인다

커밋: (이 단위)

포인터 인자, 전역, 문자열, 지역 저장소만으로 만든 기존 object 표는 `p->next->value`의 `next`가 가리키는 별도 객체를 이름 붙일 수 없었습니다. 이제 메모리에서 읽은 포인터를 실제로 역참조하거나 그 포인터를 통해 store할 때 보조 object 하나를 추가합니다. IR v1 본문은 비순환이므로 한 접근 지점은 한 실행에서 최대 한 새 객체만 요구합니다. 본문당 상한은 32개이며 넘으면 부분 로어링하지 않고 `unsupported_pointer` UNKNOWN입니다.

단순히 포인터 비트를 load, 비교, 반환하는 경우에는 object를 추가하지 않습니다. 대상에 접근하지 않은 함수의 입력 도메인을 불필요하게 좁히지 않기 위한 조건입니다. 분기와 SSA 합류에서도 양쪽 값이 기존 object를 갖거나 메모리 load에서 왔을 때만 이 권한을 유지합니다. 외부 호출이 반환한 포인터처럼 대상 저장소를 설명할 근거가 없는 값은 계속 UNKNOWN입니다.

보조 base/size는 본문 로어링 중 발견되므로 private builder 경로로 late parameter를 추가합니다. 모든 경로에 같은 전제조건이 걸리도록 본문을 다 만든 뒤 순수 prelude entry block을 앞에 붙입니다. 보조 descriptor는 이전 descriptor와 완전히 같은 base/size이거나 완전히 disjoint해야 합니다. 이 exact-alias 선택지가 있어야 load된 포인터가 이미 알려진 객체 안을 가리키는 유효한 실행을 배제하지 않습니다. 부분 overlap은 계속 금지합니다. 공개 IR builder의 parameters-first 계약은 바꾸지 않았습니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 9,751 (32.63%) | **11,920 (39.89%)** |
| 증가 | | **+2,169** |
| `unsupported_pointer` | 6,674 | **3,241** |
| verifier 통과 | 9,751 / 9,751 | **11,920 / 11,920** |
| status 실패 | 0 | **0** |

`tests/test_c_lower_records.cpp`는 서로 다른 head/tail 객체와 자기 자신을 가리키는 exact-alias 객체를 실제 컴파일된 C와 대조합니다. 같은 시험이 source signature와 late-parameter IR의 binding도 확인합니다. `tests/test_proof_smt_memory.cpp`는 양쪽 함수가 메모리에서 읽은 포인터를 따라가는 쌍을 product miter와 Bitwuzla까지 실행해 `PROVED_EQUIVALENT`를 확인합니다. 기존 aggregate differential도 포인터 멤버를 읽기만 하는 경우 보조 object가 생기지 않는 회귀 시험 역할을 합니다.

### 26. 호출 반환값과 정수 주소도 접근 시점에 객체를 얻는다

커밋: (이 단위)

flat-address profile에서는 외부 호출이 반환한 포인터와 정수에서 포인터로 바꾼 주소도 기존 또는 외부 live object를 가리킬 수 있습니다. 이 값들도 실제 load/store가 뒤따를 때만 보조 descriptor를 추가합니다. 단순 null 비교나 포인터 반환은 새 전제조건을 만들지 않습니다. 호출은 기존처럼 trace와 memory를 소비하고 새 상태를 내며, 동적 object는 그 호출 결과 주소의 접근 가능 범위만 설명합니다.

정수 인자만 받는 함수가 `(T *)address`를 역참조하면 기존 pre-pass에는 memory parameter를 예측할 단서가 없습니다. 이 경우 접근 직전에 `__memory`와 object base/size를 private late-parameter 경로로 함께 추가합니다. 이 경로를 넣기 전 코퍼스의 두 본문이 잘못 status 실패했으며, 지금은 둘 다 로어링되고 전체 status 실패가 다시 0입니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 11,920 (39.89%) | **14,015 (46.90%)** |
| 증가 | | **+2,095** |
| `unsupported_pointer` | 3,241 | **211** |
| verifier 통과 | 11,920 / 11,920 | **14,015 / 14,015** |
| status 실패 | 0 | **0** |

`tests/test_c_lower_calls.cpp`는 실제 C callee가 반환한 주소와 같은 객체를 interpreter에 공급해 호출, 포인터 결과, 후속 load를 한 번에 대조합니다. `tests/test_c_lower_pointers.cpp`는 64비트 정수 주소를 포인터로 바꾸는 코퍼스 형태를 실제 실행과 대조합니다. `tests/test_proof_smt_calls.cpp`는 같은 포인터 반환 호출을 서로 다르게 쓴 두 함수를 product miter와 Bitwuzla까지 보내 동적 object와 호출 congruence가 함께 `PROVED_EQUIVALENT`를 만드는지 확인합니다.

### 27. variadic 호출의 추가 인자에 default promotions를 적용한다

커밋: (이 단위)

ellipsis 앞의 고정 인자는 계속 prototype의 선언 타입으로 변환합니다. 그 뒤 추가 인자는 C default argument promotions를 적용합니다. 현재 restricted slice에는 부동소수점 값이 없으므로 작은 정수와 `_Bool`은 integer promotion, 데이터 포인터는 같은 타입 유지가 전부입니다. 지원 타입 밖의 값은 추측하지 않고 `unsupported_call` UNKNOWN입니다. 호출 IR에는 promotion 뒤의 폭과 부호가 operand 타입으로 남으므로 interpreter callback, 호출 trace, product congruence가 같은 ABI 인자를 봅니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 14,015 (46.90%) | **14,507 (48.55%)** |
| 증가 | | **+492** |
| `unsupported_call` | 1,473 | **162** |
| verifier 통과 | 14,015 / 14,015 | **14,507 / 14,507** |
| status 실패 | 0 | **0** |

첫 차단 `a variadic callee has no fixed signature` 1,317개를 제거했지만 825개는 뒤의 loop, control flow, type 한계로 이동했습니다. `tests/test_c_lower_calls.cpp`는 `short`와 `unsigned char`가 모두 32비트 `int`로 전달되는지 실제 `va_arg(int)` 실행과 대조합니다. `tests/test_proof_smt_calls.cpp`는 한쪽만 명시적 `int` 지역을 거치는 같은 variadic 호출을 Bitwuzla까지 보내 `PROVED_EQUIVALENT`를 확인합니다.

### 28. switch의 dispatch, fallthrough, break를 비순환 SSA로 내린다

커밋: (이 단위)

제어식을 한 번 평가하고 integer promotion을 적용한 뒤, 각 case 상수를 차례로 비교하는 비순환 dispatch chain을 만듭니다. case label은 지역 읽기나 호출을 받지 않는 integer constant expression으로 제한하고, 제어식의 승격 타입으로 변환해 비교합니다. `default`는 소스 중간에 있어도 dispatch의 마지막 no-match target이 되고, case 실행은 소스 순서대로 이어져 빈 label과 fallthrough를 그대로 보존합니다.

각 case block에는 dispatch 경로와 바로 앞 case의 fallthrough 경로가 함께 들어올 수 있습니다. 기존 두 갈래 병합을 임의 개수 predecessor의 SSA 병합으로 일반화하여 scalar, definite initialization, pointer object 권한, memory, external-call trace를 모두 같은 PHI 규칙으로 합쳤습니다. switch exit도 no-match, 여러 `break`, 마지막 fallthrough를 한 번에 병합합니다.

`break`는 만나는 즉시 아직 존재하지 않는 exit block으로 branch하지 않습니다. source block과 상태를 기록해 두고 모든 case를 내린 뒤 live exit가 실제로 있을 때만 block을 만들고 edge를 완성합니다. 모든 case와 default가 반환하는 switch에 도달 불가능한 빈 block을 남기지 않기 위한 조건입니다. 중첩 switch는 break scope stack으로 가장 안쪽 switch만 빠져나갑니다.

switch body나 case에 직접 선언되어 뒤 case와 scope를 공유하는 형태는 경로별 object lifetime과 bypassed initialization을 따로 모델링해야 하므로 이번 단위에서는 compound block으로 감싼 선언만 받습니다. 부분 lowering하지 않고 `unsupported_control_flow` UNKNOWN으로 남깁니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 14,507 (48.55%) | **15,751 (52.71%)** |
| 증가 | | **+1,244** |
| `unsupported_control_flow` | 2,715 | **1,145** |
| verifier 통과 | 14,507 / 14,507 | **15,751 / 15,751** |
| status 실패 | 0 | **0** |

`tests/test_ir_differential.cpp`는 unsigned 입력의 match, fallthrough, 중첩 조건 안 break, default를 실제 컴파일된 C와 edge 및 random 입력에서 대조합니다. interpreter 시험은 중간 `default`, all-return switch, 문자와 계산 상수를 고정합니다. product miter는 switch와 같은 if chain의 위반식이 UNSAT이고 domain이 SAT인지 Bitwuzla로 확인합니다.

### 29. 전방 goto의 지연 edge와 label 합류를 내린다

커밋: (이 단위)

함수 body의 direct label을 본문 lowering 전에 수집하되 IR block은 만들지 않습니다. 전방 `goto`를 만나면 현재 source block과 함수 scope 변수, memory, call trace 상태를 target label에 보관하고 그 경로를 종료합니다. top-level body scanner는 죽은 평문을 건너뛰다가 pending predecessor가 있는 label에서 다시 시작합니다.

label에 정상 fallthrough도 도달하면 goto 상태들과 함께 임의 predecessor PHI로 병합합니다. goto가 `if`나 중첩 compound 안에 있어도 안쪽 지역을 버리고 label에서 보이는 함수 scope prefix만 운반합니다. 여러 cleanup jump와 정상 경로가 한 label에 모이는 형태도 같은 규칙입니다. label block과 branch terminator는 실제 predecessor가 있을 때만 만들므로 도달 불가능 block을 남기지 않습니다.

후방 goto는 v1 CFG에 cycle을 만들므로 계속 UNKNOWN입니다. goto와 label 사이에 함수 scope 선언이 있으면 그 선언의 초기화를 우회한 상태를 별도로 만들어야 하므로 이번 단위에서는 명시적으로 거부합니다. nested label도 direct-label slice 밖입니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 15,751 (52.71%) | **16,388 (54.85%)** |
| 증가 | | **+637** |
| `unsupported_control_flow` | 1,145 | **118** |
| verifier 통과 | 15,751 / 15,751 | **16,388 / 16,388** |
| status 실패 | 0 | **0** |

compiled differential은 두 전방 label, 중첩 block의 goto, 여러 합류 경로를 실제 C 실행과 edge 및 random 입력에서 대조합니다. product miter는 같은 cleanup을 early return과 structured if로 쓴 함수와 동치임을 증명합니다. 후방 edge와 선언 우회는 UNKNOWN 회귀 시험으로 고정합니다.

### 30. 순환 CFG와 구조적 C 루프를 내린다

커밋: `702131e`

IR payload에 이미 있던 `cfg_kind`를 사용해 `QL_IR_CFG_CYCLIC`을 추가했습니다. artifact schema와 payload 배치는 바꾸지 않았습니다. builder는 루프 헤더 결과를 먼저 노출한 뒤 back-edge가 완성될 때 incoming을 추가하는 append-only PHI API를 제공합니다. decoder와 reader view는 선언된 CFG 종류를 보존합니다.

코어 검증은 비순환 그래프의 기존 Kahn 경로를 그대로 유지하고, 순환 그래프에서는 DFS reverse postorder와 반복 immediate-dominator 계산을 사용합니다. 독립 verifier는 지배자 비트 집합을 고정점까지 교집합합니다. 인터프리터는 back-edge를 실제로 실행하고 step limit을 실행 예산으로 사용하며, 한 블록의 PHI들을 이전 상태 snapshot에서 동시에 읽습니다. loop-free SMT product는 순환 IR을 명시적으로 거부하므로 루프를 증명했다고 주장하지 않습니다.

C lowering은 `for`, `while`, `do while`을 구조 그대로 내립니다. 루프 진입 상태와 각 정상 back-edge, `continue` edge를 header PHI로 연결하고 scalar, memory, external-call trace를 함께 운반합니다. `for`의 `continue`는 update block을 거쳐 가며, `do while`의 `continue`는 조건 block으로 갑니다. `break`와 `continue` scope를 분리해 루프 안의 중첩 switch에서 `break`는 switch만, `continue`는 루프를 대상으로 합니다. ordinary label과 goto가 루프 안에 섞인 경우는 아직 별도 scope/lifetime 모델이 없어 UNKNOWN입니다.

`const T *p`에서 `const`가 포인터 객체가 아니라 pointee를 한정한다는 점도 바로잡았습니다. 이 수정 전에는 포인터 인자나 지역 포인터의 정상 대입을 const 객체 수정으로 잘못 거부했습니다. 초기화 없는 const 객체는 존재할 수 있지만 읽기와 대입은 각각 기존 uninitialized/const 규칙으로 계속 거부합니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 16,388 (54.85%) | **20,249 (67.77%)** |
| 증가 | | **+3,861** |
| `unsupported_loop` | 5,076 | **0** |
| verifier 통과 | 16,388 / 16,388 | **20,249 / 20,249** |
| status 실패 | 0 | **0** |

`tests/test_ir.cpp`는 self-loop의 교차 PHI가 병렬로 실행되는지, cyclic artifact가 왕복 후 독립 verifier를 통과하는지 확인합니다. `tests/test_ir_interp.cpp`는 세 루프와 `break`/`continue` 결과를 고정합니다. `tests/test_ir_differential.cpp`는 같은 루프 소스를 실제 컴파일해 경계값과 무작위 입력에서 IR interpreter와 비교합니다.

### 31. 8비트 octal과 hexadecimal escape를 해석한다

커밋: (이 단위)

ordinary character와 string literal의 `\\ooo`, `\\xhh` escape를 실제 byte로 해석합니다. octal은 C 규칙대로 최대 세 자리, hexadecimal은 뒤따르는 모든 hex digit을 소비합니다. 값이 target `unsigned char`의 8비트를 넘으면 잘라내지 않고 UNKNOWN으로 남깁니다.

ASM2C_GNU_V1의 plain `char`는 signed이므로 ordinary character constant는 8비트 byte를 C의 `int`로 sign extension합니다. string literal은 같은 byte를 변형하지 않고 memory image에 보관합니다. 따라서 `\'\\xc0\'`는 정수 `-64`, `"\\xc0"`의 첫 저장 byte는 `0xc0`입니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 20,249 (67.77%) | **20,279 (67.87%)** |
| 증가 | | **+30** |
| verifier 통과 | 20,249 / 20,249 | **20,279 / 20,279** |
| status 실패 | 0 | **0** |

`tests/test_c_lower_expressions.cpp`는 ordinary character escape를 실제 컴파일된 C와 대조하고 범위를 넘는 escape를 UNKNOWN으로 고정합니다. `tests/test_c_lower_aggregates.cpp`는 string escape가 정확한 memory byte를 요구하는지 확인합니다. 전체 train 측정은 단일 `coverage` 프로세스가 순차 실행이라는 점을 고려해 6개 샤드로 병렬화한 뒤 카운터와 29,880개 상세 행을 합쳤습니다.

### 32. 초기화되지 않은 scalar 읽기를 경로별 UB로 표현한다

커밋: (이 단위)

초기화되지 않은 scalar에는 임의의 답을 주지 않습니다. SSA value와 별도로 그 값이 현재 경로에서 초기화되었는지를 나타내는 boolean predicate를 운반하고, 읽는 순간 predicate가 거짓이면 explicit UB guard가 실행됩니다. 분기 합류에서는 값과 predicate를 각각 PHI로 합치고, loop header와 back-edge도 두 PHI를 함께 갱신합니다. 아직 초기화되지 않은 경로의 typed zero는 PHI를 구성하기 위한 placeholder일 뿐이며 predicate가 거짓인 경로에서 관찰 가능한 반환값이 되지 않습니다.

따라서 `int x; if (take) x = 7; return x;`는 `take != 0`에서 7을 반환하고 반대 경로에서 UB입니다. `x += 1`처럼 쓰기 전에 읽는 연산도 같은 규칙을 사용합니다. 반면 초기화되지 않은 지역의 주소가 외부 호출로 escape하는 경우는 callee가 그 저장소를 쓸 수 있으므로 계속 UNKNOWN입니다. 이를 지원하려면 외부 호출의 memory-write 계약이 먼저 필요합니다.

| | 이전 | 이후 |
|---|---:|---:|
| train 로어링 (29,880) | 20,279 (67.87%) | **20,688 (69.24%)** |
| 증가 | | **+409** |
| `uninitialized_read` | 2,448 | **1,972** |
| verifier 통과 | 20,279 / 20,279 | **20,688 / 20,688** |
| status 실패 | 0 | **0** |

`uninitialized_read`는 476개 줄었지만 그중 67개는 뒤에 있던 다른 미지원 의미론으로 이동했으므로 최종 성공 순증은 409개입니다. `tests/test_c_lower_locals.cpp`는 초기화된 분기와 UB 분기, compound assignment를 interpreter로 고정합니다. 기존 UNKNOWN 목록도 새 계약에 맞게 갱신했습니다. 전체 검증은 Windows 530/530, Linux Clang 529/529, Linux ASan/UBSan 529/529입니다. `fuzz_c_lower`는 60초 동안 269,343회, `cov: 2833`, crash 0으로 끝났습니다.

### 33. coverage detail에 첫 진단 메시지를 보존한다

커밋: (이 단위)

기존 detail TSV의 path, function, outcome, diagnostic code 네 열은 그대로 두고 첫 진단 메시지를 다섯 번째 열로 추가했습니다. 기존 `cut -f4`와 3열 이상을 읽는 판정률 도구는 그대로 동작합니다. 메시지의 tab과 줄바꿈은 한 행을 깨지 않도록 공백으로 정규화합니다.

29,880개 train 행을 6개 프로세스로 다시 생성해 모든 행이 정확히 5열이고 verifier 실패와 status 실패가 0임을 확인했습니다. 이 측정으로 최신 9,192개 UNKNOWN의 첫 원인을 추정 없이 분류할 수 있습니다. 상위 메시지는 uninitialized address escape 1,972개, 지원하지 않는 record member 형태 967개, 알려지지 않은 type spelling 754개, function local 또는 다차원/동적 array 601개, record value 552개입니다.

## 막힌 것

- 없음

## 알게 된 로어링 공백

현재 train 재측정으로 순위를 정한 큰 범주입니다. 각 범주 안의 독립 원인은 메시지별로 다시 나눕니다.

- `unsupported_type` 4,508
- `uninitialized_read` 1,972
- `type_error` 702
- `unsupported_pointer` 649
- `unsupported_control_flow` 435

## 조율자에게 요청할 것

### 1. libFuzzer 타깃과 Linux sanitize 프리셋

`tests/test_fuzz.cpp` 를 `QL_FUZZ_LIBFUZZER` 를 정의하고 `-fsanitize=fuzzer,address,undefined` 로 컴파일하는 실행 파일 하나만 있으면 됩니다. `LLVMFuzzerTestOneInput` 이 이미 그 안에 있습니다. `CMakeLists.txt` 와 `CMakePresets.json` 이 조율자 소유라 W1 이 직접 못 합니다.

지금 상태로도 결정적 캠페인이 매 `ctest` 마다 돌지만, **coverage-guided 가 아니므로 G8 의 "퍼징 크래시 0" 을 완전히 닫았다고 보지 않습니다.**

### 2. 커버리지 도구의 verifier 계측

커버리지 도구(`quodlibet coverage`)에 **`SUPPORTED` 인 본문에 대해 `ql_ir_verify` 를 돌리고 실패를 별도 열로 세는 계측**을 넣어 주시면 좋겠습니다. 지금은 53개뿐이라 시험 표본으로도 충분하지만, 타입과 포인터를 열면 수만 개가 되므로 코퍼스 전체에 대한 verifier 통과가 G8 종료 조건("IR verifier 가 로어링 출력 전부에 대해 통과한다")의 유일한 증거가 됩니다.

## 다음에 할 것

1. reaching `}`인 integer 함수는 관찰되는 반환값이 없으므로 explicit UB로 내려 128개 `missing_return`을 닫습니다.
2. 남은 타입 철자와 선언 형태를 빈도순으로 닫고, uninitialized address escape는 외부 호출의 memory-write 계약을 먼저 고정합니다.
3. 루프 안 ordinary label/goto와 후방 goto는 scope, lifetime, loop-carried 상태를 보존하는 경우에만 순환 CFG로 확장합니다.
4. 각 단위마다 compiled differential, source-signature binding, verifier 전수 통과, status 실패 0을 유지하고 전체 train을 다시 측정합니다.
