# W4 진행 기록. FFI 가 아닌 파이썬 바인딩

브랜치: `stuxnet147/w4-python-bindings`
지시서: `docs/workstreams/W4.md`
담는 GOAL: G4

## 지금 하는 것

**G4 종료 조건 여섯 개가 전부 닫혔습니다.** Linux 까지 조율자가 확인해 주었습니다.

## 읽은 것과 확인한 코어 경계

- 판정 경로는 `prove.smt-product` 이고 입력은 schema v2 `quodlibet.problem` artifact 하나입니다. 만드는 절차는 `tests/w2_fixtures.h` 의 `w2::Pair` 가 그대로 보여줍니다. 좌우 각각 `ql_c_frontend_analyze` -> `ql_c_frontend_select_function` -> `ql_c_lower_selected_function` -> `ql_source_signature_from_c_function` 을 하고, 인자 전단사를 붙여 `ql_problem_artifact_create_v2` 를 부릅니다.
- 예산은 `ql_pipeline_run_with_budget` 이 이미 배선되어 있습니다. 노드마다 `QL_BUDGET_SCOPE_NODE` 스코프가 열리고 run context 의 취소 술어로 budget 이 전달되며, 축이 소진되면 산출물을 전부 해제하고 `QL_STATUS_CANCELLED` 또는 `QL_STATUS_OUT_OF_MEMORY` 를 냅니다. 그래서 바인딩은 자체 예산 배선을 새로 만들지 않고 이 경로를 씁니다.
- 판정 정책은 `ql_policy_parse` 와 `ql_policy_evaluate` 입니다. 파싱 거부는 실행 전이므로 파이썬 예외로 올립니다.
- solver 실행 파일은 `{"solver_options":"{\"executable\":\"...\"}"}` 로 method 옵션에 실려 들어갑니다. 파이썬 인자 `solver_executable` 하나로 노출했습니다.
- policy 의 trusted backend 문자열은 `"<descriptor->name> <descriptor->version>"`, 즉 `"bitwuzla 0.9.1"` 입니다.

## 설계 결정

- **확장 모듈 하나 + 얇은 파이썬 패키지.** `quodlibet._quodlibet` 이 abi3 C 확장이고 `quodlibet/__init__.py` 가 결과 객체와 이름 변환과 배치 경로를 감쌉니다. C 쪽은 dict 하나만 만들어 돌려주므로 CPython 객체 수명 규율이 좁은 면적에 갇힙니다.
- **판정 본체(`bindings/python/src/ql_check.c`)는 CPython 심볼을 하나도 쓰지 않습니다.** 그래서 `Py_BEGIN_ALLOW_THREADS` 로 통째로 감쌀 수 있고, GIL 을 놓은 구간에서 인터프리터 객체를 건드리는 사고가 구조적으로 불가능합니다. 인자는 GIL 을 쥔 채 `ql_py_spec` 으로 복사하고, 결과는 GIL 을 되찾은 뒤 `ql_py_result` 에서 읽습니다.
- **`check_batch` 는 파이썬 스레드 풀** 위에서 `check` 를 부릅니다. GIL 해제가 실제 병렬성의 근거이므로 C 쪽에 별도 스레드 풀을 만들지 않습니다. `workers=0` 은 `os.cpu_count()` 입니다. 실패한 spec 은 예외 객체로 자리에 남겨 배치 전체를 잃지 않습니다.
- **`Py_LIMITED_API=0x030B0000`.** 정적 타입 객체를 만들지 않고 함수와 dict 만 씁니다. `PySequence_Fast_*` 처럼 stable ABI 밖의 매크로는 쓰지 않습니다.
- **예산은 모든 실패 경로에서 UNKNOWN 입니다.** 메모리 축은 method 실행 전 frontend 할당에서 먼저 걸리므로, `ql_budget_is_exhausted` 가 참이면 어떤 실패든 예외가 아니라 budget UNKNOWN 으로 접습니다. 마지막에 `ql_budget_guard_outcome` 을 한 번 더 통과시킵니다.
- **해제 순서**는 artifact -> pipeline result -> pipeline -> scheduler -> registry -> problem -> side -> policy -> budget 입니다. budget 이 모든 handle 이 쓴 allocator 의 소유자이므로 마지막입니다. 파이썬으로 넘기는 counterexample 바이트는 budget allocator 가 아니라 CRT `malloc` 으로 복사해서 budget 수명과 무관하게 만듭니다.
- **Windows 에서 `pip install` 은 Clang + Ninja 를 고릅니다.** 기본 생성기의 MSVC 는 런타임이 쓰는 C11 atomics 를 거부합니다. 저장소의 `windows-clang` 프리셋과 같은 툴체인입니다.
- **install component 분리.** `install(TARGETS _quodlibet ... COMPONENT quodlibet_python)` 과 `install.components` 로 코어의 install 규칙(CLI, 헤더, solver 실행 파일)이 wheel 에 딸려 들어가지 않게 했습니다. `COMPONENT` 는 artifact 그룹마다 따로 붙여야 합니다. `LIBRARY DESTINATION ... RUNTIME DESTINATION ... COMPONENT x` 로 쓰면 `COMPONENT` 가 `RUNTIME` 에만 붙어서 정작 `.pyd` 는 `Unspecified` 로 남고 wheel 에서 빠집니다.
- **재진입 가드.** 루트에 `add_subdirectory(bindings/python)` 훅이 들어간 뒤로는 `bindings/python` 을 최상위로 configure 하면 자기 자신을 두 번 처리하게 됩니다(이 디렉터리 -> 루트 -> 이 디렉터리). `QL_PYTHON_BINDINGS_ENTERED` 로 두 번째 진입에서 `return()` 합니다. cache 가 아니라 보통 변수여야 같은 build tree 를 다시 configure 할 때 첫 진입이 다시 살아납니다.

## 끝난 작업 단위

1. CPython C 확장, 빌드, pytest 25개, CTest 등록, 문서 - 커밋 `daccf93` (진행 기록 `6fbb893`)
2. 재진입 가드와 abi3 교차 버전 확인, pytest 27개 - 커밋 `5e8897c`
3. `solver_executable` 인자 시험 추가, pytest 28개 - 커밋 `ad2cd44`

## 조율자에게 올려 처리된 것

- 루트 `CMakeLists.txt` 의 `add_subdirectory(bindings/python)` 훅. 조율자가 main 에 넣었습니다(`199654e` 계열).
- `bindings/python/CMakeLists.txt` 의 Linux PIC 한 줄. Linux 에서 벤더링된 libuv archive 가 `-fPIC` 없이 빌드되어 공유 모듈에 접히지 않았습니다. 조율자가 standalone 경로에 `set(CMAKE_POSITION_INDEPENDENT_CODE ON)` 을 넣었고(`8ddd5e7`), WSL Ubuntu 24.04 에서 import 와 시험 27/27 이 통과했습니다. 이 워크스트림에는 Linux 가 없어 직접 재현할 수 없는 실패였습니다.
- `src/solver.c` 의 동시 실행 결함. 4-way 배치에서 `solver output pipes remained open after the child exit drain deadline` 가 6/60 로 났고, 원인은 `QL_PROCESS_DRAIN_GRACE_MS` 250ms 가 Windows 동시 부하에서 짧은 것이었습니다. 조율자가 5000ms 로 고쳤습니다. rebase 후 같은 측정을 다시 돌려 **0/60** 입니다.

## 막힌 것

- **`src/solver.c` 의 두 번째 동시 실행 결함(조율자에게 보고함).** 8-way 배치에서 `could not open Bitwuzla executable for identity hashing` 가 6/80 로 납니다. 4-way 에서는 0/60 이었지만 `ctest -R python` 을 6회 반복하니 1회 나왔습니다. `create_executable_snapshot` 이 판정마다 Bitwuzla 실행 파일(약 5MB)을 개인 임시 디렉터리로 새로 복사하고 곧바로 `digest_executable` 이 그 파일을 여는데, Windows 에서 갓 쓰인 실행 파일은 실시간 검사가 잠깐 공유 거부로 물고 있어서 `fopen("rb")` 가 실패합니다. 동시에 여러 개를 복사할수록 확률이 오릅니다. `src/solver.c` 는 조율자 소유라 손대지 않았고, 짧은 backoff 재시도를 제안해 두었습니다.

## 다음에 할 것

W4 가 담는 G4 는 닫혔습니다. 이후에 나올 만한 것은 이렇습니다.

1. G7 의 채점기 경로에서 asm2c 코퍼스 표본에 대한 쌍당 지연 측정. 배치 경로는 이미 있으므로 측정 스크립트만 붙이면 됩니다.
2. `py.typed` 와 타입 스텁. 지금은 `__init__.py` 에 인라인 주석만 있습니다.
3. `src/solver.c` 의 snapshot hashing 경합이 닫히면 8-way 이상 배치도 안정적입니다. 현재 시험은 4-way 라 영향이 드물지만 latent 합니다.

## G4 종료 조건 대조

- [x] **CPython C 확장 모듈**이다. ctypes, cffi, ABI 를 런타임에 재선언하는 방식이 아니다 - `bindings/python/src/quodlibet_module.c`, `tests/test_extension.py::test_the_module_is_a_compiled_extension`, `::test_no_ffi_layer_is_involved`
- [x] `Py_LIMITED_API` (abi3) 로 빌드 - `Py_LIMITED_API=0x030B0000`, `python_add_library(... USE_SABI 3.11)`, wheel 태그 `cp311-abi3-win_amd64`, `::test_it_is_built_against_the_stable_abi`. **주장에 그치지 않게 실제로 확인했습니다.** 3.11 로 만든 wheel 을 CPython 3.13.12 venv 에 그대로 설치해 시험 전부 통과했습니다.
- [x] Windows 와 Linux 양쪽에서 import 되고 왕복 시험이 통과 - Windows 는 CPython 3.11 과 3.13 양쪽에서 통과(현재 28개). **Linux 는 조율자가 WSL Ubuntu 24.04 에서 import 와 27/27 을 확인**했고, 그 과정에서 필요했던 PIC 한 줄이 `8ddd5e7` 로 들어갔습니다.
- [x] GIL 을 solver 대기 동안 놓는다 - `Py_BEGIN_ALLOW_THREADS` 로 판정 전체를 감쌈. `tests/test_concurrency.py::test_python_keeps_running_while_a_check_is_in_flight` 가 판정 도중 파이썬 스레드가 실제로 도는지를, `::test_two_checks_are_in_flight_at_the_same_instant` 가 두 판정의 구간이 실제로 겹치는지를 고정합니다. 둘 다 벽시계 비율이 아니라 구조적 성질을 봅니다. 비율 시험은 기계 부하를 재는 것이라 flaky 해서 버렸습니다 (실측으로는 4-way 에서 6.7s -> 1.6s)
- [x] 예산, 판정 정책, 결과가 파이썬 쪽에서 전부 노출 - `budget=` 다섯 축, `policy_json=`, `result.verdict/.status/.evidence/.counterexample/.policy`. `tests/test_budget_and_policy.py` 9개
- [x] 빌드가 CMake 한 경로에 들어 있고 `pip install .` 이 된다 - `bindings/python/CMakeLists.txt` 가 루트를 subproject 로 부르고, `pip install ./bindings/python` 이 abi3 wheel(590KB, `.pyd` 포함)을 만들어 설치까지 확인. 같은 파일이 루트에서 `add_subdirectory` 될 때는 CTest 항목 `quodlibet.python_bindings` 를 등록합니다.
