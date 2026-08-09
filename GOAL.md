# Quodlibet GOAL

작성일: 2026-08-10

이 문서는 **현재 위임된 목표와 그 종료 조건**을 담습니다. 무엇을 할지는 `todo.md`, 세션 인계는 `handoff.md`, 설계 계약은 `ARCHITECTURE.md`/`METHODS.md`/`SOLVERS.md`가 갖습니다.

**GOAL 은 G1..G9 가 전부 닫혀야 닫힙니다.** 하나라도 열려 있으면 GOAL 은 열린 것입니다. 종료 조건은 결과에 맞춰 고치지 않습니다. 조건을 바꿔야 한다면 사용자에게 먼저 말합니다.

## 전제

- Quodlibet 은 asm2c 전용 도구가 아니라 **범용 함수 동치 판정 프레임워크**로 먼저 만듭니다. asm2c 는 첫 소비자입니다.
- asm2c 쪽 문서(`DECISIONS.md`, `MILESTONES.md` 등)는 아직 이 리포지터리를 몰라도 됩니다. 연결은 나중입니다.
- 어려운 문제는 orchestration 이 아니라 **C semantics -> proof semantics 경계의 정확성**입니다. 개발력의 대부분이 여기에 갑니다.

---

## G1. 저부하 크로스플랫폼 로깅

**요구.** 저부하, 견고, 고속, 스레드 세이프. 로그 레벨과 상세도를 사용자가 설정. **직접 구현하지 않고 외부 라이브러리를 벤더링해서 배선한다.**

**종료 조건**

- [ ] 외부 로깅 라이브러리를 `third_party/` 에 SHA-256 고정으로 벤더링하고 `DEPENDENCIES.md` 에 선정 근거와 라이선스를 적었다
- [ ] 공개 C API 로 레벨(최소 trace..fatal)과 상세도(소스 위치, 스레드 id, 타임스탬프 정밀도, 카테고리)를 런타임에 설정할 수 있다
- [ ] 스레드 세이프하고, 여러 워커에서 동시에 기록해도 줄이 섞이지 않는다는 것을 시험이 고정한다
- [ ] **비활성 레벨 호출의 오버헤드를 측정해서 수치로 적었다** (호출당 ns). 추정이 아니라 측정이다
- [ ] 로깅 ON/OFF 두 구성 모두 CTest 전체 통과
- [ ] 공개 ABI 계약(`struct_size`, `abi_version`)을 깨지 않는다

## G2. 실행 예산과 메모리 예산

**요구.** 사용자가 실행 예산과 메모리 예산을 **디테일하게** 결정할 수 있어야 한다.

**종료 조건**

- [ ] 실행 예산: 전체 wall-clock, 파이프라인 노드별, solver 호출별 상한을 각각 설정할 수 있다
- [ ] 메모리 예산: 전체 상한과 할당자 계측(현재/최대 사용량)이 있고, 초과 시 할당이 결정적으로 실패한다
- [ ] 예산 초과는 **논리 판정이 아니라 상태**로 끝난다. 예산을 넘긴 실행이 `PROVED_*` 나 `COUNTEREXAMPLE` 을 내지 않는다
- [ ] 예산 초과 시 이미 만든 artifact 가 전부 해제된다(누수 없음, ASan/UBSan 통과)
- [ ] 시험이 각 축의 상한을 개별로 고정한다

## G3. 사용자 정의 검증 결과와 판정 규칙

**요구.** 검증 결과를 사용자가 JSON 으로 정의할 수 있어야 한다. 판정 규칙과도 관련이 있다.

**종료 조건**

- [ ] 버전이 있는 JSON 스키마로 **판정 정책**(어떤 증거가 어떤 판정을 정당화하는지)을 사용자가 정의할 수 있다
- [ ] 정책은 코어의 건전성 규율을 **약화시킬 수 없다**. `BOUNDED_CLEAN` 을 proof 로 올리는 정책, replay 하지 않은 SAT model 을 counterexample 로 올리는 정책은 파서가 거부한다
- [ ] 결과 직렬화도 사용자가 정의한 스키마를 따르고, 그 출력이 다시 파스된다(왕복 시험)
- [ ] 잘못된 정책 JSON 은 실행 전에 거부되고 오류 위치를 말한다
- [ ] 시험이 정책별 판정 차이를 고정한다

## G4. FFI 가 아닌 파이썬 바인딩

**요구.** ctypes/cffi 같은 FFI 가 아닌 방식으로, 크로스플랫폼 파이썬 바인딩. 나중에 강화학습기에서 부른다.

**종료 조건**

- [ ] **CPython C 확장 모듈**이다. ctypes, cffi, ABI 를 런타임에 재선언하는 방식이 아니다
- [ ] `Py_LIMITED_API` (abi3) 로 빌드해서 파이썬 마이너 버전마다 다시 빌드하지 않는다
- [ ] Windows 와 Linux 양쪽에서 import 되고 왕복 시험이 통과한다
- [ ] GIL 을 solver 대기 동안 놓는다(RL 루프에서 다른 스레드가 굶지 않는다)
- [ ] 예산, 판정 정책, 결과가 파이썬 쪽에서 전부 노출된다
- [ ] 빌드가 CMake 한 경로에 들어 있고 `pip install .` 이 된다

## G5. semantic C frontend + IR semantics + 첫 exact backend

**요구.** scheduler 와 plugin architecture 고도화를 멈추고 여기에 개발력을 넣는다.

**종료 조건**

- [ ] problem schema v2: 좌우 source-signature digest, 인자 대응, typed-precondition digest 결합
- [ ] 좌우 IR 을 결합하는 product program 또는 SMT miter
- [ ] relation 방향, UB policy, return/termination/trap 관찰이 SMT query 에 정확히 반영된다
- [ ] Bitwuzla SAT model 을 typed input 과 observable witness 로 decode
- [ ] **decode 한 witness 를 별도 concrete replay 로 검증한 뒤에만** `COUNTEREXAMPLE` 을 낸다
- [ ] UNSAT 을 proof 로 올리는 경계가 checker 이든 명시된 trusted-backend policy 이든 **문서에 적히고 코드가 그것을 강제한다**
- [ ] `PROVED_*`, `COUNTEREXAMPLE`, `UNKNOWN` 세 결과의 end-to-end 통합 시험이 있다
- [ ] scheduler/plugin 은 이 기간에 기능을 늘리지 않는다(회귀 수정만)

## G6. VTune 프로파일링과 극한 튜닝

**요구.** VTune 으로 세밀하게 프로파일링하고 최대한 속도를 뽑는다. 병렬로 돌아야 하는 부분을 확실히 잡는다.

**종료 조건**

- [ ] `docs/perf/` 에 VTune hotspots 와 threading 리포트가 있고 재현 명령이 적혀 있다
- [ ] **소거법이 아니라 프로파일 근거로** 고른 최적화가 before/after 수치와 함께 커밋되어 있다
- [ ] 병렬로 돌아야 하는 구간이 실제로 병렬로 돈다는 것이 threading 리포트로 확인된다(worker 점유율)
- [ ] 성능 기준선이 `docs/perf/baseline.md` 에 고정되고 회귀를 재는 방법이 적혀 있다
- [ ] 튜닝이 건전성을 바꾸지 않았다(전 구성 CTest 통과, ASan/UBSan 통과)

## G7. asm2c-03 VM 에서 채점기/강화학습기로 돌아가는지 확인

**요구.** RTX PRO 6000 x8 로 asm2c-03 을 임대해서 확인한다. **확인 수준으로 충분하며 장시간 학습/평가를 돌리지 않는다.**

**종료 조건**

- [ ] Linux VM 에서 vendor + build + CTest 전체가 통과한다
- [ ] 파이썬 확장이 그 VM 에서 import 되고 asm2c 코퍼스 표본에 대해 판정을 낸다
- [ ] **채점기 경로**: 표본 N 쌍에 대해 판정과 쌍당 지연을 측정한 수치가 있다
- [ ] **강화학습기 경로**: 배치 호출이 GPU 학습 루프와 같은 프로세스/머신에서 병행 가능함을 확인한다(짧은 스모크. 장시간 금지)
- [ ] VM 기록(생성/확인/삭제)과 비용이 `docs/vm/` 에 남는다. 확인이 끝나면 머신을 정리한다

## G8. 로어링과 IR 이 정확하고 빠르고 건전하며 확장에 유리함을 증명

**종료 조건**

- [ ] **정확성**: 로어링한 IR 을 concrete 하게 실행하는 인터프리터가 있고, 같은 입력에 대해 실제 컴파일 실행 결과와 일치한다(differential 시험, 무작위 입력 포함)
- [ ] **건전성**: IR verifier 가 타입, SSA 지배관계, effect, UB guard 를 검사하고 로어링 출력에 대해 항상 통과한다
- [ ] **퍼징**: 파서, 로어링, IR 디코더, precondition 파서에 퍼저를 걸고 크래시 0 을 확인한다
- [ ] **속도**: 로어링 처리량(함수/초)을 코퍼스에서 측정해 기록한다
- [ ] **확장성**: 새 C 구문 하나를 추가하는 절차가 문서에 있고, 그 절차대로 추가한 예가 커밋에 있다
- [ ] 지원하지 않는 의미론은 좁게 해석해 통과시키지 않고 `UNKNOWN` 으로 남는다

## G9. asm2c 데이터셋 99% 커버

**요구.** 현재 최신 asm2c 아키텍처가 쓰는 데이터셋을 파서와 IR 이 99% 이상 커버한다.

**측정 대상(2026-08-10 확인).** `D:/projects/machine-model/datasets/records-local` (레코드 198,432 / train 188,110 / val 6,232 / test 4,090, clang 및 clang-nopic, O0..O3, 프로젝트 146). 측정은 C 본문 단위이며 분모를 숨기지 않는다.

**종료 조건**

- [ ] 커버리지 측정 도구가 리포지터리에 있고 명령 한 줄로 재현된다
- [ ] **파서 수용률 >= 99%**
- [ ] **IR 로어링 수용률 >= 99%** (`SUPPORTED` 비율. `UNKNOWN` 은 미달로 센다)
- [ ] 미달분은 진단 코드별로 분류되어 남는다(무엇이 남았는지 말할 수 있어야 한다)
- [ ] 수용이 정확성을 대가로 얻어진 것이 아님을 G8 의 differential 시험이 같은 코퍼스 표본에서 보인다

---

## 진행 규칙

- **한 작업 단위가 끝나면 `main` 에 커밋하고 푸시한다.** 패스트포워드만, force 금지.
- 커밋 전 최소 검증은 `AGENTS.MD` 의 표를 따른다.
- 검증 안 된 것을 완료로 적지 않는다. 시험이 없으면 종료 조건이 안 닫힌다.
- 워크스트림이 병렬로 돌 때 `CMakeLists.txt` 와 이 문서는 조율자가 소유한다.

## 워크스트림

| 코드 | 이름 | 담는 GOAL | 주 파일 |
|---|---|---|---|
| W1 | semantic-c-frontend | G5(전반부), G8, G9 | `src/c_frontend.c`, `src/c_lower.c`, `src/ir.c` |
| W2 | exact-backend | G5(후반부) | `src/problem.c`, `src/solver.c`, 새 proof method |
| W3 | runtime-services | G1, G2, G3 | 새 `src/log.c`, `src/budget.c`, `src/policy.c` |
| W4 | bindings-and-coverage | G4, G9(측정) | 새 `python/`, `tools/coverage` |
| W0 | 조율 | G6, G7, 통합 | 조율자 직접 |

현재 상태와 담당은 `todo.md` 의 "워크스트림 현황" 절이 갖습니다.
