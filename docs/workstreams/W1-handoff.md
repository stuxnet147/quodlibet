# W1 G9 핸드오프

작성 시점: 2026-08-11

## 목표와 완료 조건

사용자 요청은 Quodlibet의 G9를 완수한 뒤 Quodlibet 저장소의 `main`에 푸시하는 것입니다. `D:/projects/machine-model`의 asm2c 관련 파일은 건드리지 않습니다. G9 완료 주장은 다음을 모두 현재 상태에서 다시 확인한 뒤에만 합니다.

- train 프런트엔드 수용률 99% 이상
- train IR lowering `SUPPORTED` 99% 이상
- 모든 lowered IR verifier 통과
- status 실패 0
- 영향 범위에 맞는 회귀 시험 통과
- 최종 전체 train 재측정과 문서 갱신
- Quodlibet 변경만 커밋하고 `main` 푸시

현재 IR lowering은 **29,150 / 29,880, 97.56%**입니다. 99%에는 432개가 더 필요합니다. 아직 목표 미달이므로 푸시하지 않았습니다.

## Git 상태

현재 Quodlibet branch는 `main`, HEAD는 `cf71d15`입니다. 로컬 tracking ref `origin/main`은 `621e599`이며 로컬은 다음 9개 커밋 앞서 있습니다.

```text
cf71d15 Carry larger calls and pointer authority sets
2b44899 Bound large zeroed byte arrays separately
b24a1e2 Model block scope extern objects as globals
e456335 Keep parameter qualifiers off function returns
e5a6775 Resolve anonymous local records by syntax identity
ddbcd5f Carry nested state across forward gotos
70e8c05 Carry opaque records and nested goto state
d8a7fd1 Lower backward gotos through cyclic SSA
621e599 Preserve declared function designators
```

`621e599`까지는 이미 origin에 있고 그 뒤 8개가 미푸시입니다. 원격을 fetch해서 재확인한 상태는 아니므로 최종 push 직전에 remote tracking 상태를 확인합니다.

다음 파일은 사용자님의 별도 EGraph 작업입니다. 수정, 삭제, stage, commit하지 않습니다.

```text
 M CMakeLists.txt
 M include/quodlibet/quodlibet.h
 M src/builtins.c
?? include/quodlibet/egraph_method.h
?? src/egraph_method.c
?? tests/test_egraph_method.cpp
```

오늘 시작했던 `volatile` 구현 초안은 전부 되돌렸습니다. 따라서 위 사용자 파일 외에 미커밋 코드 변경은 없습니다. 이 핸드오프와 `W1-progress.md`만 오늘 추가한 문서 변경입니다.

## 현재 G9 측정

| 결과 | 건수 |
|---|---:|
| lowered | 29,150 |
| unknown | 730 |
| verifier 통과 | 29,150 / 29,150 |
| status 실패 | 0 |

첫 진단 코드 분포입니다.

| 진단 | 건수 |
|---|---:|
| `unsupported_call` | 224 |
| `unsupported_control_flow` | 123 |
| `unsupported_type` | 78 |
| `undeclared_identifier` | 68 |
| `duplicate_declaration` | 58 |
| `unsupported_volatile_or_atomic` | 50 |
| `unsupported_pointer` | 47 |
| `uninitialized_read` | 32 |
| `type_error` | 23 |
| `invalid_declaration` | 14 |
| `unsupported_expression` | 10 |
| `frontend_unsupported` | 3 |

세부 결과의 현재 세션 파일은 `/tmp/g9-current81-detail.tsv`입니다. `/tmp`는 영속 저장소가 아니므로 없으면 전체 train을 다시 측정하거나 `W1-progress.md` 79에서 81 단위의 기록으로 현재 수치를 복원합니다.

## 직전 완료 단위의 검증

`cf71d15`는 call operand 상한을 128로, 동적 pointer authority object 상한을 계측 최대 296보다 약간 큰 320으로 올렸습니다.

- 정확한 신규 경로 15개가 모두 lowered와 verifier 통과
- status 실패 0
- `CLowerCalls.*:CLowerPointers.*` 38/38 통과
- 대상 corpus 15개 측정 3.9초
- 공용 IR, interpreter, solver, plugin, EGraph, public ABI는 바뀌지 않아 전체 CTest를 실행하지 않음
- 신규 분기는 기존 거부 상한 뒤에만 있어 기존 성공이 탈 수 없으므로 전체 train을 실행하지 않음

## 다음 후보 1: `volatile`

train unit에서 `volatile`를 포함한 파일은 66개입니다.

| 현재 결과 | 건수 |
|---|---:|
| lowered | 9 |
| `unsupported_volatile_or_atomic` | 50 |
| `duplicate_declaration` | 6 |
| `unsupported_pointer` | 1 |

50개 모두 `volatile`이고 atomics는 없습니다. qualifier 거부만 임시로 제거한 의미론 미완성 ceiling 실험에서는 42개가 lowered와 verifier 통과, 7개가 control-flow, 1개가 call 제약으로 이동했습니다. 이 코드는 전부 되돌렸습니다.

현재 lowered 9개도 volatile cast qualifier를 버리는 조용한 의미 오류가 있으므로 66개 전체가 영향 집합입니다. 설계와 검증 계획은 `W1-progress.md`의 82절에 적었습니다. 핵심은 pointer layer별 qualifier 보존, scalar와 record storage, LOAD/STORE의 volatile effect, call과 공유하는 event trace, global initializer event 억제, interpreter의 `TRACE_APPEND` 실행입니다.

`volatile`만으로 얻을 수 있는 상한은 약 42개이므로 G9까지 남은 432개를 단독으로 닫지 못합니다. 구현 후 즉시 다음 큰 메시지 묶음으로 넘어가야 합니다.

## 다음 후보 2 이후

메시지별 큰 묶음은 다음과 같습니다.

- `unsupported_call` 224 중 202개: selected unit에 callee 선언이 없음. 선언을 추측해서는 안 됩니다.
- `unsupported_control_flow` 123 중 42개: goto가 initialization 또는 nested automatic state를 우회함.
- `unsupported_control_flow` 42개: loop 내부 VLA의 per-iteration lifetime.
- `unsupported_control_flow` 12개: nested state를 운반하는 loop goto의 cyclic SSA 결합.
- `unsupported_type` 78 중 47개: function local, multidimensional array, 접을 수 없는 array bound.
- `undeclared_identifier` 68, `duplicate_declaration` 58은 메시지별 재분류가 필요합니다.
- `uninitialized_read` 32개는 address가 callee로 escape할 수 있어 write contract 없이 성공 처리하면 안 됩니다.

`unsupported_call`의 나머지 non-identifier callee 19개는 단순 괄호 해제가 아닙니다. explicit function-pointer cast와 conditional function designator가 주류라 signature recovery와 indirect-call semantics가 필요합니다.

## 재개 순서

1. `git status --short`, `git log --oneline origin/main..HEAD`로 사용자 변경과 미푸시 커밋을 다시 확인합니다.
2. `W1-progress.md` 81, 82절과 이 파일을 읽습니다.
3. `/tmp/g9-current81-detail.tsv`가 있으면 29,880행과 29,150 lowered인지 확인합니다. 없으면 현재 HEAD에서 train을 재측정합니다.
4. `volatile`를 구현한다면 먼저 66개 영향 집합과 새 focused tests만 실행합니다.
5. `src/ir_interp.c`의 공용 `TRACE_APPEND` 실행을 바꾼 경우 focused tests가 통과한 뒤 전체 CTest를 실행합니다. 공용 실행기 영향 때문에 이때는 전체 CTest의 이유가 있습니다.
6. `volatile` 단위의 corpus 확인은 66개 전수로 충분합니다. 전체 train은 최종 G9 수치 또는 여러 진단 분기를 함께 바꾼 뒤 실행합니다.
7. 99%를 넘은 뒤 전체 train, verifier, status 0을 현재 커밋 상태에서 재확인하고 문서를 갱신합니다.
8. stage 목록에서 사용자 EGraph 파일이 빠졌는지 확인한 뒤 Quodlibet 변경만 커밋합니다.
9. 원격 상태를 확인하고 `main`에 push합니다.

## 테스트 원칙

오래 걸리는 시험 전에 항상 먼저 답합니다.

1. 현재 변경이 다른 subsystem이나 기존 성공 입력을 건드릴 수 있는가?
2. 아니라면 전체 시험이 어떤 추가 증거를 주는가?

영향 집합이 닫혀 있으면 exact filter와 그 corpus 전수가 우선입니다. 공용 또는 공개 경로를 바꾼 경우에만 전체 CTest를 실행합니다. 전체 train은 일반 회귀 시험이 아니라 G9 최종 수치와 기존 성공 회귀를 증명할 때 실행합니다. `ctest`는 반드시 정확한 `--gtest_filter` 또는 명시적 preset으로 실행하고 실수로 전체 시험을 시작하지 않습니다.
