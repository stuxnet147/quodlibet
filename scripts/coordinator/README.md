# 조율자 스크립트

병렬 워크스트림을 조율하는 세션이 반복해서 쓰는 명령들입니다. Windows Git Bash 전제이고, Orca CLI 와 (프로파일링은) WSL Ubuntu-24.04 를 씁니다. 전부 리포지터리 루트에서 부릅니다.

| 스크립트 | 하는 것 |
|---|---|
| `inbox.sh` | orchestration 인박스를 읽어 사람이 읽을 모양으로 낸다. `--ack <deliveryId>` 로 확인 처리, `--peek` 로 소비 없이 열람 |
| `integrate.sh <원격브랜치>` | fetch, main 위로 rebase, ff 병합, 빌드와 CTest, 전부 통과하면 push. 어느 단계든 실패하면 그 자리에서 멈춘다 |
| `coverage-val.sh` | val 커버리지를 돌리고 요약과 첫 차단 사유 분포를 낸다. `train` 인자로 train 스플릿 |
| `vtune-wsl.sh [워크로드...]` | WSL VTune hotspots 수집. 기본 워크로드는 val 커버리지. **호출자가 백그라운드 + 120초 제한을 걸어야 한다** |
| `spawn-worker.sh <이름> <지시서경로>` | 워크트리 생성, Opus 5 medium bypass 로 Claude 기동, task 생성과 dispatch 주입까지 |

## 규약

- 이 스크립트들은 조율자 전용입니다. 워크스트림 세션은 부르지 않습니다.
- `integrate.sh` 는 `main` 에 ff 만 합니다. force 경로가 아예 없습니다.
- 프로파일링 계열은 항상 볼 것만 좁혀서 백그라운드로 돌리고 120초를 넘기면 끊습니다. 백그라운드 작업의 timeout 인자는 강제되지 않으므로 넘긴 것은 직접 끊어야 합니다.
- Orca 리포 id 같은 세션 상수는 `env.sh` 가 갖습니다. 환경이 바뀌면 그 파일만 고칩니다.
