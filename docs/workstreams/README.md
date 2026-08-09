# 워크스트림 공통 규칙

이 디렉터리는 병렬로 도는 작업 세션의 지시서를 담습니다. 각 세션은 자기 `W<번호>.md` 를 먼저 읽고 이 문서의 규칙을 그대로 따릅니다.

**세션이 끊겨도 재개 가능해야 합니다.** 그래서 지시서와 진행 기록이 전부 추적되는 파일에 있습니다.

## 시작할 때 읽는 것

순서대로 읽습니다. 건너뛰지 않습니다.

1. `AGENTS.MD` - 저장소 전체 규칙
2. `CLAUDE.MD` - Claude 계열 작업자 요약
3. `GOAL.md` - 지금 닫아야 하는 목표와 종료 조건
4. `todo.md` - 작업 목록과 워크스트림 현황
5. 자기 워크스트림의 `docs/workstreams/W<번호>.md`
6. 자기가 건드릴 영역의 설계 문서 (`ARCHITECTURE.md`, `METHODS.md`, `SOLVERS.md`, `DEPENDENCIES.md` 중 해당하는 것)

## 브랜치와 커밋

- **`main` 에 직접 커밋하지 않고 푸시하지 않습니다.** 통합은 조율자가 합니다.
- 자기 브랜치에서만 일합니다. 브랜치 이름은 지시서에 있습니다.
- 한 작업 단위가 끝날 때마다 자기 브랜치에 커밋하고 `git push origin <브랜치>` 합니다.
- 커밋 메시지는 영어입니다. 기존 커밋 문체를 따릅니다.
- `git rebase`, `git push --force`, `git reset --hard` 를 자기 브랜치 밖에 쓰지 않습니다.
- 다른 워크스트림이 소유한 파일을 고치지 않습니다. 필요하면 진행 기록에 적고 조율자에게 말합니다.

## 진행 기록

**`docs/workstreams/W<번호>-progress.md` 를 만들고 계속 갱신합니다.** 이것이 세션이 끊겼을 때 다음 세션이 읽는 유일한 기록입니다. 담는 것은 이렇습니다.

- 지금 무엇을 하는 중인지 (한 줄)
- 끝난 작업 단위와 그 커밋 해시
- 내린 설계 결정과 그 근거
- 막힌 것과 그 이유
- 다음에 할 것

작업 단위를 커밋할 때 이 파일도 같이 커밋합니다.

## 빌드와 검증

`third_party/` 는 이미 벤더링되어 있습니다. `scripts/vendor.sh` 를 다시 돌릴 필요가 없습니다.

```sh
cmake --preset windows-clang
cmake --build --preset windows-clang --parallel
ctest --preset windows-clang
```

또는 `./scripts/check.sh windows-clang` 한 줄입니다. Linux 는 `linux-clang` 입니다.

커밋 전 최소 검증은 `AGENTS.MD` 의 표를 따릅니다. **CTest 실패나 skip 을 성공으로 숨기지 않습니다.**

## CMakeLists 를 고치지 않습니다

`CMakeLists.txt` 의 `QL_OPTIONAL_CORE_SOURCES` 에 **아직 없는 파일 이름이 미리 등록되어 있습니다.** 자기 지시서에 적힌 파일 이름을 쓰면 CMake 를 고치지 않아도 빌드에 들어갑니다. 목록에 없는 새 코어 소스가 필요하면 조율자에게 말합니다.

`tests/` 는 `test_*.cpp` 를 자동으로 잡습니다. 시험 파일은 그냥 만들면 됩니다.

`CMakeLists.txt`, `GOAL.md`, `todo.md` 는 조율자가 소유합니다. 고치지 않습니다.

## 지켜야 하는 계약

`AGENTS.MD` 에 전문이 있고 그중 이 작업에서 특히 잘 깨지는 것은 이렇습니다.

- 코어는 C17 입니다. 시험만 C++17 과 GoogleTest 입니다.
- 공개 ABI 구조체는 `struct_size` 와 `abi_version` 을 유지하고 **append-only** 로만 넓힙니다. 기존 필드의 뜻이나 순서를 바꾸지 않습니다. **다른 워크스트림이 지금 그 헤더를 쓰고 있습니다.**
- 지원하지 않는 C 의미론을 추측해서 통과시키지 않습니다. `UNKNOWN` 또는 명시적 오류입니다.
- `BOUNDED_CLEAN` 을 `PROVED_*` 로 올리지 않습니다.
- replay 하지 않은 SAT model 은 확정 `COUNTEREXAMPLE` 이 아닙니다.
- raw solver `UNSAT` 은 그 자체로 Quodlibet proof 가 아닙니다.
- BLAKE3 가 영속 identity 이고 xxHash 는 프로세스 안 임시 해시입니다. 바꿔 쓰지 않습니다.
- 자체 SMT solver 를 만들지 않습니다. Bitwuzla 를 씁니다.

## 말투

사용자께 존댓말입니다. 이모지와 em dash 를 쓰지 않습니다. 답변은 한국어입니다. 코드와 영어 설계 문서(`README.md`, `ARCHITECTURE.md`, `METHODS.md`, `SOLVERS.md`, `DEPENDENCIES.md`)는 영어를 유지합니다.

## 조율자와의 소통

- 상태는 `orca worktree set --worktree active --comment "<한 줄>" --json` 으로 갱신합니다. 작업 단위가 끝날 때마다 합니다.
- 조율자가 터미널로 물으면 답합니다.
- 막히면 멈추지 말고 **막힌 것을 진행 기록에 적고 다음으로 갈 수 있는 항목으로 넘어갑니다.** 전부 막혔을 때만 대기합니다.
- 다른 워크스트림의 결과가 필요하면 그 인터페이스를 가정하고 자기 쪽을 먼저 만듭니다. 가정은 진행 기록에 적습니다.
