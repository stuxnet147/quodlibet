# W8 진행 기록 (성능 튜닝, G6)

브랜치: `w8-performance`. 지시서: `docs/workstreams/W8.md`. 기준선: `docs/perf/baseline.md`.

## 지금 하는 것

WU1. 워크트리 부트스트랩. windows-clang 과 linux-clang 빌드를 세우고 기준선을 이 워크트리에서 재현합니다.

## 인수한 상태 (조율자로부터)

- Windows VTune 은 이 호스트에서 죽어 있습니다. 프로파일링은 WSL Ubuntu-24.04 의 VTune 으로 합니다.
- hotspots 기준선: val 1,050 유닛 0.380s CPU, tree-sitter 파싱 지배.
- 적용된 최적화 1건: coverage 경로 중복 파스 제거 (-27%).
- threading 리포트는 WSL 에서 수집이 걸립니다. W9 의 Linux VM 결과를 받아 씁니다.

## 환경 차이 (인수 시 확인)

조율자의 측정은 `D:/projects/machine-model/python/quodlibet` (main 워크트리) 에서 나왔습니다. 이 워크스트림은 `C:/Users/hkdc/orca/workspaces/quodlibet/w8-performance` 에서 돕니다. WSL 에서는 `/mnt/c/...` 로 보입니다. `scripts/coordinator/vtune-wsl.sh` 는 경로가 main 워크트리로 고정되어 있으므로 W8 이 소유한 재현 스크립트를 워크트리 상대 경로로 다시 씁니다.

`out/` 은 이 워크트리에 없으므로 코퍼스 추출부터 다시 합니다. 코퍼스는 `D:/projects/machine-model/datasets/records-local` 로 공유됩니다.

## 끝난 작업 단위

(아직 없음)

## 설계 결정

(아직 없음)

## 막힌 것

(아직 없음)

## 다음에 할 것

1. WU1: 양쪽 빌드 + val 코퍼스 추출 + 기준선 재현.
2. WU2: 프로파일 근거 최적화.
3. WU3: threading 리포트 (W9 경유).
4. WU4: 회귀 측정 하네스 고정.
5. WU5: 전 구성 CTest + ASan/UBSan.
