# Claude 작업 안내

이 저장소에서 작업할 때 먼저 `AGENTS.MD`를 읽고 그대로 따릅니다. 사용자 지시가 최우선이며, 이 파일은 Claude 계열 작업자가 빠르게 저장소 계약을 확인하기 위한 요약입니다.

## 필수 규칙

- 사용자께 존댓말을 사용하고 이모지와 em dash를 사용하지 않습니다.
- Windows shell은 Git Bash입니다. 명령은 Bash와 POSIX 문법으로 작성합니다.
- 현재 정책은 검증한 변경을 `main`에 직접 커밋하고 푸시하는 것입니다. PR은 만들지 않습니다.
- 원격 CI는 비활성화되어 있으므로 로컬 CMake, GTest, CTest 결과가 배포 관문입니다.
- C17 코어와 공개 C ABI의 ownership, `struct_size`, `abi_version` 계약을 깨지 않습니다.
- 사용하지 못하는 C 의미론은 `UNKNOWN` 또는 오류로 거부하며 임의로 단순화하지 않습니다.
- Bitwuzla가 SMT solver입니다. 별도의 자체 SMT solver를 구현하지 않습니다.
- `BOUNDED_CLEAN`, raw `UNSAT`, replay하지 않은 SAT model을 각각 proof나 확정 counterexample로 잘못 승격하지 않습니다.
- 현재 problem v1은 typed precondition을 양쪽 source signature와 결합하지 않으므로 non-null precondition에 대한 `PROVED_*`를 허용하지 않습니다.
- 장기 작업 상태는 `todo.md`에, 세션 인계는 추적하지 않는 `handoff.md`에 남깁니다.

작업 시작 전에 변경 영역에 따라 `README.md`, `ARCHITECTURE.md`, `METHODS.md`, `SOLVERS.md`, `DEPENDENCIES.md`, `todo.md`를 읽습니다.

## 표준 빌드

Windows Git Bash:

```sh
cd D:/projects/machine-model/python/quodlibet
./scripts/vendor.sh
./scripts/check.sh windows-clang
```

Linux:

```sh
./scripts/vendor.sh
./scripts/check.sh linux-clang
```

수동 실행은 configure, build, CTest 순서입니다.

```sh
cmake --preset windows-clang
cmake --build --preset windows-clang --parallel
ctest --preset windows-clang
```

Linux preset은 `linux-clang`입니다. Bitwuzla 없는 구성은 별도 build directory에서 `-DQL_ENABLE_BITWUZLA=OFF`, test 없는 core-only 구성은 `-DBUILD_TESTING=OFF -DQL_BUILD_TESTS=OFF`를 사용합니다. 문서만 고쳐도 `git diff --check`와 링크, ignore 상태를 확인합니다.

## 디렉터리 지도

- `include/quodlibet/`: 공개 C API
- `src/`: 런타임, frontend, IR, e-graph, proof registry, solver adapter
- `cli/`: `quodlibet` 실행 파일
- `tests/`: GoogleTest와 CTest 입력
- `third_party/`: 고정 dependency와 Bitwuzla bundle
- `scripts/`: `vendor.sh`, `check.sh`
- `examples/`: C 입력과 pipeline 예제
- `cmake/`: 설치 및 package 지원
- `out/`: 커밋하지 않는 build 생성물

## 현재 구현 경계

공통 기반은 마련되어 있지만 end-to-end 동치 proof path는 아직 없습니다. 다음 핵심 단계는 problem schema v2, source-signature와 typed-precondition binding, product-program SMT query, counterexample replay, trusted UNSAT checker입니다. 우선순위와 세부 항목은 `todo.md`를 기준으로 합니다.
