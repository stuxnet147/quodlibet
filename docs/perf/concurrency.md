# 동시성과 취소 비용

W7 항목 3 의 측정 기록입니다. `docs/perf/baseline.md` 는 W8 이 소유하고 hotspots 와 worker occupancy 를 담습니다. 이 문서는 그것이 다루지 않는 축, **예산 guard 가 걸려 있을 때의 비용과 예산이 만료된 뒤 실제로 포기하기까지의 지연**을 담습니다.

취소가 묻는 것이 바로 이것입니다. 공개 파이썬 표면은 cancel token 이 아니라 예산을 노출하고, 코어는 예산이 소진된 실행을 취소된 실행과 **같은 경로로** UNKNOWN 으로 회수합니다. 그래서 예산이 취소를 관측할 수 있는 손잡이입니다.

## 측정 환경

- Windows 11, 24 논리 코어. 다른 워크스트림이 같은 기계에서 동시에 빌드하고 시험합니다.
- 커밋 `032c080`, `bitwuzla 0.9.1`, CPython 3.13 abi3 확장
- 워크로드: 교환법칙으로 동치인 `add`/`sum` 쌍. 쌍마다 항등항을 달리 넣어 digest 가 겹치지 않게 합니다.

**모든 행이 median 이 아니라 min/median/max 입니다.** 다른 작업이 도는 기계에서는 산포가 재려는 효과보다 넓은 경우가 잦고 median 하나는 바로 그것을 가립니다. 두 행의 범위가 겹치면 그 실행은 아무것도 재지 못한 것이며 그렇게 적습니다.

## 재현

```sh
cmake --build --preset windows-clang --parallel

PY=out/build/windows-clang/bindings/python/package
QL_BENCH_REPEATS=15 QL_BENCH_SECTIONS=giving-up \
    PYTHONPATH=$PY python -u scripts/perf/bench-concurrency.py 1 1

QL_BENCH_REPEATS=9 QL_BENCH_SECTIONS=arming,scaling \
    PYTHONPATH=$PY python -u scripts/perf/bench-concurrency.py 6 1,2,4,8
```

빌드가 찾은 인터프리터를 씁니다. abi3 확장은 3.11 태그로 만들지만 **빌드에 쓴 헤더보다 낮은 CPython 에서 돌리면 import 는 되고 호출에서 `SystemError` 로 실패합니다**(3.13 헤더로 만든 모듈을 3.11 에서 호출, 2026-08-10 실측). 지원하는 방향은 낮은 태그로 만들어 높은 인터프리터에서 쓰는 쪽입니다.

## 1. 포기하기까지의 지연 (repeats=15)

| 예산 | min | median | max | verdict | state |
|---|---|---|---|---|---|
| 1 ns | 0.0002s | 0.0002s | 0.0038s | unknown | total-time-exhausted |
| 1 ms | 0.393s | 0.461s | 0.663s | unknown | total-time-exhausted |
| 10 ms | 0.379s | 0.407s | 0.430s | unknown | total-time-exhausted |
| 100 ms | 0.386s | 0.410s | 0.555s | unknown | total-time-exhausted |
| 500 ms | 0.784s | 0.812s | 0.871s | unknown | total-time-exhausted |
| 없음 | 0.831s | 0.862s | 1.172s | **proved-equivalent** | active |

**지연이 마감에 비례하지 않고 단계 경계로 양자화됩니다.** 1 ms, 10 ms, 100 ms 세 마감이 전부 같은 ~0.41초에 돌아옵니다. 500 ms 는 ~0.81초입니다. 이 두 값은 이 워크로드의 두 solver 왕복(도메인 질의, 위반 질의)이 끝나는 지점과 맞고, 전체 실행이 0.86초입니다.

읽는 법은 이렇습니다. **예산 guard 는 pipeline 이 실제로 손을 쓸 수 있는 단계 경계에서 검사되지 solver 호출 안에서 검사되지 않습니다.** 그래서 마감과 복귀 사이 간격은 그때 돌던 단계의 길이만큼 벌어집니다. 1 ns 만 예외인데 그것은 아무 작업도 시작하기 전에 걸리기 때문이고 0.2 ms 에 돌아옵니다.

쌍당 마감을 거는 채점기에 이것이 뜻하는 바는 분명합니다. **100 ms 마감과 1 ms 마감은 같은 값이 듭니다.** 마감을 단계 길이보다 촘촘하게 주는 것은 지금 구조에서 효과가 없습니다.

**건전성은 흔들리지 않습니다.** 마감을 넘긴 실행도 전부 `unknown` + `total-time-exhausted` 이고 `proved-equivalent` 가 새어 나온 경우가 없습니다. 초과분은 wall clock 만 쓰고 판정을 오염시키지 않습니다.

## 2. guard 를 걸어 두는 비용 (repeats=9, 6쌍)

발동하지 않을 만큼 큰 예산(600초)과 예산 없음을 같은 배치로 비교합니다.

| workers | 예산 없음 min/med/max | 예산 있음 min/med/max | 범위 분리 |
|---|---|---|---|
| 1 | 5.117 / 5.436 / 5.771s | 5.140 / 5.288 / 5.641s | 아니오 |
| 2 | 2.630 / 3.387 / 6.750s | 2.668 / 4.124 / 8.008s | 아니오 |
| 4 | 2.103 / 2.465 / 4.318s | 2.053 / 2.852 / 5.747s | 아니오 |
| 8 | 1.406 / 1.654 / 7.872s | 1.196 / 1.742 / 4.161s | 아니오 |

**네 행 모두 범위가 겹칩니다. 이 실행은 arming 비용을 분해하지 못했습니다.** 비용이 없다는 뜻이 아니라 이 소음 수준에서는 보이지 않는다는 뜻입니다.

말할 수 있는 것은 workers=1 행뿐입니다. 거기서는 산포가 좁고(양쪽 ±6% 안) 두 median 이 5.44초와 5.29초로 사실상 같습니다. **직렬 실행에서 예산을 걸어 두는 것은 이 해상도에서 공짜입니다.** worker 수가 늘면 산포가 max 기준 5배까지 벌어지는데 이것은 guard 가 아니라 같은 기계에서 도는 다른 워크스트림의 빌드 때문입니다.

분해하려면 조용한 기계가 필요합니다. G7 의 전용 Linux VM 이 그 자리이고 거기서 다시 재는 것을 권합니다.

## 3. worker 수별 처리량 (repeats=9, 6쌍)

W8 의 `bench-batch.py` 가 소유한 축입니다. 같은 기계, 같은 커밋에서의 교차 확인으로만 싣습니다.

| workers | min | median | max | 쌍/초 | speedup |
|---|---|---|---|---|---|
| 1 | 5.937s | 7.650s | 14.443s | 0.78 | 1.00x |
| 2 | 3.810s | 5.455s | 11.205s | 1.10 | 1.40x |
| 4 | 2.041s | 2.465s | 5.698s | 2.43 | 3.10x |
| 8 | 1.221s | 1.777s | 2.433s | 3.38 | 4.31x |

8 worker 에서 4.31x 로 `baseline.md` 의 4.67x 와 같은 자리에 있습니다. workers=1 의 max 가 median 의 1.9배인 것이 기계 경합의 크기를 그대로 보여 줍니다.

## 남은 것

- **arming 비용은 미해결.** 조용한 기계에서 다시 재야 합니다.
- **취소 지연을 줄이려면** guard 를 solver 왕복 안으로 넣어야 합니다. `ql_solver_check_request_v1` 에 이미 `is_cancelled` 와 `cancel_state` 가 있으므로 배선의 문제이지 ABI 의 문제가 아닙니다. 다만 이것은 `src/solver.c` 와 파이프라인 쪽이라 W2 와 겹치므로 W7 이 손대지 않았습니다.
- **W8 의 세션 API** 가 `check_batch` 내부를 바꿉니다. 위 수치는 전부 `032c080` 기준이며 세션 API 전후 비교의 기준선으로 쓸 수 있습니다.
