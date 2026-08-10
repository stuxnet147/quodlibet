# 설치 패키지 상태

W7 항목 5 의 기록입니다. `scripts/check-install.sh` 가 여기 적힌 것을 전부 실제로 해 보고 PASS/FAIL 로 찍습니다. 수치를 갱신할 때는 그 스크립트를 다시 돌리고 이 문서를 잇습니다.

```sh
cmake --build --preset windows-clang --parallel
bash scripts/check-install.sh windows-clang
```

Linux 는 인자 없이 돌리면 `linux-clang` 을 고릅니다.

## 지금 상태 (2026-08-10, Windows, `windows-clang`)

| 항목 | 결과 |
|---|---|
| `cmake --install` 자체 | 통과 |
| `bin/quodlibet`, 공개 헤더 전부 | 통과 |
| `bin/bitwuzla` 를 실행 파일 옆에 설치 | 통과 |
| `lib/quodlibet_static` | 통과 |
| bin, lib, include 밖에 설치되는 것 없음 | **실패** |
| 설치된 산출물만으로 소비자가 링크 | **실패** |
| prefix 를 옮긴 뒤 relocatable Bitwuzla 탐색 | 통과 |

## 통과한 것: relocatable Bitwuzla 탐색

prefix 전체를 다른 경로로 옮긴 뒤 그 안의 `bin/` 에서 프로그램을 돌리면 Bitwuzla 를 **자기 옆에서** 찾습니다.

이 확인에는 함정이 하나 있습니다. 개발 기계에는 빌드 시점에 박힌 절대 경로가 아직 살아 있어서, 옆을 보지 않고 그 절대 경로로 조용히 되돌아가도 성공처럼 보입니다. 그래서 검사가 음성 방향으로도 갑니다. **옆의 복사본을 Bitwuzla 가 아닌 것으로 바꾸면 실행이 실패해야 합니다.** 실측 결과는 이렇습니다.

```
PASS  the probe runs from the moved prefix
      created ok
PASS  the lookup uses the adjacent copy, not the build-time default
      create failed: configured solver is not the pinned Bitwuzla 0.9.1 executable
```

옆의 것을 바꾸자 실패했으므로 실제로 옆의 것을 쓴 것입니다. 우선순위는 `options_json` 의 `executable`, 그다음 실행 파일 옆, 그다음 빌드 시점 기본값입니다.

## 실패 1: 설치된 산출물만으로는 링크할 수 없습니다

설치된 헤더로 컴파일하고 설치된 라이브러리로 링크하면 이렇게 끝납니다.

```
undefined symbol: yyjson_read_opts
undefined symbol: uv_exepath
undefined symbol: uv_os_getpid
undefined symbol: uv_os_tmpdir
undefined symbol: uv_fs_mkdtemp
```

원인은 둘입니다.

- **벤더링한 dependency 가 설치되지 않고 아카이브에 합쳐지지도 않았습니다.** 설치된 `quodlibet_static` 은 Quodlibet 자기 오브젝트 38개만 담습니다. libuv, yyjson, tree-sitter, BLAKE3, xxHash 는 `lib/` 에도 없습니다.
- **CMake package config 가 없습니다.** `install(TARGETS ...)` 만 있고 `install(EXPORT ...)` 나 `quodlibet-config.cmake` 가 없어 `find_package(quodlibet)` 이 존재하지 않습니다. 그래서 소비자가 어떤 라이브러리를 어떤 순서로 링크해야 하는지 알아낼 방법도 없습니다.

`AGENTS.MD` 의 디렉터리 설명은 "`cmake/`: package와 빌드 지원 모듈" 을 적고 있지만 저장소에 `cmake/` 디렉터리가 없습니다. 계획은 있었고 구현이 없는 상태입니다.

**플러그인 작성자는 이것에 막히지 않습니다.** 플러그인은 `quodlibet/plugin.h` 를 include 하고 심볼 하나를 export 하면 되며 host 가 적재 시점에 호출을 해결하므로 Quodlibet 을 링크할 필요가 없습니다. 막히는 것은 Quodlibet 을 라이브러리로 링크하려는 소비자입니다.

고치는 방법은 셋 중 하나입니다.

1. `install(EXPORT)` 와 `quodlibet-config.cmake` 를 내고 벤더링한 아카이브도 같이 설치해 `find_package` 가 링크 순서를 알려 주게 합니다. 정공법입니다.
2. 벤더링한 오브젝트를 `quodlibet_static` 하나로 합쳐 자기완결 아카이브로 만듭니다. 소비자는 단순해지지만 심볼 충돌 위험이 옮겨 갑니다.
3. 공유 라이브러리(`QL_BUILD_SHARED=ON`)를 설치 기본으로 삼습니다. 파이썬 확장이 이미 반대 방향(정적 링크로 단일 파일)을 택한 이유가 있으므로 그 결정과 충돌합니다.

**루트 `CMakeLists.txt` 가 조율자 소유라 W7 이 고르지 않았습니다.**

## 실패 2: prefix 뿌리에 `quodlibet/` 이 생깁니다

파이썬 확장이 `<prefix>/quodlibet/_quodlibet.pyd` 로 설치됩니다. prefix 는 다른 패키지와 공유하는 자리이고 그 뿌리에 이름 하나를 차지하는 디렉터리가 생기는 것은 충돌을 기다리는 상태입니다. site-packages 상대 경로이거나, 애초에 루트 install 에 들어가지 않는 것이 맞습니다. `bindings/python` 은 W4 소유입니다.

## 아직 재지 않은 것

- **Linux 실측.** 스크립트는 `linux-clang` 을 그대로 받지만 이 문서의 표는 Windows 결과입니다. Linux 는 `rpath` 가 추가로 걸리는 축이라 별도로 재야 합니다.
- **설치된 CLI 는 solver 를 쓰지 않습니다.** `version`, `methods`, `parse-c`, `validate`, `coverage` 중 어느 것도 판정을 돌리지 않으므로 CLI 만으로는 Bitwuzla 탐색을 확인할 수 없습니다. 위 확인이 작은 소비자 프로그램(`scripts/install/consumer.c`)을 쓰는 이유입니다.
