# Quodlibet 작업 목록

이 문서는 저장소에 추적되는 작업 백로그입니다. 항목을 완료한 커밋에서 체크 표시와 관련 설명을 함께 갱신합니다. 세션별 임시 상태는 gitignored `handoff.md`에 기록합니다.

## 현재 마일스톤

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

현재 기반 마일스톤은 약 95%이며, 실제 end-to-end 동치 판정기 전체 기준으로는 약 30%로 추정합니다. 이 수치는 작업량 추정이며 proof capability를 뜻하지 않습니다.

## P0: 최초의 sound end-to-end 판정

- [ ] versioned source-signature artifact를 정의하고 signedness, width, pointer/address-space, ABI 정보를 보존
- [ ] problem schema v2에 좌우 signature digest, 인자 대응, typed-precondition digest를 결합
- [ ] problem v1의 non-null precondition으로는 `PROVED_*`를 낼 수 없도록 gate 추가
- [ ] 좌우 loop-free scalar IR을 결합하는 product program 또는 SMT miter 구현
- [ ] relation 방향, UB policy, return/termination/trap 관찰을 SMT query에 정확히 반영
- [ ] Bitwuzla SAT model을 typed input과 observable witness로 decode
- [ ] SAT witness를 별도 concrete semantic replay로 검증한 뒤에만 `COUNTEREXAMPLE` 발행
- [ ] UNSAT 결과를 Quodlibet proof로 승격할 신뢰 가능한 checker 또는 명시적 trusted-backend policy 설계
- [ ] `PROVED_*`, `COUNTEREXAMPLE`, `UNKNOWN`의 end-to-end 통합 시험 추가

## P1: C 의미론과 관찰 범위 확장

- [ ] pointer 연산, object identity, provenance, 유효 범위와 alignment를 IR에 하강
- [ ] externally reachable final memory 비교 구현
- [ ] alias group과 disjoint precondition을 memory model에 연결
- [ ] globals, function calls, external-call event trace 구현
- [ ] volatile, atomic, I/O 관찰 의미론 구현
- [ ] 정수 promotion, enum, aggregate, union, bit-field 지원 범위를 명시하고 확대
- [ ] trap과 undefined behavior를 target profile에 맞춰 분리 검증
- [ ] loop bounded unrolling과 `BOUNDED_CLEAN` evidence 구현
- [ ] loop invariant, induction 또는 CHC/PDR method 연결

## P1: 증거와 method 파이프라인

- [ ] e-graph merge log의 독립 replay checker 구현
- [ ] rewrite rule별 soundness 조건과 side condition을 versioned evidence에 기록
- [ ] AIG/SAT backend와 certificate checker 검토 및 구현
- [ ] concrete differential refutation method 구현
- [ ] 여러 method의 병렬 실행을 투표가 아니라 증거 우선 규칙으로 결합
- [ ] persistent artifact/evidence cache 저장소 구현
- [ ] method/backend version, option, semantic contract가 cache key에 빠지지 않는지 통합 검증

## P2: 성능, 안정성, 배포

- [ ] parser/lowering/e-graph/solver 단계별 benchmark와 전체 latency 기준선 작성
- [ ] 병렬 worker 수, solver 동시성, cancellation overhead 측정
- [ ] parser, artifact decoder, IR verifier, precondition parser fuzzing
- [ ] solver crash, timeout, pipe 상속, corrupt model에 대한 fault-injection 확대
- [ ] Windows와 Linux 설치 패키지 및 relocatable Bitwuzla 탐색 방식 정리
- [ ] public ABI compatibility 시험과 plugin SDK 예제 추가
- [ ] 지원 compiler/target matrix를 asm2c dataset manifest에서 자동 검증
- [ ] 필요성이 생기면 사용자 승인 후 원격 CI를 새로 설계하고 활성화

## 완료 판정 원칙

- `BOUNDED_CLEAN`은 어떤 경우에도 proof 완료로 세지 않습니다.
- raw solver `UNSAT`은 problem, contract, query, backend identity와 checker 경계가 결합되기 전에는 `PROVED_*`가 아닙니다.
- replay되지 않은 SAT model은 확정 counterexample이 아닙니다.
- 지원하지 않는 의미론을 좁게 해석해서 통과시키지 않고 `UNKNOWN` 또는 명시적 오류로 남깁니다.
