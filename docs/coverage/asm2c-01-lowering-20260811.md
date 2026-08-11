# asm2c-01 전체 lowering 재측정

2026-08-11에 `asm2c-01`의 실제 train 데이터셋 전체를 WSL에서 준비한
Linux Clang 바이너리로 다시 측정했습니다. residual 통과의 합이 아니라 최종
바이너리 하나가 1,204,532개 정의를 전부 처리한 결과를 정본으로 삼습니다.

## 결과

| 지표 | 기준선 `07f8774` | 최종 작업 트리 |
|---|---:|---:|
| unit | 1,204,715 | 1,204,715 |
| C 정의 | 1,204,532 | 1,204,532 |
| frontend supported | 1,204,432 | 1,204,432 |
| lowered and verified | 1,037,460 | 1,181,212 |
| lowering 성공률 | 86.1297% | 98.0640% |
| verifier 실패 | 0 | 0 |
| status 오류 | 219 | 256 |

순증은 143,752개 정의와 11.9343%p입니다. 기준선 UNKNOWN 중 143,981개가
lowered로 바뀌었고, 기준선 status 오류 한 개는 명시적 UNKNOWN으로 바뀌었습니다.

기준선에서 lowered였던 229개는 최종 코드에서 명시적 UNKNOWN이 되었습니다.
이는 더 많은 객체를 발견한 뒤 bounded object constraint 한계를 적용한 104개,
유효한 object-table identity가 없는 포인터 41개, 불완전 record 32개, bounded
aggregate slice보다 큰 zero initialization 25개, loop-carried pointer authority
19개, 지원하지 않는 string decoding 5개, 서로 다른 record identity의 복사
3개입니다. 이들을 예전의 좁은 모델로 계속 수용하는 것을 성공으로 세지
않았습니다.

최종 status 오류 256개 중 218개는 기준선에서도 status 오류였습니다. 나머지
38개는 예전에는 더 앞선 unsupported 진단에서 멈췄지만 이번 확장으로 더 깊은
기존 lowering 오류에 도달한 표본입니다. 기준선에서 성공하던 정의가 status
오류로 바뀐 경우는 없습니다. 따라서 98.0640%는 오류나 verifier 실패를 성공에
넣지 않은 수치이며, G9의 99% 목표는 아직 달성하지 않았습니다.

## 이번 전수 확인에서 고친 오류

exact pointer identity가 현재 object table 밖을 가리키면 access predicate가
내부 오류를 냈습니다. 더구나 low-level에서 UNKNOWN만 설정하면 호출자가 일부
IR을 계속 만들어 verifier 오류로 끝날 수 있었습니다. 이제 object access 전에
identity 범위를 검사하고 `unsupported_pointer` UNKNOWN으로 종료합니다. 실제
corpus 재현을 `CLower.RejectsUnmodeledSemanticSurfacesAsUnknown`에 넣었습니다.

같은 작업 범위에는 다음 정확성 수정도 포함됩니다.

- 같은 이름의 record tag를 file scope와 block scope에서 하나로 합치던 문제를
  lexical scope와 declaration point에 따라 서로 다른 identity로 분리했습니다.
- 주소를 취한 미초기화 scalar를 alias로 읽을 때 `defined` 값이 아직 IR에
  materialize되지 않아 invalid operand가 생기던 문제를 고쳤습니다.
- character array를 string literal로 초기화할 때 불필요한 별도 string object를
  만들던 중복을 제거했습니다.
- invalid IR value ID가 builder까지 흐르면 원인을 잃는 경로에 방어 검사를
  추가했습니다.

## 재현

측정 VM은 Ubuntu 22.04 계열의 Linux 6.8 x86-64이며 online CPU는 224개입니다.
최종 전수 명령은 다음과 같습니다.

```sh
cd /opt/asm2c/checks/quodlibet-lowering-20260811-07f8774/src
/usr/bin/time -v env QL_EXE=out/build/linux-clang/quodlibet \
  bash scripts/coverage-parallel.sh \
  ../corpus/train/units.txt \
  ../results/final-full-fixed-detail.tsv \
  224 > ../results/final-full-fixed.json \
  2> ../results/final-full-fixed.stderr
```

18,824개 batch를 처리했고 wall time은 64.41초, 최대 RSS는 507,200 KiB였습니다.
JSON의 SHA-256은
`6b761ac94c74d70801a3ec9e75e24f26de7e310640c5168e929f80d0a5e03d67`입니다.
detail TSV는 정확히 1,204,532행입니다.

이 측정은 proof가 아니며 bounded unrolling을 사용하지 않습니다.
