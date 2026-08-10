# G8 완료 감사

2026-08-10 현재 G8의 여섯 종료 조건을 현재 메인 소스와 실행 결과에
대응시킨 기록입니다. 수용률 자체는 G9의 별도 목표이며, G8은
`SUPPORTED`라고 말한 범위의 정확성·건전성과 확장 규율을 증명합니다.

## 종료 조건별 증거

| 조건 | 구현 증거 | 실행 증거 | 결과 |
| --- | --- | --- | --- |
| 정확성 | `src/ir_interp.c`, `tests/test_ir_differential.cpp`와 C 로어링별 differential 시험 | 실제 컴파일 C와 경계값 및 사례당 2,000개 무작위 입력 비교. `sizeof(type)` 포함 expression 사례는 512개 무작위 입력 비교 | 완료 |
| 건전성 | 독립 reader 기반 `src/ir_verify.c`가 타입, SSA dominance, effect, UB guard를 재검사 | Windows 511/511, Linux ASan+UBSan 510/510. val 365/365와 train 8,755/8,755 verifier 통과 | 완료 |
| 퍼징 | `tests/fuzz/fuzz_*.c` 9개와 실패 전파가 보정된 `fuzz-campaign.sh` | ASan+UBSan에서 대상당 60초, 9개 실패 0. 마지막 C 로어링 변경 후 해당 target 추가 60초 실패 0 | 완료 |
| 속도 | `bench-coverage.sh`가 실제 함수 수와 verifier 포함 wall time 측정 | val 1,155.12 함수/초, train 1,549.23 함수/초 | 완료 |
| 확장성 | `docs/lowering/adding-a-construct.md`의 7단계 계약 | 계약을 구현·시험·커버리지와 한 단위로 적용한 `sizeof(type)` 커밋 `3f96cb7` | 완료 |
| 보수적 경계 | 로어링 catch-all과 구문별 구체 진단, `QL_C_LOWER_UNKNOWN` | `RefusesWhatItCannotState`, fuzz 검증 관문, 코퍼스 status 실패 0. 미지원 `sizeof` 이웃도 `UNKNOWN` | 완료 |

## 정확성 범위

`IrDifferential.MatchesCompiledExecutionOnRandomInputs`는 테스트에 실제로
컴파일된 C 함수와 concrete IR interpreter를 사례당 2,000개 입력으로
비교합니다. 부분 연산은 정의된 입력의 값 비교와 정의되지 않은 입력의
`UNDEFINED_BEHAVIOR` 확인을 분리하며, 두 경계를 모두 밟지 못하면 시험이
실패합니다.

포인터, 지역 메모리, 배열·record, 전역 상태와 선언된 call은 각각의
`MatchesCompiledExecution...` 시험에서 값 외에 최종 메모리와 call trace까지
비교합니다. 새 `sizeof(type)` 사례도 같은 소스 매크로로 컴파일 함수와
로어링 입력을 함께 만들어 둘이 갈라지지 않게 했습니다.

## 건전성과 보수적 경계

verifier는 builder와 decoder의 구현을 재사용하지 않고 공개 IR reader만으로
타입 규칙, dominator, effect bit, partial operation과 지배하는 `UB_GUARD`를
다시 계산합니다. `coverage`는 이제 모든 `SUPPORTED` artifact를 열고 verifier에
통과시키며, 하나라도 실패하면 비영 종료합니다.

현재 train의 29,880개 함수 중 8,755개만 `SUPPORTED`입니다. 나머지를 성공으로
간주하지 않으며 포인터 provenance, 루프, 미지원 타입·제어 흐름·표현식 등은
진단이 붙은 `UNKNOWN`으로 남습니다. 따라서 이 완료 표시는 G9의 99% 커버리지
완료를 뜻하지 않습니다.

## 재현 자료

- 정확성·코퍼스 verifier: `docs/coverage/coverage-20260810-g8-sizeof.md`
- 퍼징: `docs/fuzz/campaign-20260810.md`
- 처리량: `docs/perf/baseline.md`의 G8 코퍼스 처리량 절
- 확장 절차: `docs/lowering/adding-a-construct.md`
