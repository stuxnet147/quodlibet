# W8 진행 기록 (성능 튜닝, G6)

브랜치: `w8-performance`. 지시서: `docs/workstreams/W8.md`. 기준선: `docs/perf/baseline.md`.

## 지금 하는 것

WU2. 이중 파스 제거를 W1 과 합의합니다. 그동안 소유한 영역의 측정과 문서를 마무리합니다.

## 인수한 상태 (조율자로부터)

- Windows VTune 은 이 호스트에서 죽어 있습니다. 프로파일링은 WSL Ubuntu-24.04 의 VTune 으로 합니다.
- hotspots 기준선: val 1,050 유닛 0.380s CPU, tree-sitter 파싱 지배.
- 적용된 최적화 1건: coverage 경로 중복 파스 제거 (-27%).
- threading 리포트는 WSL 에서 수집이 걸립니다. W9 의 Linux VM 결과를 받아 씁니다.

## 환경 차이 (인수 시 확인)

조율자의 측정은 `D:/projects/machine-model/python/quodlibet` (main 워크트리) 에서 나왔습니다. 이 워크스트림은 `C:/Users/hkdc/orca/workspaces/quodlibet/w8-performance` 에서 돕니다. WSL 에서는 `/mnt/c/...` 로 보입니다. `scripts/coordinator/vtune-wsl.sh` 는 경로가 main 워크트리로 고정되어 있으므로 W8 이 소유한 재현 스크립트를 워크트리 상대 경로로 다시 씁니다.

`out/` 은 이 워크트리에 없으므로 코퍼스 추출부터 다시 합니다. 코퍼스는 `D:/projects/machine-model/datasets/records-local` 로 공유됩니다.

## 끝난 작업 단위

### WU1. 측정 하네스와 호출 귀속 (커밋은 아래 push 기록)

- `scripts/perf/stage-corpus.sh`, `bench-coverage.sh`, `vtune-hotspots.sh`, `wsl.sh` 를 만들었습니다. 전부 워크트리 상대 경로입니다.
- `scripts/coordinator/vtune-wsl.sh` 는 `/mnt/d` 로 고정되어 있어 워크스트림 워크트리에서 안 돌았습니다. 새 스크립트로 넘기는 shim 으로 바꿨습니다.
- 기준선을 이 워크트리에서 재현했습니다: val 1,050 유닛 380 ms, 유닛당 0.3619 ms. 조율자 기록(0.394s / 0.362ms)과 일치합니다.
- **스택 수집을 켠 VTune 으로 호출 귀속을 확정했습니다.** 이것이 이번 작업 단위의 실제 성과입니다.

## 설계 결정

### 짧은 수집은 가지별 비율을 못 준다

0.38s 실행은 표본이 90개 남짓입니다. 첫 수집에서 로어링의 파스가 52%, 프런트엔드의 파스가 18% 로 나와 같은 일이 3배 비싼 것처럼 보였습니다. 유닛 목록을 10회 이어붙여 4.48s 로 늘리자 두 가지가 30.0% 와 31.7% 로 대칭이 되었습니다. **앞의 비대칭은 표본 부족이었습니다.** 유닛당 비용은 반복으로 바뀌지 않으므로 이 늘리기는 공짜입니다. `vtune-hotspots.sh` 의 기본 반복이 10인 이유입니다.

### 회귀 관문은 프로파일러를 요구하지 않는다

VTune 은 WSL 이나 VM 에서만 살아 있고 수집에 수십 초가 듭니다. 회귀를 매번 그걸로 재면 아무도 안 잽니다. `bench-coverage.sh` 는 wall-clock 5회 최소값만 재고 어디서든 돕니다. 대신 **coverage JSON 의 digest 를 같이 찍어서**, 빨라진 것과 덜 한 것을 구분합니다. digest 가 바뀌면 시간 비교는 무효입니다.

### 제거 가능한 몫은 46% 이고 파스 절반이 아니다

인수 문서는 "파싱 비용 절반" 을 말했지만 귀속을 보면 중복은 파스만이 아닙니다. 프런트엔드와 로어링이 각각 파서를 만들고, 각각 파싱하고, 각각 같은 방식으로 트리를 걸어 **필드가 글자 그대로 같은 노드 배열**(`ql_c_syntax_record` = `lower_node`)을 만들고, 각각 트리를 해제합니다. 파스 61.6% + 걷기 24.0% + 트리 해제 4.4% + 파서 생성 1.9% 중 절반이 중복이므로 약 46% 입니다. 실제 분석은 1.3%, 실제 로어링은 1.7% 입니다.

## 막힌 것

- **이중 파스 제거는 W1 소유 파일**(`src/c_frontend.c`, `src/c_lower.c`)입니다. 지시서대로 조율자 경유로 W1 과 인터페이스를 합의해야 합니다. 제안한 append-only API 는 `docs/perf/baseline.md` 에 적었습니다.
- **threading 리포트는 WSL 에서 안 뜹니다**(인수 시 실측). W9 의 Linux VM 결과가 필요합니다.

## 다음에 할 것

1. W1 과 `_with_tree` API 합의 (조율자 경유).
2. 합의되면 소비자(`cli/coverage.c`, 나중에 bindings) 이전 + before/after 측정.
3. 내가 소유한 영역의 작은 최적화: 측정 하네스의 파일 I/O 4.4%.
4. threading 리포트 (W9 경유).
5. 전 구성 CTest + ASan/UBSan.
