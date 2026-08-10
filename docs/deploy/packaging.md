# 설치 패키지 상태

W7 항목 5 의 기록입니다. `scripts/check-install.sh` 가 여기 적힌 것을 전부 실제로 해 보고 PASS/FAIL 로 찍습니다. 수치를 갱신할 때는 그 스크립트를 다시 돌리고 이 문서를 잇습니다.

```sh
cmake --build --preset windows-clang --parallel
bash scripts/check-install.sh windows-clang
```

Linux 는 인자 없이 돌리면 `linux-clang` 을 고릅니다. 스크립트는 하나라도 실패하면 exit 이 0 이 아닙니다.

## 지금 상태 (2026-08-10)

| 항목 | Windows | Linux |
|---|---|---|
| `cmake --install` 자체 | 통과 | 통과 |
| `bin/quodlibet`, 공개 헤더 전부 | 통과 | 통과 |
| `bin/bitwuzla` 를 실행 파일 옆에 설치 | 통과 | 통과 |
| CMake package configuration 설치 | 통과 | 통과 |
| 코어 라이브러리 설치 | 통과 (`quodlibet_static.lib`) | 통과 (`libquodlibet.a`) |
| bin, lib, include 밖에 예상 밖의 것 없음 | 통과 | 통과 |
| **설치된 라이브러리 + 시스템 라이브러리만으로 링크** | 통과 | 통과 |
| **`find_package(quodlibet)` 로 소비** | 통과 | 통과 |
| prefix 를 옮긴 뒤 relocatable Bitwuzla 탐색 | 통과 | 통과 |

Windows 는 `windows-clang`, Linux 는 WSL Ubuntu 24.04 의 `linux-clang` 입니다.

## 자기완결 라이브러리

벤더링한 dependency 를 `libquodlibet` **안으로 접어 넣습니다.** 소비자는 하나만 링크하며 libuv, yyjson, tree-sitter, BLAKE3, xxHash 를 자기 링크 줄에 적지 않습니다. 파이썬 확장이 같은 이유로 이미 자기완결이었고(로더 경로에 놓을 동반 파일이 없는 단일 import 파일) C 소비자를 같은 자리에 세웠습니다.

아카이브를 병합하는 대신 오브젝트를 접는 방식을 골랐습니다. **이것이 export 를 가능하게 하는 조건이기도 합니다.** 정적 라이브러리에 대한 PRIVATE 링크도 interface 에 `$<LINK_ONLY:...>` 로 남고, `install(EXPORT)` 는 export 되지 않은 타깃을 interface 에 담은 타깃을 거부합니다. 벤더 타깃은 설치될 일이 없으므로 오브젝트만 들어가고 타깃은 빠집니다.

접힌 오브젝트가 운영체제에서 필요로 하는 것은 exported interface 로 따라갑니다. 전부 평범한 시스템 라이브러리라 소비자가 자기 toolchain 에서 해결합니다.

- Windows: `psapi user32 advapi32 iphlpapi userenv ws2_32 dbghelp ole32 shell32`
- Linux: `pthread dl rt m`

확인은 실제 링크로 합니다. 설치된 라이브러리와 위 시스템 라이브러리만 주고 소비자를 링크하며, **벤더 dependency 를 하나라도 링크 줄에 요구하면 실패입니다.**

## find_package

```cmake
find_package(quodlibet REQUIRED)
target_link_libraries(my_target PRIVATE quodlibet::quodlibet)
```

`cmake/quodlibet-config.cmake.in` 에서 생성되며 `find_dependency` 호출이 없습니다. 전부 접었으므로 부를 것이 없습니다. 검사는 이 세 줄짜리 프로젝트를 실제로 configure 하고 build 합니다.

**플러그인은 이것이 필요 없습니다.** `quodlibet/plugin.h` 를 include 하고 심볼 하나를 export 하면 host 가 적재 시점에 호출을 해결합니다. `examples/plugin/README.md` 가 그렇게 적습니다.

## relocatable Bitwuzla 탐색

prefix 전체를 다른 경로로 옮긴 뒤 그 안의 `bin/` 에서 돌리면 Bitwuzla 를 **자기 옆에서** 찾습니다.

이 확인에는 함정이 하나 있습니다. 개발 기계에는 빌드 시점에 박힌 절대 경로가 아직 살아 있어서, 옆을 보지 않고 그 절대 경로로 조용히 되돌아가도 성공처럼 보입니다. 그래서 검사가 음성 방향으로도 갑니다. **옆의 복사본을 Bitwuzla 가 아닌 것으로 바꾸면 실행이 실패해야 합니다.** 양쪽 플랫폼 실측입니다.

```
PASS  the probe runs from the moved prefix
      created ok
PASS  the lookup uses the adjacent copy, not the build-time default
      create failed: configured solver is not the pinned Bitwuzla 0.9.1 executable
```

우선순위는 `options_json` 의 `executable`, 그다음 실행 파일 옆, 그다음 빌드 시점 기본값입니다.

## rpath

**기본 구성에는 재배치할 것이 없습니다.** `QL_BUILD_SHARED` 가 기본 OFF 라 설치되는 것은 정적 아카이브이고, 설치된 CLI 에는 `RPATH` 도 `RUNPATH` 도 없습니다. 동적 의존은 libc 와 libm 뿐입니다.

```
--- RUNPATH/RPATH on the installed CLI ---
(none: nothing to relocate at load time)
--- dynamic deps ---
    linux-vdso.so.1
    libm.so.6 => /lib/x86_64-linux-gnu/libm.so.6
    libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6
```

`QL_BUILD_SHARED=ON` 설치는 재지 않았습니다. 그 구성에서는 `libquodlibet.so` 를 찾는 문제가 생기므로 별도 축입니다.

## 결함이 아닌 것: prefix 의 `quodlibet/` 디렉터리

파이썬 확장이 `<prefix>/quodlibet/_quodlibet.pyd` 로 나타납니다. **배포 경로가 `cmake --install` 이 아니라 pip 이므로 결함이 아닙니다.** 같은 트리에서 같이 빌드되기 때문에 보이는 것뿐입니다. 검사는 이 디렉터리를 예상된 예외로 두고 note 로만 적으며, 그 밖의 것이 prefix 뿌리에 생기면 실패합니다.

## 아직 재지 않은 것

- **`QL_BUILD_SHARED=ON` 의 설치와 rpath.** 위에 적은 대로 별도 축입니다.
- **설치된 CLI 는 solver 를 쓰지 않습니다.** `version`, `methods`, `parse-c`, `validate`, `coverage` 중 어느 것도 판정을 돌리지 않으므로 CLI 만으로는 Bitwuzla 탐색을 확인할 수 없습니다. 위 확인이 작은 소비자 프로그램(`scripts/install/consumer.c`)을 쓰는 이유입니다.
