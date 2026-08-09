# W10 진행 기록: AIG/SAT backend 와 certificate checker

브랜치: `stuxnet147/w10-aig-sat`
지시서: `docs/workstreams/W10.md`

## 지금 하는 것

QF_BV bit-blaster 를 끝냈습니다. 다음은 kissat / drat-trim 벤더링과 `prove.aig-sat` method 본체입니다.

## 끝난 작업 단위

### 1. SAT solver / proof 형식 / checker 후보 비교 (`4264e16`)

`docs/notes/sat-backend-and-proof-checker.md`.

권고는 **경로 A** 입니다.

- solver: Kissat rel-4.0.4 (MIT, C99, DRAT 만 냄)
- elaborator: drat-trim (MIT, C 한 파일). **신뢰하지 않습니다**
- checker: `lrat-check` (drat-trim 저장소 동봉, MIT, C 한 파일 약 18KB). **여기에만 신뢰가 남습니다**

핵심 논거는 drat-trim 의 DRAT -> LRAT elaborate 출력을 원본 CNF 에 대해 `lrat-check` 로 다시 검사하면 drat-trim 이 틀려도 통과하지 못한다는 것입니다. 그래서 Kissat 을 유지하면서도 TCB 가 `lrat-check.c` 한 파일로 줄어듭니다. CaDiCaL 의 native LRAT(경로 B)도 TCB 가 같은 크기인데, 이미 `DEPENDENCIES.md` 와 `THIRD_PARTY_NOTICES.md` 에 적힌 Kissat pin 을 뒤집을 만큼의 이득이 없습니다.

cake_lpr(경로 C, HOL4 로 기계어까지 검증)은 첫 절단에 넣지 않고, **checker 를 method option 으로 교체 가능한 경계로 설계**해서 나중에 붙일 수 있게 둡니다.

### 2. 조율자 답 (닫힘)

경로 A 승인. 조건 넷을 받았습니다.

1. `checked_proof=true` 는 `lrat-check` 가 원본 CNF 와 LRAT 에 대해 통과했을 때만이고, envelope 에 kissat / drat-trim / lrat-check 세 실행 파일의 BLAKE3 digest 를 전부 기록합니다
2. 세 프로세스 모두 기존 Bitwuzla adapter 의 규율(절대 경로, deadline, 출력 상한, shell 없음)을 따릅니다
3. **Windows 빌드가 필수입니다.** Kissat 의 POSIX 의존이 마찰을 내면 후보를 바꾸기 전에 측정과 함께 다시 물어봅니다
4. 벤더는 SHA-256 pin, configure 중 네트워크 금지

`scripts/vendor.sh` 와 `third_party/CMakeLists.txt` 편집은 이 벤더 추가에 한해 위임받았습니다. 기존 pin 은 건드리지 않습니다.

`CMakeLists.txt` 는 조율자가 `main` `bc65956` 에서 `src/aig.c`, `src/proof_aigsat.c`, `src/proof_diff.c` 를 예약했습니다. 그 위로 rebase 했습니다.

### 3. `refute.concrete-differential` (5단계)

`include/quodlibet/proof_diff.h`, `src/proof_diff.c`, `tests/test_proof_diff.cpp` 13개 시험. `src/builtins.c` 에 등록을 한 줄 추가하고 `include/quodlibet/quodlibet.h` 에 헤더를 넣었습니다. `METHODS.md` 의 concrete differential 절에 구현 결정을 적었습니다. CTest 280/280 통과입니다.

### 4. AIG 회로 계층 (2단계 전반)

`include/quodlibet/aig.h`, `src/aig.c`, `tests/test_aig.cpp` 12개 시험. `METHODS.md` AIG/SAT 절에 회로 계층 소절을 넣었습니다. CTest 292/292 통과입니다.

담은 것은 이렇습니다.

- and-inverter graph. 반전은 edge 에 싣고 node 를 쓰지 않습니다. 상수 folding, 한쪽 피연산자 항등식 네 개, 피연산자 정규 순서 + 구조 해싱
- bit-vector 계층: ripple-carry 덧셈, 폭을 유지하는 shift-and-add 곱셈, 나머지를 한 비트 넓게 잡는 restoring 나눗셈(부호 있음/없음), barrel shift 세 종류, 비교 네 개와 eq, mux, zext/sext/trunc, reduce or/and
- 구체 평가기(`ql_aig_evaluate`). node 색인이 곧 위상 순서라 재귀 없이 한 번 훑습니다
- CNF: 루트 하나의 cone of influence 에 대한 Tseitin, 결정적 DIMACS 바이트, input 색인에서 DIMACS 변수로 가는 표

### 5. miter 구조 결정: query bytes 를 blast 한다 (조율자 승인)

`W10.md` 2항은 IR 을 직접 bit-blast 하라고 씁니다. `src/product.c` 를 읽고 나서 그 문구에 반대 의견을 냈고 조율자가 받아들였습니다.

`product.c` 는 이미 계약 전체를 인코딩합니다. block reachability, PHI, 관찰 축, UB policy 와 그 totalization, typed precondition, relation 방향, 메모리와 effect 와 비스칼라 타입의 거부까지입니다. 이것을 bit 수준에서 다시 지으면 C 의미론의 인코더가 둘이 되고, `checked_proof=true` 는 그 둘이 절대 어긋나지 않을 때만 값을 갖습니다. **틀린 질문에 대한 검증된 증명은 권위를 가진 오답이라 검증 없는 정답보다 나쁩니다.**

그래서 AIG 경로는 Bitwuzla 에게 보내는 것과 **같은 query bytes** 를 bit-blast 합니다. 두 backend 가 구성상 같은 질문에 답하고, envelope 이 양쪽에 같은 query digest 를 기록합니다.

조율자가 붙인 조건 넷입니다.

1. 임의 SMT-LIB 파서가 아니라 `ql_smt2_builder` 가 내는 닫힌 문법의 파서. 문법 밖은 전부 거부하고 `UNKNOWN`. `Array` sort 는 거부 (닫힘)
2. round-trip 시험과, 같은 query bytes 에 대해 Bitwuzla 와 kissat 의 SAT/UNSAT 일치 시험. 불일치는 다수결하지 않고 상태 오류로 크게 표면화 (round-trip 과 인터프리터 일치는 닫힘, kissat 대조는 벤더링 뒤)
3. 파서를 `tests/fuzz/` 표면에 추가 (닫힘)
4. envelope 에 AIG 경로의 query digest 가 SMT 경로의 그것과 같은 값으로 기록 (method 본체에서)

### 6. QF_BV bit-blaster (2단계 후반)

`include/quodlibet/proof_aigsat.h`, `src/proof_aigsat.c`, `tests/test_proof_aigsat.cpp` 10개 시험, `tests/fuzz/fuzz_blaster_target.h` 와 `tests/fuzz/fuzz_smt2_blaster.c`. `METHODS.md` 에 "Where the miter comes from" 소절을 넣었습니다. CTest 302/302 통과입니다.

가장 중요한 시험은 **인터프리터 일치 시험**입니다. 진짜 product query 를 blast 하고, blast 한 miter 가 어떤 입력을 위반이라고 부르는 것과 `ir_interp` 로 좌우를 실제로 돌린 결과가 정확히 같은지를 경계값과 의사난수 입력에서 요구합니다. 나눗셈, 나머지, shift, typed precondition 을 각각 겁니다. 어긋나면 크게 실패합니다.

`tests/fuzz/fuzz_targets.h` 와 `tests/test_fuzz.cpp` 는 W1 소유라 건드리지 않았습니다. blaster target 은 자기 헤더에 두고 libFuzzer driver 와 결정적 캠페인이 같은 본문을 씁니다.

## 내린 설계 결정

- **신뢰는 checker 한 곳으로 모읍니다.** solver 도 elaborator 도 신뢰하지 않습니다. 이것이 이 워크스트림이 `prove.smt-product` 와 다른 유일한 이유입니다
- **LRAT 를 씁니다.** DRAT checker 는 전파 탐색을 스스로 해야 해서 커집니다. LRAT 는 antecedent 가 붙어 있어 checker 가 재생만 하면 되고, 그래서 감사 가능한 크기가 됩니다
- **solver 와 checker 는 실행 파일입니다.** `libquodlibet` 은 둘 다 링크하지 않습니다. 기존 Bitwuzla adapter(`src/solver.c`)의 snapshot / deadline / 출력 상한 패턴을 그대로 따릅니다
- **checker 는 교체 가능합니다.** method option 이 실행 파일과 형식을 받고, envelope 이 어떤 checker 가 무엇을 검사했는지 기록합니다

### AIG 쪽

- **부분 연산은 여기서 totalize 하고 여기서 판정하지 않습니다.** 0 으로 나누기는 SMT-LIB 값(몫은 all ones, 나머지는 피제수), 폭 이상 shift 는 0/0/부호비트, signed division overflow 는 wrap 입니다. 회로는 total 해야 하고, 그 연산이 허용된 것이냐는 UB guard 의 질문입니다. 회로가 그것까지 답하면 같은 질문에 답하는 곳이 두 군데가 되고 둘이 어긋날 수 있습니다. SMT 인코딩이 이미 하는 것과 같은 선택입니다
- **구조 해싱이 miter 에서 값을 냅니다.** 좌우가 input wire 를 공유하므로 서로 같은 부분이 solver 를 부르기 전에 한 node 로 붕괴합니다. `EqualSubcircuitsOfTwoFunctionsCollapseIntoOneNode` 가 이것을 고정합니다
- **`src/aig.c` 는 IR 을 모릅니다.** 회로 라이브러리이고 시험은 IR 하강이 아니라 평범한 C 산술과 대조합니다. 4비트 두 피연산자 전수(256쌍)로 모든 연산을 겁니다. IR -> AIG 하강은 `src/proof_aigsat.c` 로 갑니다
- **CNF 는 루트 하나의 cone of influence 만 냅니다.** 루트가 닿지 않는 회로는 solver 에게 비용이 0 입니다. 루트가 상수로 folding 되면 solver 를 부르지 않고 trivially true/false 로 보고하며, 그것은 checked proof 가 아닙니다

### bit-blaster 쪽

- **파서는 한 producer 전용입니다.** 일반 SMT-LIB front end 가 아닙니다. 문법 밖 입력은 추측하지 않고 `QL_STATUS_TYPE_MISMATCH` 로 거부합니다. `extract` 는 low index 가 0 인 것만 받습니다(emitter 가 그것만 냅니다). 조용히 shift 를 끼워 넣지 않습니다
- **정의된 symbol 은 자유 변수가 아닙니다.** `declare-const` 만 AIG input 을 받고 model decoding 대상입니다. `define-fun` 은 회로입니다
- **AIG 경로가 읽는 유일한 외부 바이트가 이 파서입니다.** 그래서 fuzz target 이 여기에 붙습니다. 나머지 경로는 구조체를 소비합니다

### `refute.concrete-differential` 쪽

- **native sandbox 를 쓰지 않고 `ir_interp` 로 좌우를 돌립니다.** `METHODS.md` 원문은 instrumented native runner 를 말하지만, native 실행은 C UB 를 조용히 통과시키고 ABI 와 CPU feature 가정을 새로 만들어야 합니다. 인터프리터는 definedness 를 명시적으로 모델하므로 그 두 문제가 없습니다
- **생성한 입력을 solver-model 문법으로 직렬화해서 `ql_replay_decode_model` 에 넣습니다.** 두 번째 decoder 를 만들지 않기 위해서입니다. SMT 가 낸 witness 와 differential 이 만든 witness 가 같은 decoder 와 같은 relation 평가기를 지납니다. 디코더가 둘이면 서로 어긋날 수 있고, 하나면 어긋날 수 없습니다
- **아무것도 못 찾으면 `UNKNOWN` 입니다. `BOUNDED_CLEAN` 이 아닙니다.** `METHODS.md` 원문은 유한 시험 통과에 `BOUNDED_CLEAN` 을 허용하지만 이 구현은 쓰지 않습니다. `BOUNDED_CLEAN` 은 어떤 bound 를 소진했다는 뜻인데 64비트 공간의 무작위/경계 표본은 어떤 bound 도 소진하지 않습니다. capability 가 `QL_PROOF_RESULT_BOUNDED` 를 어떤 option 으로도 켜지 않습니다. 지시서(`W10.md` 5항)와 같은 판단이고 `METHODS.md` 에 근거를 적었습니다
- **찾은 반례는 replay 단계가 따로 없습니다.** 좌우를 concrete 하게 돌려서 나온 것이므로 정의상 replay 된 것입니다. `replay_confirmed` 를 기록하고 SMT 경로와 같은 `quodlibet.counterexample` artifact 를 냅니다
- **생성기는 결정적입니다.** splitmix64 에 seed 를 물리고, 첫 12개 tuple 은 모든 입력에 같은 경계 패턴(0, 1, all ones, sign bit, signed 극값, 교대 패턴, 바이트 경계, half-width bit)을 넣은 뒤 무작위 단계로 갑니다. seed 와 시험 수와 query digest 가 cache key 에 들어가므로 다른 표본은 다른 검색입니다
- **`src/builtins.c` 를 한 줄 고쳤습니다.** W10 소유 파일이 아닙니다. 새 method 를 built-in registry 에 넣는 등록 호출뿐이고 기존 동작을 바꾸지 않습니다

## 막힌 것

없습니다.

## Windows 이식 위험

Kissat 과 drat-trim 은 둘 다 `unistd.h`, `sys/resource.h`, `sys/time.h` 를 씁니다. `windows-clang` 프리셋은 MSVC ABI 이므로 그대로는 빌드되지 않습니다. tree-sitter 처럼 자체 CMake target 으로 컴파일하면서 얇은 이식 shim 을 넣는 방향으로 봅니다. 실제 규모는 벤더링해 본 뒤 여기에 적습니다.

## 다음에 할 것

1. 벤더링. kissat rel-4.0.4 와 drat-trim 을 SHA-256 pin 으로 `scripts/vendor.sh` 에 넣고 `third_party/CMakeLists.txt` 에 실행 파일 target 을 만든다. Windows 이식 마찰을 여기에 기록한다
2. 3단계와 4단계. `prove.aig-sat` method 본체. product query -> blast -> CNF -> kissat -> drat-trim(신뢰 안 함) -> lrat-check. checker 통과 뒤에만 `PROVED_*` 와 `checked_proof=true`. envelope 에 세 실행 파일 digest 와 SMT 경로와 같은 query digest
3. Bitwuzla 와 kissat 이 같은 query bytes 에 같은 답을 내는지 고정하는 시험 (조율자 조건 2 의 나머지 절반)
