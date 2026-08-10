# Quodlibet 작업 목록

이 문서는 저장소에 추적되는 작업 백로그입니다. 항목을 완료한 커밋에서 체크 표시와 관련 설명을 함께 갱신합니다. 세션별 임시 상태는 gitignored `handoff.md`에 기록합니다.

**종료 조건은 `GOAL.md` 가 갖습니다.** 이 문서는 그 조건을 닫기 위한 작업 목록입니다.

## 워크스트림 현황

갱신일: 2026-08-10 (통합 4회차, `main` 시험 239/239)

| 코드 | 이름 | GOAL | 상태 | 담당 |
|---|---|---|---|---|
| W1 | semantic-c-frontend | G5 전반, G8, G9 | 진행. 정확성 장치(verifier/interpreter/differential/fuzz)와 타입 확장 통합됨. 지금은 포인터 로어링(flat memory model 로 확정) | 병렬 세션 |
| W2 | exact-backend | G5 후반 | **지시서 완료.** WU1..WU6 전부 main 에. 세션 정리됨. IR 이 넓어지면 miter 확장 작업 단위를 새로 연다 | 완료 |
| W3 | runtime-services | G1, G2, G3 | **지시서 완료.** 세 GOAL 전부 닫힘. 세션 정리됨 | 완료 |
| W4 | python-bindings | G4 | **지시서 완료.** G4 닫힘. 세션 정리됨 | 완료 |
| W0 | 조율, 프로파일링, VM | G6, G7 | 진행. 다음은 VTune 기준선과 VM 확인 | 조율자 |

조율 규칙은 `GOAL.md` 의 "진행 규칙" 절입니다. `CMakeLists.txt`, `GOAL.md`, 이 표는 조율자가 소유합니다.

**2026-08-10 사용자 지시로 조율자는 브랜치 통합/푸시와 오케스트레이션만 합니다.** 코드, 튜닝, 측정은 전부 작업자 몫입니다. 작업자 상한은 10, 전원 Opus 5 medium 입니다. G6 은 W8 이, G7 은 W9 가 인수했습니다.

**닫힌 백로그: W6(증거 파이프라인), W7(안정성/배포, 원격 CI 재설계만 사용자 승인 대기).**

**닫힌 GOAL: G1, G2, G3, G4, G5(현 슬라이스 기준).** 열린 것: G6, G7(VM 검증 진행 중), G8, G9, 그리고 **W6/W7 백로그 전부**(2026-08-10 사용자 지시로 todo 전 항목이 완료 범위).

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

- [ ] 커버리지 측정으로 드러난 미수용 구문을 진단 코드별 빈도순으로 정리
- [ ] 포인터: 연산, object identity, provenance, 유효 범위, alignment 를 IR 로 하강
- [ ] 배열, 구조체, 공용체, 비트필드, enum
- [ ] 정수 promotion 과 usual arithmetic conversion 을 C11 6.3.1.8 대로
- [ ] 함수 호출과 외부 효과, 호출 순서 관찰
- [ ] 루프: 구조 보존 하강과 bounded unrolling
- [ ] `switch`, `goto`, 레이블, 중첩 제어 흐름
- [ ] 전역 변수와 정적 저장 기간
- [ ] `volatile`, `_Atomic`, I/O 관찰 의미론
- [ ] IR verifier 확장: 타입, SSA 지배관계, effect, UB guard
- [ ] IR concrete interpreter (G8 differential 시험의 기준)
- [ ] 새 C 구문 추가 절차 문서화와 그 절차를 따른 예시 커밋

## W2: 첫 exact backend

- [ ] versioned source-signature artifact (signedness, width, pointer/address-space, ABI 보존)
- [ ] problem schema v2 에 좌우 signature digest, 인자 대응, typed-precondition digest 결합
- [ ] problem v1 의 non-null precondition 으로는 `PROVED_*` 를 낼 수 없도록 gate 추가
- [ ] 좌우 loop-free scalar IR 을 결합하는 product program 또는 SMT miter
- [ ] relation 방향, UB policy, return/termination/trap 관찰을 SMT query 에 정확히 반영
- [ ] Bitwuzla SAT model 을 typed input 과 observable witness 로 decode
- [ ] SAT witness 를 별도 concrete semantic replay 로 검증한 뒤에만 `COUNTEREXAMPLE` 발행
- [ ] UNSAT 승격 경계를 checker 또는 명시적 trusted-backend policy 로 확정하고 코드가 강제
- [ ] `PROVED_*`, `COUNTEREXAMPLE`, `UNKNOWN` end-to-end 통합 시험
- [ ] proof method 를 registry 에 실제로 등록해서 `quodlibet methods` 에 나오게 한다

## W3: 런타임 서비스

- [ ] 외부 로깅 라이브러리 선정, 벤더링, SHA-256 고정, `DEPENDENCIES.md` 근거
- [ ] 공개 로깅 C API: 레벨, 상세도, 싱크, 카테고리
- [ ] 비활성 레벨 호출 오버헤드 측정과 기록
- [ ] 실행 예산: 전체 wall-clock, 노드별, solver 호출별
- [ ] 메모리 예산: 상한과 할당자 계측, 초과 시 결정적 실패
- [ ] 예산 초과가 논리 판정으로 새지 않도록 gate 와 시험
- [ ] 판정 정책 JSON 스키마와 파서
- [ ] 정책이 건전성 규율을 약화시킬 수 없도록 거부 규칙
- [ ] 사용자 정의 결과 직렬화와 왕복 시험

## W4: 파이썬 바인딩과 커버리지

- [ ] 커버리지 측정 도구: 코퍼스를 걸어 파서/로어링 수용률과 진단 분포를 낸다
- [ ] 측정 결과를 `docs/coverage/` 에 재현 명령과 함께 기록
- [ ] CPython C 확장 모듈 (`Py_LIMITED_API` abi3)
- [ ] solver 대기 중 GIL 해제
- [ ] 예산, 판정 정책, 결과의 파이썬 노출
- [ ] `pip install .` 과 Windows/Linux import 시험
- [ ] 강화학습기에서 부를 배치 API

## W0: 조율, 프로파일링, VM

- [ ] VTune hotspots 와 threading 프로파일, `docs/perf/`
- [ ] 프로파일 근거 최적화와 before/after 수치
- [ ] 성능 기준선 `docs/perf/baseline.md`
- [ ] asm2c-03 VM 에서 vendor/build/CTest 통과
- [ ] 채점기 경로 확인(쌍당 지연 수치)
- [ ] 강화학습기 경로 스모크(장시간 금지)
- [ ] VM 기록과 비용 `docs/vm/`

## W6: 증거와 method 파이프라인 (2026-08-10 범위 복귀)

사용자 지시로 이 문서의 전 항목이 GOAL 닫힘의 조건입니다. 아래는 원래 "뒤로 미룬 것"이었다가 복귀한 것입니다.

- [ ] e-graph merge log 의 독립 replay checker
- [ ] rewrite rule 별 soundness 조건과 side condition 을 versioned evidence 에 기록
- [ ] AIG/SAT backend 와 certificate checker
- [ ] concrete differential refutation method (등록되는 method 로)
- [ ] 여러 method 의 병렬 실행을 투표가 아니라 증거 우선 규칙으로 결합
- [ ] persistent artifact/evidence cache 저장소
- [ ] method/backend version, option, semantic contract 가 cache key 에 빠지지 않는지 통합 검증
- [ ] CHC/PDR 또는 loop invariant 연결 (루프 판정. W1 의 루프 로어링 뒤)

## W7: 성능, 안정성, 배포 (2026-08-10 범위 복귀)

- [ ] parser/lowering/e-graph/solver 단계별 benchmark 와 전체 latency 기준선 (일부는 docs/perf/baseline.md 에 있음)
- [ ] 병렬 worker 수, solver 동시성, cancellation overhead 측정
- [ ] artifact decoder 와 precondition 파서 퍼징 (파서/로어링/IR 디코더는 완료)
- [ ] solver crash, timeout, pipe 상속, corrupt model fault-injection 확대
- [ ] Windows 와 Linux 설치 패키지, relocatable Bitwuzla 탐색
- [ ] public ABI compatibility 시험과 plugin SDK 예제
- [ ] 지원 compiler/target matrix 를 asm2c dataset manifest 에서 자동 검증
- [ ] 원격 CI 재설계와 활성화 (**사용자 승인 필요.** 원문이 승인 조건부라 마지막에 여쭙는다)

scheduler 고도화와 plugin SDK "확장" 중 위 목록에 없는 것은 여전히 범위 밖입니다(G5 의 동결 원칙).

## 완료 판정 원칙

- `BOUNDED_CLEAN`은 어떤 경우에도 proof 완료로 세지 않습니다.
- raw solver `UNSAT`은 problem, contract, query, backend identity와 checker 경계가 결합되기 전에는 `PROVED_*`가 아닙니다.
- replay되지 않은 SAT model은 확정 counterexample이 아닙니다.
- 지원하지 않는 의미론을 좁게 해석해서 통과시키지 않고 `UNKNOWN` 또는 명시적 오류로 남깁니다.
- 커버리지 수치를 정확성보다 앞세우지 않습니다. 수용률이 오르면 같은 표본에서 differential 시험도 같이 돌립니다.
