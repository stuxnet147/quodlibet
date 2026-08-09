# W10 진행 기록: AIG/SAT backend 와 certificate checker

브랜치: `stuxnet147/w10-aig-sat`
지시서: `docs/workstreams/W10.md`

## 지금 하는 것

CaDiCaL 2.2.1 과 `lrat-check` 벤더링을 끝냈습니다(`302e0de`). 남은 것은 `prove.aig-sat` method 본체와 Bitwuzla 대조 시험입니다. **그 둘을 막는 것은 판단이 아니라 없는 부품 하나입니다: 범용 프로세스 실행기.** 아래 "다음에 할 것" 에 설계를 적었습니다.

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

### 7. CaDiCaL 전환과 벤더링 (`302e0de`)

kissat 실격 뒤 조율자가 CaDiCaL 로 결정했고, 조건 (a) 대로 **벤더링 전에** 양 플랫폼 프로브를 먼저 돌렸습니다.

| 프로브 | Windows (`windows-clang`, MSVC ABI) | Linux (WSL Ubuntu 24.04, gcc) |
|---|---|---|
| 사소 SAT | `s SATISFIABLE`, 코드 10 | 같음 |
| 사소 UNSAT (kissat 이 틀린 그 식) | `s UNSATISFIABLE`, 코드 20 | 같음 |
| 중간 UNSAT (pigeonhole 9/8) | 코드 20, LRAT 70,701줄, **VERIFIED** | 코드 20, LRAT 69,002줄, **VERIFIED** |
| 중간 SAT (무작위 3-SAT 250변수) | `s SATISFIABLE`, model 미충족 절 0개 | `s SATISFIABLE` |

전부 통과해서 벤더링했습니다.

- `scripts/vendor.sh` 에 CaDiCaL rel-2.2.1(`16d24cc1...`)과 drat-trim 커밋 `2e3b2dc`(`a75e5a20...`) SHA-256 pin 추가
- `third_party/CMakeLists.txt` 에 `ql_cadical` 과 `ql_lrat_check` 실행 파일 target. `QL_ENABLE_SAT` 로 끕니다. **`libquodlibet` 은 둘 다 링크하지 않습니다**
- `third_party/quodlibet-compat/`: Windows 에 없는 헤더 넷과 prelude 하나. **pin 한 소스를 고치지 않습니다.** 고치면 checksum 이 빌드되는 것과 다른 것을 가리킵니다
- `DEPENDENCIES.md`, `THIRD_PARTY_NOTICES.md`, `docs/notes/sat-backend-and-proof-checker.md` 에 결정 기록 (조건 c)

**drat-trim 이 사슬에서 빠졌습니다.** CaDiCaL 이 LRAT 를 직접 내므로 kissat -> drat-trim -> lrat-check 세 프로세스가 CaDiCaL -> lrat-check 두 프로세스가 됩니다. TCB 는 그대로 `lrat-check.c` 한 파일입니다. `drat-trim.c` 는 벤더에 있지만 빌드하지 않습니다.

루트 `CMakeLists.txt` 는 조율자 소유라 건드리지 않았습니다. 실행 파일 경로 두 개는 `cmake_language(DEFER DIRECTORY ...)` 로 최상위 스코프 끝에 `target_compile_definitions` 를 걸어 전달합니다.

CTest 317/317 통과입니다.

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

없습니다. 남은 것은 판단이 아니라 분량입니다.

## Windows 이식 측정 (2026-08-10)

조율자 조건 3("Kissat 의 POSIX 의존이 Windows clang 에서 마찰을 내면 후보를 바꾸기 전에 측정과 함께 다시 물어보세요")에 대한 측정입니다. **저장소에는 아무것도 벤더링하지 않았습니다.** 전부 scratch 에서 쟀습니다.

고정한 것부터.

- kissat rel-4.0.4 tarball SHA-256 `bfe93eaa6323b48011e4b1fcf74b3f2e20f9de544767e728009e5b2018296193` (<https://codeload.github.com/arminbiere/kissat/tar.gz/refs/tags/rel-4.0.4>)
- drat-trim 커밋 `2e3b2dc0ecf938addbd779d42877b6ed69d9a985` (2024-11-25) tarball SHA-256 `a75e5a2072fa5a5493ee8504067661c6b452d45a32f889ac5b2c9470c227b58c`. MIT. `drat-trim.c` 1501줄, `lrat-check.c` 509줄

### 마찰 1. tarball 에 symlink 가 있다

`test/cnf/hard.cnf` 가 symlink 라 Windows 에서 `tar -xzf --strip-components=1` 이 실패합니다. `scripts/vendor.sh` 의 `fetch` 는 그대로 쓸 수 없고 `--exclude='test/*'` 를 주는 변형이 필요합니다. 작은 문제입니다.

### 마찰 2. POSIX shim (해결됨)

`windows-clang`(MSVC ABI)에서 93개 소스 중 92개가 약 60줄짜리 shim 으로 컴파일됩니다. shim 이 채우는 것은 `unistd.h`(isatty, access, popen, sysconf, alarm, R_OK), `strings.h`(strcasecmp), `sys/time.h`(gettimeofday), `sys/resource.h`(getrusage), 그리고 prelude 의 `SIGBUS`/`SIGALRM`/`SIGQUIT`/`S_ISDIR`/`S_ISREG` 입니다. 링크에는 16MB 이상 스택이 필요합니다(`-Wl,/stack:...`). 여기까지는 감당할 수 있는 크기입니다.

### 마찰 3. **MSVC ABI 에서 kissat 의 자료구조가 깨진다** (막힘)

`-O0` 빌드가 kissat 자신의 assertion 에서 멈춥니다.

```
Assertion failed: sizeof (watch) == sizeof (unsigned), file inline.h, line 79
```

`watch` union 의 bitfield 가 GNU 배치를 전제합니다. MSVC ABI 는 bitfield 의 기반 타입이 바뀔 때 새 저장 단위를 잡으므로 4바이트를 넘깁니다. `-DNDEBUG` 빌드는 assertion 이 없으니 그대로 풀이에 들어가 SIGSEGV 로 죽습니다.

`-mno-ms-bitfields` 는 길이 막혀 있습니다. Windows SDK 헤더가 `error: Itanium-compatible layout for the Microsoft C++ ABI is not yet supported` 로 거부합니다.

### 마찰 4. **MinGW 빌드는 서고 돌지만 답이 틀리다** (막힘, 더 나쁨)

`x86_64-w64-mingw32-gcc 13`(GNU bitfield 배치)으로는 93개 중 91개가 그대로 컴파일되고, 나머지 둘은 `sys/resource.h` 와 `SIGALRM`/`_SC_PAGESIZE` 만 채우면 됩니다. `-static` 링크도 됩니다.

그런데 **명백히 UNSAT 인 식에 SATISFIABLE 을 냅니다.**

```
p cnf 2 4
1 2 0
-1 2 0
1 -2 0
-1 -2 0
```

네 절이 두 변수의 네 배정을 전부 막으므로 UNSAT 입니다. MinGW 빌드는 `s SATISFIABLE` 과 `v 1 2 0` 을 냅니다. `v 1 2 0` 은 네 번째 절을 만족시키지 않습니다.

**이것은 shim 문제가 아니라 정답 문제입니다.** 원인은 아직 특정하지 않았습니다.

### 판단

**두 Windows 빌드 모두 신뢰할 수 없습니다.** 하나는 죽고 하나는 틀립니다. 틀린 SAT 답을 내는 solver 위에 `checked_proof=true` 를 올리는 것은 이 워크스트림이 존재하는 이유와 정반대입니다. checker 가 UNSAT proof 는 잡아주지만 **틀린 SAT 답은 checker 가 잡지 않습니다.** replay 가 잡지만, 그 전에 이미 UNSAT 였어야 할 질문을 SAT 로 답한 solver 를 우리가 신뢰 사슬에 넣었다는 뜻입니다.

그래서 벤더링을 멈추고 조율자에게 물었습니다. 선택지는 이렇게 봅니다.

1. 원인 규명을 계속한다. MinGW 오답이 내 shim(특히 `getrusage`/`sysconf` 대체) 탓인지 kissat 자체의 Windows 이식성 문제인지 좁힌다. 가장 정직하지만 시간이 얼마나 들지 모릅니다
2. **CaDiCaL 로 바꾼다.** C++11 이고 native LRAT(`--lrat`)라 drat-trim 이 사슬에서 빠집니다. TCB 는 여전히 `lrat-check` 한 파일입니다. Windows 이식성은 다시 재야 합니다
3. Bitwuzla 처럼 **공식 Windows 바이너리를 pin 한다.** kissat 에는 공식 Windows 릴리스가 없어 이 경로는 kissat 에 대해 닫혀 있습니다
4. Linux 에서만 `prove.aig-sat` 를 제공하고 Windows 에서는 method 를 등록하지 않는다. 조율자 조건 3("Windows 빌드가 필수")과 어긋나므로 조율자 결정이 필요합니다

## 다음에 할 것

### 1. 범용 프로세스 실행기 (선행 부품)

`prove.aig-sat` 를 막는 유일한 부품입니다. `src/solver.c` 의 프로세스 계층은 Bitwuzla 전용이고(`ql_solver_add_smt2`, `ql_solver_check`) DIMACS 파일을 인자로 받는 실행 파일을 돌릴 수 없습니다. 공개 헤더에도 범용 실행기가 없습니다.

`src/proof_aigsat.c` 안에 libuv 기반으로 다음을 만듭니다. `src/solver.c` 의 규율을 그대로 따릅니다.

- shell 없이 argument vector 로 실행, 절대 경로
- snapshot 디렉터리에 CNF 와 LRAT 를 쓰고 끝나면 정리
- deadline 과 stdout/stderr 상한. 초과는 `UNKNOWN`
- 실행 파일 내용의 BLAKE3 digest 를 evidence 에 기록

`src/solver.c` 는 W2 소유라 거기서 실행기를 꺼내 공유하려면 조율자 조정이 필요합니다. 중복을 피하려면 그쪽이 낫고, 빨리 가려면 `proof_aigsat.c` 안에 두는 쪽입니다. **조율자에게 물을 것.**

### 2. `prove.aig-sat` method 본체 (3, 4단계)

흐름은 이렇습니다.

1. problem v2 -> 양쪽 lowering -> `ql_product_query_build`
2. `ql_aig_blast_smt2([prefix, violation])` -> `ql_aig_cnf_create` -> DIMACS
3. CaDiCaL 을 `--lrat --no-binary` 로 실행
4. `s SATISFIABLE` 이면 DIMACS 변수를 `ql_aig_blast_symbol_*` 로 되짚어 solver-model 문법 텍스트를 만들고 `ql_replay_decode_model` + `ql_replay_execute`. **replay 가 위반을 재현할 때만** `COUNTEREXAMPLE`
5. `s UNSATISFIABLE` 이면 (가) `lrat-check` 가 원본 CNF 와 LRAT 에 대해 통과하고 (나) domain query(`[prefix, domain]`)를 같은 방식으로 blast 해서 SAT 여야 vacuous 가 아니며 (다) `ql_problem_require_proof_binding` 이 통과할 때만 `PROVED_*` 와 **`checked_proof=true`**
6. envelope 에 CaDiCaL 과 `lrat-check` 두 실행 파일의 BLAKE3 digest, 그리고 **SMT 경로와 같은 query digest**(`prefix_digest`/`violation_digest`)를 기록 (조건 d)

`ql_aig_cnf_get_view` 의 `trivially_true` / `trivially_false` 는 solver 를 부르지 않고 처리하되 **checked proof 가 아닙니다.**

### 3. Bitwuzla 대 CaDiCaL 일치 시험 (조건 e)

같은 query bytes 에 대해 두 backend 의 SAT/UNSAT 이 일치하는지 고정합니다. 불일치는 다수결하지 않고 상태 오류로 크게 표면화합니다. 불일치는 어느 한쪽 backend 또는 blaster 의 버그 증거입니다.
