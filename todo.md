# Quodlibet 작업 목록

이 문서는 저장소에 추적되는 작업 백로그입니다. 항목을 완료한 커밋에서 체크 표시와 관련 설명을 함께 갱신합니다. 세션별 임시 상태는 gitignored `handoff.md`에 기록합니다.

**종료 조건은 `GOAL.md` 가 갖습니다.** 이 문서는 그 조건을 닫기 위한 작업 목록입니다.

## 워크스트림 현황

갱신일: 2026-08-10 (G8 완료, Windows 511/511, Linux ASan+UBSan 510/510)

| 코드 | 이름 | GOAL | 상태 | 담당 |
|---|---|---|---|---|
| W1 | semantic-c-frontend | G5 전반, G8, G9 | G8 완료. G9의 포인터 provenance와 남은 C 로어링 범위 진행 | 병렬 세션 |
| W2 | exact-backend | G5 후반 | **지시서 완료.** WU1..WU6 전부 main 에. 세션 정리됨. IR 이 넓어지면 miter 확장 작업 단위를 새로 연다 | 완료 |
| W3 | runtime-services | G1, G2, G3 | **지시서 완료.** 세 GOAL 전부 닫힘. 세션 정리됨 | 완료 |
| W4 | python-bindings | G4 | **지시서 완료.** G4 닫힘. 세션 정리됨 | 완료 |
| W0 | 조율, 프로파일링, VM | G6, G7 | 진행. 다음은 VTune 기준선과 VM 확인 | 조율자 |

조율 규칙은 `GOAL.md` 의 "진행 규칙" 절입니다. `CMakeLists.txt`, `GOAL.md`, 이 표는 조율자가 소유합니다.

**2026-08-10 사용자 지시로 조율자는 브랜치 통합/푸시와 오케스트레이션만 합니다.** 코드, 튜닝, 측정은 전부 작업자 몫입니다. 작업자 상한은 10, 전원 Opus 5 medium 입니다. G6 은 W8 이, G7 은 W9 가 인수했습니다.

**닫힌 백로그: W6(증거 파이프라인), W7(안정성/배포, 원격 CI 재설계만 사용자 승인 대기).**

**닫힌 GOAL: G1부터 G8까지.** 열린 목표는 G9의 asm2c 데이터셋 99% 커버리지입니다.

## 완료한 기반 마일스톤

- [x] C17 공통 런타임, artifact, registry, plugin ABI
- [x] JSON DAG 파이프라인과 네이티브 병렬 스케줄러
- [x] 사용자 설정 가능한 relation, UB policy, observation, target profile
- [x] Tree-sitter C 구문 계층과 제한된 C 의미론 eligibility
- [x] 타입이 있는 SSA IR과 loop-free integer 및 `_Bool` 하강
- [x] 타입이 있는 precondition AST와 signature-bound digest
- [x] e-graph 기반 정규화 코어와 merge evidence
- [x] proof-method capability registry
- [x] Bitwuzla 0.9.1 process backend와 solver ABI
- [x] evidence envelope와 결정적 cache key
- [x] Windows 및 Linux 빌드, GTest와 CTest 검증
- [x] 원격 CI 비활성화

**scheduler 와 plugin architecture 는 이 기간에 기능을 늘리지 않습니다**(G5). 회귀 수정만 받습니다.

## W1: semantic C frontend 와 IR semantics

목표는 G9 의 99% 수용률과 G8 의 정확성 증명입니다. 수용률을 정확성보다 앞세우지 않습니다.

- [x] **`?:`, `&&`, `||` 의 피연산자를 실제 분기로 내린다** (증거: `lower_short_circuit_expression` 과 `lower_conditional_expression` 의 분기 경로, `tests/test_c_lower_recursion.cpp` 8개. 효과가 팔 안에서만 일어나는 것을 호출 횟수로 고정하고, 루프 조건은 반복당 정확히 한 번을 컴파일된 C 와 대조한다. train 26,219 -> 29,340, verifier 전수 통과. `docs/coverage/coverage-20260811-branching.md`)
- [ ] 재귀에 assume-guarantee 규칙을 준다 (로어링은 열렸고 miter 가 거부한다. 재귀 깊이 귀납이지만 종료 side condition 을 가정으로 기록해야 한다. 증거와 policy 까지 걸리는 별개 작업)
- [x] 직접 재귀 호출 로어링 (증거: `collect_callees` 가 `function_definition` 도 훑는다, `tests/test_c_lower_recursion.cpp` 6개, 컴파일된 같은 함수를 callee 명세로 준 differential 포함. 정의 항목은 호출 대상일 뿐이고 값으로 쓰면 거부된다)
- [x] 커버리지 측정으로 드러난 미수용 구문을 진단 코드별 빈도순으로 정리 (증거: `docs/coverage/coverage-20260811-buckets.md` 의 잔여표. 배열 선언 거부와 goto 거부는 한 메시지가 여러 형태를 덮고 있어서 갈랐고, `duplicate_declaration` 58 과 `undeclared_identifier` 33 은 clang 이 거부하는 C 라 로어링 공백이 아님을 확인)
- [x] 루프 안 VLA 에 반복별 객체 수명을 준다 (증거: 객체가 크기와 별도로 미치는 범위를 지니고 선언이 그것을 묶는다, `CLowerLocals.ChecksALoopVlaAgainstTheCurrentIterationsBound`. 가변 길이 배열 타입은 계속 거부)
- [x] 다차원 배열 (증거: `array_row_length` 가 타입과 감쇠한 포인터에 실린다, `CLowerLocals.AddressesATwoDimensionalArrayByRowThenElement`, `FillsATwoDimensionalArrayRowByRow`, `CLowerPointers.AdvancesARowThroughATwoDimensionalParameter`. 삼차원은 보폭이 둘이라 계속 거부)
- [x] 함수 포인터로의 캐스트와 그것을 통한 호출 (증거: `CLowerCalls.CallsThroughACastToAFunctionPointer`. 되돌렸던 원인은 초기 호출 스캔이 캐스트 지정자를 못 봐서 트레이스가 없던 것)
- [ ] 주소를 취한 이름이 서로 다른 저장 유형으로 풀린다 (12. 슬롯 표가 이름으로 키를 잡아서 같은 이름의 두 선언이 객체 하나를 나눠 쓴다. 선언마다 슬롯을 만들고 `add_variable` 이 선언 노드로 고른다)
- [ ] 포인터: 연산, object identity, provenance, 유효 범위, alignment 를 IR 로 하강
- [ ] 배열, 구조체, 공용체, 비트필드, enum
- [ ] 정수 promotion 과 usual arithmetic conversion 을 C11 6.3.1.8 대로
- [ ] 함수 호출과 외부 효과, 호출 순서 관찰
- [x] 루프: `for`/`while`/`do` 구조 보존 하강, 순환 SSA와 interpreter 실행. exact proof method는 W6에 별도 미완료
- [ ] `switch`, `goto`, 레이블, 중첩 제어 흐름
- [ ] 전역 변수와 정적 저장 기간
- [ ] `volatile`, `_Atomic`, I/O 관찰 의미론
- [x] IR verifier 확장: 타입, SSA 지배관계, effect, UB guard. train 로어링 8,755/8,755 통과
- [x] IR concrete interpreter와 실제 컴파일 C differential 시험. 무작위 입력 포함
- [x] 새 C 구문 추가 절차 문서화와 `sizeof(type)` 예시 커밋 `3f96cb7`

## W2: 첫 exact backend

- [x] versioned source-signature artifact (signedness, width, pointer/address-space, ABI 보존) (증거: `src/signature.c` 의 `quodlibet.source-signature` v1, `tests/test_signature.cpp`)
- [x] problem schema v2 에 좌우 signature digest, 인자 대응, typed-precondition digest 결합 (증거: `ql_problem_artifact_create_v2` in `src/problem.c`, `tests/test_problem.cpp:308`)
- [x] problem v1 의 non-null precondition 으로는 `PROVED_*` 를 낼 수 없도록 gate 추가 (증거: `ql_problem_require_proof_binding`, `tests/test_problem.cpp:339`, `SmtProductMethod.RefusesASchemaV1ProblemBeforeExecuting`)
- [x] 좌우 loop-free scalar IR 을 결합하는 product program 또는 SMT miter (증거: `src/product.c`, `tests/test_product.cpp`. GOAL.md G5 의 한정대로 loop-free 슬라이스 위에서이고, 메모리 확장은 `tests/test_proof_smt_memory.cpp`. 루프는 여전히 `UNKNOWN`)
- [x] relation 방향, UB policy, return/termination/trap 관찰을 SMT query 에 정확히 반영 (증거: `ProductMiter.UndefinedBehaviourDomainsSeparateTheTwoRefinementDirections`, `TrapAxisAloneDecidesADifferentTrapCode`, `TerminationAxisAloneDecidesADivergingSide`, `SmtProductMethod.DischargesRefinementDirectionsSeparately`)
- [x] Bitwuzla SAT model 을 typed input 과 observable witness 로 decode (증거: `src/replay.c` 의 `ql_replay_decode_model`, `Replay.DecodesASolverModelAndConfirmsTheViolation`)
- [x] SAT witness 를 별도 concrete semantic replay 로 검증한 뒤에만 `COUNTEREXAMPLE` 발행 (증거: `SmtProductMethod.EmitsAReplayedCounterexample`, `Replay.AModelThatDoesNotReproduceIsNotACounterexample`, GOAL.md G5)
- [x] UNSAT 승격 경계를 checker 또는 명시적 trusted-backend policy 로 확정하고 코드가 강제 (증거: `src/proof_smt.c` 의 여섯 조건과 항상 기록되는 `checked_proof=false`, `SmtProductMethod.ProvesEquivalenceOnlyUnderTheRecordedTrustPolicy`, `AdvertisesProofOnlyWhenTheTrustPolicyIsSelected`, `METHODS.md`)
- [x] `PROVED_*`, `COUNTEREXAMPLE`, `UNKNOWN` end-to-end 통합 시험 (증거: `tests/test_proof_smt.cpp` 9개, `AVacuousDomainNeverBecomesAProof`, `RunsThroughAPipelineSelectedByName`)
- [x] proof method 를 registry 에 실제로 등록해서 `quodlibet methods` 에 나오게 한다 (증거: `src/builtins.c:51` 의 `ql_register_smt_product_method`, `cli/main.c:198` 의 `methods`, `SmtProductMethod.RegistersAsANamedProofMethod`)

## W3: 런타임 서비스

- [x] 외부 로깅 라이브러리 선정, 벤더링, SHA-256 고정, `DEPENDENCIES.md` 근거 (증거: zf_log 0.4.1, `scripts/vendor.sh:148` SHA-256 pin, `third_party/zf_log`, `DEPENDENCIES.md:43`, GOAL.md G1)
- [x] 공개 로깅 C API: 레벨, 상세도, 싱크, 카테고리 (증거: `include/quodlibet/log.h`, `src/log.c`, `EveryVerbosityAxisIsIndependent`, `ConcurrentWritersNeverInterleaveALine`)
- [x] 비활성 레벨 호출 오버헤드 측정과 기록 (증거: 0.071 ns/호출, `docs/runtime-services/logging.md`, `tests/test_log_overhead.cpp`, GOAL.md G1)
- [x] 실행 예산: 전체 wall-clock, 노드별, solver 호출별 (증거: `src/budget.c`, `TotalWallClockAxisExpiresOnItsOwn`, `NodeAndSolverAxesAreSetSeparately`)
- [x] 메모리 예산: 상한과 할당자 계측, 초과 시 결정적 실패 (증거: `MemoryLimitFailsDeterministicallyAtTheSameAllocation`, `tests/test_budget.cpp` 21개, GOAL.md G2)
- [x] 예산 초과가 논리 판정으로 새지 않도록 gate 와 시험 (증거: `ql_budget_guard_outcome` 이 `PROVED_*`/`COUNTEREXAMPLE`/`BOUNDED_CLEAN` 을 `UNKNOWN` 으로 회수, `PipelineBudgetLeak`, GOAL.md G2)
- [x] 판정 정책 JSON 스키마와 파서 (증거: `include/quodlibet/policy.h` schema v1, `src/policy.c`, `tests/test_policy.cpp` 22개)
- [x] 정책이 건전성 규율을 약화시킬 수 없도록 거부 규칙 (증거: 파서의 네 문법 거부와 `ql_policy_evaluate` 의 증거 재강제, `tests/test_policy.cpp:298` 부근, `tests/test_fuzz_contracts.cpp` policy target, GOAL.md G3)
- [x] 사용자 정의 결과 직렬화와 왕복 시험 (증거: `PolicySerializationRoundTrips`, `ResultSerializationRoundTrips`, `GatedResultSerializationRoundTrips`, `tests/test_policy_canonical.cpp` 고정점 시험)

## W4: 파이썬 바인딩과 커버리지

- [x] 커버리지 측정 도구: 코퍼스를 걸어 파서/로어링 수용률과 진단 분포를 낸다 (증거: `cli/coverage.c` 와 `cli/main.c:178` 의 `quodlibet coverage`, `tools/corpus/extract.py`)
- [x] 측정 결과를 `docs/coverage/` 에 재현 명령과 함께 기록 (증거: `docs/coverage/baseline-20260810.md` 의 결과표와 "재현" 절 명령. 수용률 자체는 G9 의 종료 조건이고 아직 열려 있습니다)
- [x] CPython C 확장 모듈 (`Py_LIMITED_API` abi3) (증거: `bindings/python/src/quodlibet_module.c`, `Py_LIMITED_API=0x030B0000`, `tests/test_extension.py::test_it_is_built_against_the_stable_abi`, GOAL.md G4)
- [x] solver 대기 중 GIL 해제 (증거: 판정 전체가 `Py_BEGIN_ALLOW_THREADS` 안, `tests/test_concurrency.py::test_two_checks_are_in_flight_at_the_same_instant`, GOAL.md G4)
- [x] 예산, 판정 정책, 결과의 파이썬 노출 (증거: `budget=` 다섯 축, `policy_json=`, `result.verdict/.status/.evidence/.counterexample/.policy`, `bindings/python/tests/test_budget_and_policy.py`)
- [x] `pip install .` 과 Windows/Linux import 시험 (증거: `pip install ./bindings/python` abi3 wheel, Windows 3.11/3.13 과 WSL Ubuntu 24.04, CTest 항목 `quodlibet.python_bindings`, GOAL.md G4)
- [x] 강화학습기에서 부를 배치 API (증거: `bindings/python/quodlibet/__init__.py:489` 의 `check_batch` 와 워커별 `SolverSession`, `tests/test_concurrency.py:100`, VM 채점기 경로 측정 `docs/vm/asm2c-03-plan-20260810.md`)

## W0: 조율, 프로파일링, VM

- [ ] VTune hotspots 와 threading 프로파일, `docs/perf/` (hotspots 는 `docs/perf/hotspots-coverage-val.txt` 로 있으나 **threading 리포트 산출물이 없습니다.** 세 환경 모두 수집 실패로 `docs/perf/baseline.md` "아직 없는 것" 과 GOAL.md G7 이 미달성으로 기록. G6 은 사용자가 2026-08-10 에 "의도 충족" 으로 인정해 닫혔으므로, 남은 것은 작업이 아니라 이 항목을 그 판단대로 접을지의 결정입니다)
- [x] 프로파일 근거 최적화와 before/after 수치 (증거: GOAL.md G6, `docs/perf/baseline.md` "적용한 최적화" 여섯 건 - 중복 파스 -27%, BLAKE3 어셈블리 -52%, 세션 재사용 -55.6%, 트리 이전, 예산 마감 양자화, lseek -1.09%)
- [x] 성능 기준선 `docs/perf/baseline.md` (증거: 파일 존재, 측정 환경/기준선/회귀 관문 `scripts/perf/bench-coverage.sh` 와 재현 절 포함)
- [x] asm2c-03 VM 에서 vendor/build/CTest 통과 (증거: GOAL.md G7, `docs/vm/asm2c-03-plan-20260810.md` 재시도 결과 - ubuntu-2404 에서 ctest 266/266, pytest 28 passed)
- [x] 채점기 경로 확인(쌍당 지연 수치) (증거: GOAL.md G7 - 쌍당 median 4ms / p90 7ms, 219.6 pairs/s at workers=8, error 0, 100쌍. 근거는 `docs/vm/scorer_bench.py` 와 계획서 "정본 수치" 절)
- [x] 강화학습기 경로 스모크(장시간 금지) (증거: GOAL.md G7 - 8 GPU 전부하 병행 `check_batch` 완주. 단 CUDA context + workers=8 의 uv_spawn SEGV 는 재현표와 함께 기록되어 solver 쪽 별건으로 이관됨, `docs/vm/rl_smoke.py`)
- [x] VM 기록과 비용 `docs/vm/` (증거: `docs/vm/asm2c-03-plan-20260810.md` 의 생성/확인/삭제와 대여 시간 기록, 두 대여 모두 상한 안에 `instances delete` 확인, asm2c-01/02 불간섭)

## W6: 증거와 method 파이프라인 (2026-08-10 범위 복귀)

사용자 지시로 이 문서의 전 항목이 GOAL 닫힘의 조건입니다. 아래는 원래 "뒤로 미룬 것"이었다가 복귀한 것입니다.

- [x] e-graph merge log 의 독립 replay checker (증거: `src/egraph_check.c` 가 엔진을 부르지 않고 자체 union-find 로 재생, `tests/test_egraph_check.cpp` 15개, `JustifiesEveryMergeAWideSaturationProduces`, `ReportsEveryRejectionNotOnlyTheFirst`)
- [x] rewrite rule 별 soundness 조건과 side condition 을 versioned evidence 에 기록 (증거: `ql_egraph_rule_descriptor_v1` 과 36개 rule catalogue, `QL_EGRAPH_RULE_CATALOGUE_DIGEST_HEX`, `tests/test_egraph_rules.cpp` 13개, `DescribesEveryRewriteTheEngineRecords`)
- [x] e-graph 정규화를 등록되는 method 로 배선 (증거: `src/egraph_method.c` 의 `normalize.egraph`, `src/builtins.c:50` 등록, `tests/test_egraph_method.cpp` 3개. 파이프라인에서 `add x, 0` 이 사라지고 인터프리터 결과가 보존되는 것까지 고정. proof method 로는 등록되지 않는다. 현재 수용 범위와 opaque leaf 대체 규칙은 `METHODS.md` 의 "The shipped `normalize.egraph` method")
- [x] AIG/SAT backend 와 certificate checker (증거: `src/aig.c`, `src/proof_aigsat.c`, CaDiCaL `--lrat` + `third_party/drat-trim` 의 `lrat-check`, `tests/test_proof_aigsat.cpp` 의 `AProvedEquivalenceCarriesACheckedProof` 가 이 저장소 첫 `checked_proof=true` 를 고정. folded-false 는 승격하지 않고 `UNKNOWN`)
- [x] concrete differential refutation method (등록되는 method 로) (증거: `src/proof_diff.c` 의 `refute.concrete-differential`, `src/builtins.c:58` 등록, `tests/test_proof_diff.cpp` 13개. 아무것도 못 찾으면 `BOUNDED_CLEAN` 이 아니라 `UNKNOWN`)
- [x] 여러 method 의 병렬 실행을 투표가 아니라 증거 우선 규칙으로 결합 (증거: `src/combine.c`, `tests/test_combine.cpp` 24개, `ThreeAgreeingUncheckedProofsStillDecideNothing`, `OneCheckedProofOutweighsAnyNumberOfUncheckedDisagreements`)
- [x] persistent artifact/evidence cache 저장소 (증거: `src/cache.c` 의 BLAKE3 key 저장소, load 마다 두 digest 재검증, 검증 실패는 miss 가 아니라 거부, `tests/test_cache.cpp`)
- [x] method/backend version, option, semantic contract 가 cache key 에 빠지지 않는지 통합 검증 (증거: `tests/test_cache_key.cpp` 16개가 축을 하나씩 변주. backend 를 아무 데도 적지 않으면 두 빌드가 충돌한다는 것까지 고정)
- [ ] CHC/PDR 또는 loop invariant 연결 (루프 판정. W1 의 루프 로어링 뒤) (구현이 없습니다. `include/quodlibet/proof_method.h:19` 의 family 상수와 `METHODS.md:625` 의 권고 절만 있고 `prove.chc-pdr` method 도 시험도 없습니다. `src/c_lower.c:6135` 대로 루프는 아직 로어링되지 않습니다)

## W7: 성능, 안정성, 배포 (2026-08-10 범위 복귀)

- [ ] parser/lowering/e-graph/solver 단계별 benchmark 와 전체 latency 기준선 (일부는 docs/perf/baseline.md 에 있음) (파스+프런트엔드+로어링 총량과 배치 latency 는 `scripts/perf/bench-coverage.sh` 와 `bench-batch.py` 로 있으나 **e-graph 단계와 prove 경로의 단계 분해가 없습니다.** `docs/perf/baseline.md` "아직 없는 것" 이 판정당 87ms 가 프런트엔드/로어링/miter/SMT-LIB 직렬화/solver 중 어디로 가는지 못 쟀다고 적고 있습니다)
- [x] 병렬 worker 수, solver 동시성, cancellation overhead 측정 (증거: `docs/perf/concurrency.md` 와 `scripts/perf/bench-concurrency.py` 의 취소 지연표, `docs/perf/baseline.md` "확장이 어디서 멈추는지" 의 worker 사다리. 취소 지연이 마감이 아니라 단계 경계로 양자화된다는 결과 포함)
- [x] artifact decoder 와 precondition 파서 퍼징 (파서/로어링/IR 디코더는 완료) (증거: `tests/fuzz/fuzz_precondition.c`, `fuzz_problem_decoder.c`, `fuzz_signature_decoder.c`, `fuzz_policy.c`, `fuzz_policy_result.c`, `tests/fuzz/fuzz_contract_targets.h`, 매 ctest 에서 도는 `tests/test_fuzz_contracts.cpp` 6개)
- [x] solver crash, timeout, pipe 상속, corrupt model fault-injection 확대 (증거: `tests/test_fault_injection.cpp` - 거짓말하는 backend 16가지와 프로세스 고장 9가지, 할당 계수기로 오류 경로 누수까지 고정)
- [x] Windows 와 Linux 설치 패키지, relocatable Bitwuzla 탐색 (증거: `scripts/check-install.sh` 9개 검사가 Windows 와 WSL Ubuntu 24.04 양쪽 통과, `cmake/quodlibet-config.cmake.in` 의 `find_package(quodlibet)`, `docs/deploy/packaging.md`. 이동 후 탐색은 음성 검사로 확정)
- [x] public ABI compatibility 시험과 plugin SDK 예제 (증거: `tests/test_abi_compat.cpp` 6개가 옛 헤더 caller 수용/작은 prefix 거부/필드 순서 `static_assert` 를 고정, `examples/plugin/` 과 `tests/test_example_plugin.cpp` 5개)
- [x] 지원 compiler/target matrix 를 asm2c dataset manifest 에서 자동 검증 (증거: `tests/test_target_matrix.cpp` 가 manifest 와 `ql_semantic_contract_init` 의 선언을 대조하고 drift 를 양방향으로 보고. 데이터셋이 없으면 통과가 아니라 skip)
- [ ] 원격 CI 재설계와 활성화 (**사용자 승인 필요.** 원문이 승인 조건부라 마지막에 여쭙는다) (승인 기록이 없습니다. `.github/` 도 CI 설정도 저장소에 없고 W7 지시서가 명시적으로 범위 밖으로 두었습니다. 작업이 아니라 사용자 결정 대기 항목입니다)

scheduler 고도화와 plugin SDK "확장" 중 위 목록에 없는 것은 여전히 범위 밖입니다(G5 의 동결 원칙).

## 완료 판정 원칙

- `BOUNDED_CLEAN`은 어떤 경우에도 proof 완료로 세지 않습니다.
- raw solver `UNSAT`은 problem, contract, query, backend identity와 checker 경계가 결합되기 전에는 `PROVED_*`가 아닙니다.
- replay되지 않은 SAT model은 확정 counterexample이 아닙니다.
- 지원하지 않는 의미론을 좁게 해석해서 통과시키지 않고 `UNKNOWN` 또는 명시적 오류로 남깁니다.
- 커버리지 수치를 정확성보다 앞세우지 않습니다. 수용률이 오르면 같은 표본에서 differential 시험도 같이 돌립니다.
