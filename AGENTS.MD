# Quodlibet 작업 규칙

이 파일은 저장소 전체에 적용됩니다. 사용자 지시가 이 문서보다 우선합니다.

## 의사소통과 작업 방식

- 사용자께는 존댓말을 사용합니다.
- 이모지와 em dash를 사용하지 않습니다.
- Windows 명령은 Git Bash에서 실행할 POSIX shell 문법으로 작성합니다.
- 검색은 `rg`와 `rg --files`를 우선 사용하고, 파일 편집은 `apply_patch`를 사용합니다.
- 사용자의 기존 변경과 생성물을 보존합니다. 관련 없는 파일을 고치거나 커밋하지 않습니다.
- 현재 배포 정책은 작업이 검증되면 별도 브랜치나 PR 없이 `main`에 직접 커밋하고 푸시하는 것입니다. 사용자가 변경하면 최신 지시를 따릅니다.
- 원격 CI는 의도적으로 비활성화되어 있습니다. 로컬 검증 결과를 커밋 전에 확인합니다.
- 장기 작업은 `todo.md`, 세션 인계는 gitignored `handoff.md`에 기록합니다.

## 의미론과 proof 규칙

- 코어는 C17로 작성하고 시험은 C++17과 GoogleTest를 사용합니다.
- 공개 ABI 구조체는 `struct_size`와 `abi_version`을 유지하고 append-only로 확장합니다.
- plugin 경계에서 서로 다른 CRT가 할당한 메모리의 해제를 넘기지 않습니다. host service와 immutable artifact ownership 규칙을 따릅니다.
- 영속 identity와 evidence에는 BLAKE3 content digest를 사용합니다. process-local hash를 proof 또는 cache identity로 사용하지 않습니다.
- 지원하지 않는 C 구문이나 의미론은 추측하지 않습니다. 명시적 오류 또는 `UNKNOWN`을 반환합니다.
- `BOUNDED_CLEAN`은 `PROVED_*`로 승격하지 않습니다.
- replay되지 않은 SAT model은 확정 `COUNTEREXAMPLE`이 아닙니다.
- raw solver `UNSAT`은 독립 checker 또는 명시된 신뢰 정책 없이 Quodlibet proof가 아닙니다.
- problem v1의 non-null precondition은 typed signature와 결합되지 않았으므로 proof 근거로 사용하지 않습니다.
- SMT solver는 직접 구현하지 않습니다. Bitwuzla를 사용하며, SMT-LIB 계층은 결정적 직렬화와 adapter 역할만 합니다.
- proof method를 병렬 결합할 때 다수결을 사용하지 않습니다. 검증 가능한 evidence와 soundness 경계로 판정합니다.

작업 전에 `ARCHITECTURE.md`, `METHODS.md`, `SOLVERS.md`, `todo.md`에서 관련 계약을 확인합니다.

## 빌드와 시험

필수 도구는 CMake 3.21 이상, Ninja, Clang C17/C++17, Git Bash 또는 POSIX shell입니다. 첫 빌드에서는 고정된 third-party dependency를 준비합니다.

```sh
cd D:/projects/machine-model/python/quodlibet
./scripts/vendor.sh
./scripts/check.sh windows-clang
```

Linux에서는 다음을 사용합니다.

```sh
./scripts/check.sh linux-clang
```

명시적인 세 단계는 다음과 같습니다.

```sh
cmake --preset windows-clang
cmake --build --preset windows-clang --parallel
ctest --preset windows-clang
```

Bitwuzla 없이 transport-independent 코어를 확인하려면 별도 build directory에서 `-DQL_ENABLE_BITWUZLA=OFF`를 사용합니다. test 없는 core-only 구성은 `-DBUILD_TESTING=OFF -DQL_BUILD_TESTS=OFF`를 사용합니다. 기존 `out/`을 파괴적으로 지우지 말고 목적별 build directory를 사용합니다.

변경 규모에 맞춰 최소한 다음을 검증합니다.

- 모든 변경: `git diff --check`
- C/CMake 변경: 영향받는 host preset configure, build, CTest
- ABI, allocator, scheduler, artifact 변경: Windows와 Linux 전체 시험
- solver 변경: Bitwuzla ON/OFF, timeout/cancel/output cap, snapshot cleanup 시험
- 문서 전용 변경: 링크와 명령 확인, ignore 상태, `git diff --check`

CTest 실패, sanitizer 실패, skip을 성공으로 숨기지 않습니다. 플랫폼이나 backend가 없어 시험하지 못한 범위를 최종 보고에 명시합니다.

## 디렉터리 설명

- `include/quodlibet/`: 설치되는 공개 C API와 versioned ABI
- `src/`: C17 코어 구현과 내부 verifier, frontend, IR, solver adapter
- `cli/`: 네이티브 명령행 프로그램
- `tests/`: GoogleTest 소스, plugin fixture, CTest 등록
- `third_party/`: checksum이 고정된 dependency source와 Bitwuzla bundle
- `scripts/`: dependency 준비와 로컬 통합 검증 진입점
- `examples/`: pipeline 및 입력 예제
- `cmake/`: package와 빌드 지원 모듈
- `out/`: 무시되는 build, test, install 생성물
- 루트 설계 문서: `README.md`, `ARCHITECTURE.md`, `METHODS.md`, `SOLVERS.md`, `DEPENDENCIES.md`

## 문서 갱신

공개 동작, ABI, dependency, proof 신뢰 경계가 바뀌면 같은 커밋에서 관련 루트 문서와 시험을 갱신합니다. 완료한 작업은 `todo.md`에 표시하되 검증되지 않은 기능을 완료로 기록하지 않습니다.
