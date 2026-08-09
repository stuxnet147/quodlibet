# 성능 기준선

시작 2026-08-10. G6 의 기준선 문서입니다. 수치를 갱신할 때는 아래 재현 명령을 그대로 다시 돌리고 이 문서에 날짜와 함께 잇습니다. 2026-08-10 부터 W8 이 소유합니다(`docs/workstreams/W8.md`).

## 이 디렉터리

| 파일 | 무엇 |
|---|---|
| `baseline.md` | 방법, 기준선, 적용한 최적화, 병렬성. 이 문서 |
| `hotspots-coverage-val.txt` | VTune hotspots 원본 리포트 (coverage over val). 생성 명령이 파일 머리에 있다 |

## 두 가지 도구

- **회귀 관문**: `scripts/perf/bench-coverage.sh`. 프로파일러가 필요 없고 총량만 봅니다. **"총량이 움직였는가"** 에 답합니다.
- **귀속 분석**: `scripts/perf/vtune-hotspots.sh`. VTune hotspots 를 스택 수집과 함께 겁니다. **"시간이 어디로 가는가"** 에 답합니다.

회귀 관문은 coverage JSON 의 digest 를 같이 찍습니다. digest 가 바뀌면 측정 대상 자체가 바뀐 것이므로 시간 비교는 무효입니다. 빨라진 것이 아니라 덜 한 것입니다.

## 측정 환경

- WSL2 Ubuntu 24.04 (호스트 Windows 11, 24 논리 코어, 3.686 GHz)
- Intel VTune Profiler 2026.3 (build 632627), **user-mode sampling**
- 대상 바이너리: `out/build/linux-clang/quodlibet` (RelWithDebInfo, Clang)
- 워크로드: val 서로 다른 C 본문 1,050개에 대한 `coverage` (파스 + 프런트엔드 + 로어링)

### 전제 조건 둘

- **WSL 에서 `kernel.yama.ptrace_scope=0` 이어야 한다.** 아니면 `Cannot start data collection` 으로 즉시 실패한다. `sudo sysctl -w kernel.yama.ptrace_scope=0`.
- **hw 샘플링 드라이버(sep5)는 WSL 에서 안 뜬다.** 사용자 모드 샘플링으로 충분하며 그것만 쓴다.

### Windows VTune 은 이 호스트에서 쓰지 않는다

hw, 기본, sw 명시, 자체 진단기, echo 최소 대상 다섯 프로브 전부에서 `amplxe-runss` 수집기가 시작 단계에서 무출력으로 매달리고 결과 디렉터리가 생기지 않았다(2026-08-10 실측). Win11 보안 정책과 수집기의 충돌로 추정한다. 프로파일링은 Linux(WSL 또는 VM)에서 한다.

### 워크로드를 `/mnt` 에 두지 않는다

같은 워크로드를 `/mnt/d`(9p) 위 파일로 돌리면 `fopen`/`fclose`/`fseek` 이 CPU 의 33.3% 를 차지해 엔진 분포가 가려진다(2026-08-10 실측). 유닛 파일을 `/tmp` 로 복사해서 돌린다. 이 왜곡은 측정 하네스의 것이고 실제 소비자(파이썬 바인딩)는 소스를 메모리로 넘기므로 이 경로 자체가 없다.

## 기준선 (2026-08-10, main `69c39dd` + 중복 파스 제거 패치)

val 1,050 유닛, `/tmp` 네이티브 디스크, 단일 스레드.

| 지표 | 값 |
|---|---|
| Elapsed | 0.394s |
| CPU Time | 0.380s |
| 유닛당 | 0.362ms (파스 + 프런트엔드 + 로어링) |

Top hotspots:

| 함수 | 모듈 | CPU | 비율 |
|---|---|---|---|
| `ts_stack_push` | quodlibet | 0.066s | 17.4% |
| `ts_tree_cursor_child_iterator_next` | quodlibet | 0.048s | 12.6% |
| `ts_lex` | quodlibet | 0.044s | 11.6% |
| `__libc_free` | libc | 0.020s | 5.3% |
| `stack__iter` | quodlibet | 0.020s | 5.3% |
| 나머지 | | 0.182s | 47.9% |

**tree-sitter 파싱이 여전히 지배한다.** 상위 항목이 전부 파서 계열이다. 로어링과 프런트엔드 자체는 아직 hotspot 에 들어오지도 않는다.

## 호출 귀속 (2026-08-10, W8 인수, `7d4dce7`)

평탄한 함수 목록은 **프런트엔드의 파스와 로어링의 파스를 구분하지 못한다.** 이 워크로드에서는 그 구분이 질문 전체이므로 스택 수집을 켜고 다시 걸었다. 짧은 실행(0.38s)은 표본이 90개 남짓이라 가지별 비율이 흔들리므로 유닛 목록을 10회 이어붙여 4.48s CPU 로 늘렸다. 유닛당 비용은 그대로다.

명령: `./scripts/perf/wsl.sh scripts/perf/vtune-hotspots.sh val 10`

| 호출 경로 | CPU 비율 |
|---|---|
| `ql_c_frontend_analyze` | **47.6%** |
| ㄴ `ql_c_parser_parse` | 31.7% |
| ㄴ `collect_syntax_nodes` | 12.2% |
| ㄴ `ql_c_syntax_tree_destroy` | 1.9% |
| ㄴ `analyze_function` (실제 분석) | 1.3% |
| `ql_c_lower_selected_function` | **47.9%** |
| ㄴ `ql_c_parser_parse` | 30.0% |
| ㄴ `collect_nodes` | 11.8% |
| ㄴ `cleanup_context` (트리 해제) | 2.5% |
| ㄴ `ql_c_parser_create` + `_destroy` | 1.9% |
| ㄴ 실제 로어링 (`collect_records`, `initialize_parameters`, ...) | 1.7% |
| 측정 하네스의 파일 I/O (`fopen`/`fseek`/`fgets`) | 4.4% |

**두 가지를 말한다.**

1. **파싱 61.6% + 구문 노드 수집 24.0% = 85.6%.** 프런트엔드가 하는 실제 분석은 1.3%, 로어링이 하는 실제 로어링은 1.7% 다. 이 도구는 지금 거의 전부 tree-sitter 다.
2. **두 가지가 대칭이다.** 프런트엔드 47.6%, 로어링 47.9%. 두 경로가 **같은 소스를 각각 파싱하고 각각 같은 방식으로 트리를 걸어 같은 모양의 노드 배열을 만든다.** 앞선 짧은 수집에서 로어링 쪽이 3배 비싸 보였던 것은 표본 부족이었고, 이 수집이 그것을 정정한다.

중복이 트리 하나가 아니라 **파스 + 걷기 + 트리 해제 + 파서 생성 전부**이므로 제거 가능한 몫은 파스 절반(30.8%)이 아니라 **약 46%** 다.

`ql_c_syntax_record` (`src/c_frontend.c:6`) 와 `lower_node` (`src/c_lower.c:28`) 는 필드가 글자 그대로 같다.

```c
typedef struct { ql_c_syntax_node_view view; size_t parent; uint32_t depth; } ...;
```

즉 트리뿐 아니라 **수집한 노드 배열까지 그대로 넘길 수 있다.** 두 파일은 W1 소유이므로 이 최적화는 W1 과 합의해야 한다.

## 적용한 최적화

### 1. coverage 경로의 중복 구문 파스 제거 (2026-08-10)

**프로파일 근거.** 첫 수집에서 상위 5 함수가 전부 tree-sitter 파싱(합 59.2%)이었고, 코드 확인으로 같은 소스가 유닛당 **세 번** 파싱되고 있었다: CLI 구문 검사(`cli/coverage.c`), 프런트엔드 분석(`src/c_frontend.c:1002`), 로어링(`src/c_lower.c:3123`).

**변경.** 프런트엔드가 오류 트리를 `QL_STATUS_PARSE_ERROR` 로 이미 구분해 주므로 CLI 의 사전 구문 파스는 순수 중복이었다. 제거했다. 측정 결과의 분류(`units_syntax_error`)는 동일하게 유지된다(val 에서 전 지표 일치 확인).

**효과** (같은 방법, 같은 코퍼스):

| | CPU Time | 유닛당 |
|---|---|---|
| 전 | 0.520s | 0.495ms |
| 후 | 0.380s | 0.362ms |
| 차 | **-27%** | |

### 남은 알려진 중복: 프런트엔드와 로어링의 이중 파스 + 이중 걷기

유닛당 아직 **두 번** 파싱하고 **두 번** 걷는다. 위 호출 귀속 절이 몫을 확정했다: 제거 가능한 CPU 는 **약 46%** 다. **이 코드는 W1 소유(`src/c_frontend.c`, `src/c_lower.c`)이므로 W1 과 합의한다.** 제안한 API 는 아래와 같다.

```c
/* c_frontend.h, append-only */
QL_API ql_status QL_CALL ql_c_frontend_analyze_with_parser(
    const ql_allocator *allocator, const char *source, size_t source_size,
    ql_c_parser *parser, ql_c_frontend_unit **output, ql_error *error);

/* c_lower.h, append-only. tree 와 nodes 가 NULL 이면 현재 동작 그대로. */
QL_API ql_status QL_CALL ql_c_lower_selected_function_with_tree(
    const ql_allocator *allocator, const char *source, size_t source_size,
    const ql_c_frontend_unit *unit, const ql_c_function_view *function,
    ql_c_syntax_tree *tree, ql_c_lower_result **output, ql_error *error);
```

`ql_c_syntax_tree` 는 이미 원자적 참조 계수를 갖고 있으므로(`src/c_syntax.c:15`) 소유권 계약을 바꾸지 않고 빌려줄 수 있다. 기존 두 함수는 새 함수를 파서/트리 NULL 로 부르는 얇은 껍데기가 되므로 **ABI 는 append-only 로 유지되고 기존 호출자는 그대로다.**

### 2. coverage 유닛 읽기의 여분 lseek 두 번 (2026-08-10)

**프로파일 근거.** 호출 귀속에서 측정 하네스의 파일 I/O 가 4.4% 였고 그중 `fseek` 이 2.9% 였다. `coverage_read_file` 이 크기를 알려고 끝으로 seek 하고 되돌아왔다. 파일당 lseek 두 번이 순수 부가다.

**변경.** POSIX 에서 `open`/`fstat`/`read` 로 바꿨다. Windows 는 버퍼 스트림 경로를 그대로 뒀다. `cli/` 는 소유자가 없어 직접 했다.

**효과.** val 유닛 10회분(10,500 유닛)에서 두 바이너리를 **번갈아** 12회씩 돌린 최소값이다. 한쪽을 몰아 돌리면 그 사이의 기계 드리프트가 부호를 뒤집는다(실제로 첫 두 시도에서 -2.2% 와 +1.4% 가 나왔다). 번갈아 재야 한다.

| | 최소 wall (10,500 유닛) |
|---|---|
| 전 | 3,760 ms |
| 후 | 3,719 ms |
| 차 | **-1.09%** |

digest 는 양쪽 `3843466845` 로 같다. 측정 대상은 안 변했다.

**프로파일이 2.9% 를 말했는데 1.1% 만 나온 이유를 적어 둔다.** VTune 의 `fseek` self time 은 페이지 캐시가 따뜻한 상태에서도 함수 진입 비용을 포함해 잡히고, 제거한 것은 그중 실제 lseek 시스템 콜 몫뿐이다. **평탄한 self time 은 상한이지 예산이 아니다.** 이 워크스트림에서 프로파일 수치를 기대 이득으로 바로 쓰지 않는 근거다.

## 병렬성 (2026-08-10)

G6 은 "병렬로 돌아야 하는 구간이 실제로 병렬로 도는가" 를 묻는다. VTune threading 리포트는 밖에서 답하고, `scripts/perf/bench-batch.py` 는 안에서 답한다. 둘은 중복이 아니다. 하네스는 **배치 API 가 실제로 내주는 것**을 재고, 프로파일러는 **못 내주고 있는 스레드가 어디 갔는지**를 잰다. 그리고 하네스는 프로파일러 없이 아무 데서나 돈다.

명령: `PYTHONPATH=out/build/linux-clang/bindings/python/package python3 scripts/perf/bench-batch.py 48`

48쌍, 전부 서로 다른 소스, 전부 `proved-equivalent`. WSL 24 논리 코어.

| workers | wall | pairs/s | ms/pair | speedup | occupancy |
|---|---|---|---|---|---|
| 1 | 4.185s | 11.5 | 87.18 | 1.00x | 100.0% |
| 2 | 2.333s | 20.6 | 48.60 | 1.79x | 99.9% |
| 4 | 1.409s | 34.1 | 29.35 | 2.97x | 99.9% |
| 8 | 1.027s | 46.8 | 21.39 | 4.08x | 99.6% |
| 16 | 0.896s | 53.5 | 18.67 | 4.67x | 98.0% |

**병렬로 돌기는 돈다.** GIL 은 판정 내내 풀려 있고 점유율이 16 워커에서도 98% 다. 스레드 풀이 노는 것이 아니다.

**그런데 24 코어에서 4.67배에서 포화한다.** 점유율이 100% 인데 speedup 이 안 붙는다는 것은 **스레드가 놀아서가 아니라 스레드마다 같은 일을 더 오래 하고 있다**는 뜻이다. 판정당 시간이 87ms 에서 약 293ms 로 3.4배 늘었다. 점유율과 speedup 을 같이 봐야 이 구분이 보인다.

### 4.67 배가 기계의 한계인가

**아니다.** 4.67 이라는 숫자는 이 기계가 우리 종류의 일에 얼마를 주는지 모르면 뜻이 없다. 대조군을 세웠다: `scripts/perf/bench-scaling.sh` 는 **완전히 독립된 K 개 프로세스**가 각자 split 전체를 파싱하고 로어링하게 한다. 공유 상태도, 락도, 파이프도, 자식도 없다. 우리 코드가 경합할 것이 아무것도 없는 구성이다.

명령: `./scripts/perf/wsl.sh scripts/perf/bench-scaling.sh val`

| workers | 대조군 (독립 프로세스) | `check_batch` |
|---|---|---|
| 1 | 1.00x | 1.00x |
| 2 | 1.95x | 1.82x |
| 4 | 3.68x | 3.05x |
| 8 | 7.69x | 4.12x |
| 16 | **10.97x** | **4.65x** |

같은 기계, 같은 순간, 같은 24 논리 코어다. **대조군은 10.97 배까지 간다.** 배치 경로는 4.65 배에서 멈춘다. 기계는 2.4 배를 더 줄 수 있는데 배치 경로가 그것을 못 쓰고 있다. **이 격차는 기계의 것이 아니라 우리(또는 Bitwuzla)의 것이다.**

**solver 프로세스 spawn 은 원인이 아니다.** 별도 프로브로 격리했다(`bitwuzla --version` 48회).

| | workers=1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| 9p (`/mnt/c`) | 5.45 ms | 2.15 | 2.12 | 0.87 | 0.52 |
| 네이티브 (`/tmp`) | 0.92 ms | 0.48 | 0.27 | 0.32 | 0.31 |

spawn 은 9p 에서 순차 5.45ms 로 비싸지만 **병렬화가 잘 되고**(16 워커에서 0.52ms) 판정당 87ms 대비 작다. 직렬화 지점이 아니다. solver 는 파이프로 붙고 임시 파일을 쓰지 않는다(`src/solver.c:1660` 부근).

**그래서 남은 후보는 둘이었다.** (a) Bitwuzla 프로세스와 기계가 천장이다. (b) 우리 프로세스 안에 워커끼리 다투는 지점이 있다.

### 스레드 대 프로세스로 그 둘을 갈랐다

`scripts/perf/bench-processes.sh` 는 **같은 판정을 스레드가 아니라 별개 프로세스로** 돌린다. 별개 프로세스는 인터프리터도, 할당자도, 코어의 전역도 공유하지 않는다. 우리 프로세스 안의 경합이 원인이라면 프로세스는 스레드가 못 간 곳까지 가야 한다.

명령: `./scripts/perf/wsl.sh scripts/perf/bench-processes.sh 16`

| workers | 스레드 (`check_batch`) | 프로세스 |
|---|---|---|
| 1 | 11.4 pairs/s | 9.7 pairs/s |
| 8 | 47.0 | 45.6 |
| 16 | **53.1** | **61.7** |

**둘 다 같은 대역(53~62 pairs/s)에서 멈춘다.** 워커 1개 기준(11.4 pairs/s)에서 선형이었다면 16 워커는 180 pairs/s 여야 한다. 프로세스도 거기 못 간다.

**결론은 둘로 나뉜다.**

1. **격차의 대부분은 스레딩 모델 탓이 아니다.** 공유를 전부 끊어도 천장이 거의 그대로다. 이 천장은 solver 프로세스와 기계의 것이다. **즉 `check_batch` 는 이 워크로드가 이 기계에서 낼 수 있는 것을 대체로 내주고 있고, 배치 API 자체를 고칠 일이 아니다.**
2. **다만 프로세스가 16 워커에서 16% 앞선다**(61.7 대 53.1). 그만큼은 우리 프로세스 안의 경합일 수 있다. **작지만 0 은 아니다.** 어디인지는 판정 안을 봐야 알고, 그것이 이 호스트에서 못 하는 측정이다(자식 프로세스 제약, 아래). **측정 전에는 단정하지 않는다. 이 16% 를 짚는 것이 W9 의 VM threading 리포트가 할 첫 번째 일이다.**

프로세스 쪽 워커당 절대 처리량이 낮은 것은 워커마다 인터프리터 시작과 backend 프로브를 따로 물기 때문이다. 곡선의 모양이 논점이다.

## 건전성 확인

튜닝이 판정을 바꾸지 않았음을 매번 전 구성으로 확인한다. `main` (`fbfac09`) 을 병합한 2026-08-10 기준:

| 구성 | 결과 |
|---|---|
| windows-clang | 292/292 |
| linux-clang | 291/291 |
| linux-sanitize (ASan + UBSan, `-fno-sanitize-recover=all`) | 291/291 |

`linux-sanitize` 프리셋은 한동안 libuv 의 `-fPIC` 누락으로 파이썬 확장 링크가 실패했다(`relocation R_X86_64_PC32 against uv_ip6_addr can not be used when making a shared object`). W8 이 보고하고 조율자가 `d04cb80` 에서 전역 `POSITION_INDEPENDENT_CODE` 로 고쳤다. 지금은 프리셋 그대로 쓰면 된다.

## 병합 후 회귀 확인 (2026-08-10, `main` `fbfac09` 병합)

`main` 이 `src/product.c`, `src/replay.c`, `src/signature.c` 에 큰 변경을 들여왔으므로 관문을 다시 돌렸다.

| | 최소 wall (1,050 유닛) | 유닛당 | digest |
|---|---|---|---|
| 병합 전 | 380 ms | 0.3619 ms | 50775242 |
| 병합 후 | 378 ms | 0.3600 ms | 50775242 |

digest 가 같고 시간은 잡음 범위다. 그 변경들은 coverage 경로를 지나지 않는다.

### 유닛당 정의는 하나다

로어링이 **정의마다** 다시 파싱하므로 한 유닛에 정의가 여럿이면 중복 배수가 커진다. 이 코퍼스에서는 안 커진다. `tools/corpus/extract.py:63` 의 `build_unit` 이 유닛 하나를 **context 선언(원형만) + target 본문 하나**로 만들기 때문에 정의 수와 유닛 수가 구조적으로 같다(val 에서 1,050 = 1,050 확인). **따라서 val 의 호출 귀속은 train 에도 그대로 적용되고, 유닛당 파스는 정확히 두 번이다.** 쫓을 배수가 따로 없다.

## 판정 87ms 의 내부: 스냅샷 경로 (2026-08-10)

`ql_py_check` 는 판정마다 registry 를 새로 만든다(`bindings/python/src/ql_check.c:506`). 따라서 Bitwuzla backend state 도 판정마다 만들어지고 없어진다. 그 state 는 선택된 실행 파일을 개인 임시 디렉터리로 **복사하고**, 생성 시 한 번, 버전 프로브 중 한 번, solver check 전과 중에 한 번씩 **총 네 번 해시한다**(`src/solver.c:2341..2761`). 실행 파일은 4,490,928 바이트다.

`scripts/perf/snapshot_probe.c` 가 그 파일 작업만 같은 원시연산으로 재현한다. 푸는 것은 아무것도 없다. 명령: `./scripts/perf/wsl.sh scripts/perf/bench-snapshot.sh 20`

| 원본 위치 | copy | hash x4 | 판정당 합 | 1 워커 | 8 워커 | 16 워커 |
|---|---|---|---|---|---|---|
| 이 워크트리 (`/mnt/c`, 9p) | 27.8 ms | 17.0 ms | **44.9 ms** | 37.3 ms | 72.4 ms | **129.8 ms** |
| 네이티브 디스크 (`/tmp`) | 1.5 ms | 16.8 ms | **18.4 ms** | 16.0 ms | 18.8 ms | **24.5 ms** |

**두 가지가 나온다.**

### 1. 이 워크트리의 위치가 측정을 오염시키고 있었다

원본이 9p 위에 있어 복사가 1.5ms 가 아니라 27.8ms 다. 그리고 **부하에서 무너진다**(37 → 130 ms). 공유 파일시스템이라 워커가 늘수록 나빠진다. 위 병렬성 절의 판정당 87ms 와 16 워커에서의 293ms 에는 이 통행료가 섞여 있다. **네이티브 체크아웃에서는 이 몫이 거의 없다.** 앞 절의 "천장은 solver 와 기계의 것" 이라는 결론은 이 통행료를 solver 몫으로 잘못 넘겼을 수 있다. 다음 절이 그것을 실제로 검증한다.

### 2. 9p 를 걷어내도 스냅샷은 판정당 18.4ms 다

그중 16.8ms 가 4.49MB 를 네 번 해시하는 값이다. 이것은 환경 artifact 가 아니라 어디서 돌리든 내는 값이고, 질의 내용과 무관한 고정 부가다.

**해시가 느린 이유는 BLAKE3 가 SIMD 없이 빌드되기 때문이다**(`third_party/CMakeLists.txt:123` 이 `BLAKE3_NO_SSE2`/`SSE41`/`AVX2`/`AVX512` 를 전부 건다). 벤더링된 소스에는 x86-64 어셈블리가 이미 들어 있다.

| BLAKE3 빌드 | hash x4 | 스냅샷 합 | digest |
|---|---|---|---|
| portable (현재) | 16.1 ms | 17.6 ms | `0a989102d9dc2892826c4ae4d6148814b123a2dabe4e723bb1bd68dac57f94a9` |
| x86 SIMD | 3.5 ms | 4.7 ms | `0a989102d9dc2892826c4ae4d6148814b123a2dabe4e723bb1bd68dac57f94a9` |

**digest 가 같다.** 추정이 아니라 프로브가 찍어서 비교한 값이다. BLAKE3 는 구현이 달라도 같은 값을 낸다. 스냅샷의 존재 이유가 신원이므로 이 확인 없이는 속도 얘기를 꺼낼 수 없다. **신원은 그대로이고 해시가 4.6배 빨라진다.**

`third_party/CMakeLists.txt` 는 조율자 소유이므로 이 변경은 합의 사항이다.

## 아직 없는 것

- **threading 리포트.** 워크로드는 준비됐다(`scripts/perf/bench-batch.py`). **WSL 에서 VTune 수집은 자식 프로세스를 띄우는 워크로드에서 걸린다.** 2026-08-10 에 두 번째 확인을 얻었다: 인수 시에는 threading 수집만 걸린다고 알려져 있었는데, **hotspots 수집도 같은 배치 워크로드에서 똑같이 매달렸다**(무출력, 결과 디렉터리는 생기지만 리포트가 안 나옴, 손으로 `pkill` 해야 끝남). 반면 자식을 안 띄우는 coverage 워크로드는 hotspots 가 정상이다. **가르는 것은 수집 종류가 아니라 자식 프로세스다.** threading 리포트는 G7 의 실제 Linux VM 에서 뜬다.
- **prove 경로의 함수 단위 프로파일.** 위 표로 규모와 확장 한계는 잡았지만, 판정당 87ms 가 프런트엔드/로어링/miter/SMT-LIB 직렬화/solver 중 어디로 가는지는 위 제약 때문에 이 호스트에서 못 잰다. VM 대기.

## 재현

한 번만 (WSL Ubuntu 24.04, 부팅마다):

```sh
sudo sysctl -w kernel.yama.ptrace_scope=0
```

코퍼스를 이 워크트리에 추출하고 Linux 바이너리를 만든다.

```sh
python tools/corpus/extract.py --corpus "$QL_CORPUS" --split val --out out/corpus/val
cmake --preset linux-clang && cmake --build --preset linux-clang --parallel
```

**회귀 관문.** 프로파일러 없이 총량만 잰다. 5회 중 최소값과 결과 digest 를 찍는다.

```sh
# Linux 에서
./scripts/perf/bench-coverage.sh val 5
# Windows Git Bash 에서 WSL 을 통해
./scripts/perf/wsl.sh scripts/perf/bench-coverage.sh val 5
```

**귀속 분석.** VTune hotspots 를 스택 수집과 함께 건다. 유닛 목록을 10회 이어붙여 표본을 확보한다.

```sh
./scripts/perf/wsl.sh scripts/perf/vtune-hotspots.sh val 10
```

`scripts/coordinator/vtune-wsl.sh` 는 뒤쪽으로 넘기는 shim 으로 남겨 두었다.

## 회귀를 재는 방법

1. **관문은 wall-clock 최소값이다.** 평균이 아니라 최소값을 쓴다. 최소값이 스케줄러 잡음에 가장 덜 오염된 추정이다.
2. **digest 가 같아야 시간 비교가 성립한다.** `bench-coverage.sh` 는 coverage JSON 의 `cksum` 을 같이 찍는다. digest 가 바뀌었으면 빨라진 것이 아니라 **덜 한 것**이므로 그 비교는 무효다.
3. **회귀 판정 기준은 유닛당 시간 5% 악화**다. 같은 기계, 같은 코퍼스, 같은 프리셋에서 잰다. 관측된 실행 간 산포는 2% 미만이다(위 기준선에서 380..387 ms).
4. 원인을 알아야 할 때만 VTune 을 건다. 수집은 항상 볼 것만 좁혀서 백그라운드 + 120초 제한으로 건다.
5. 모든 최적화 커밋은 before/after 를 이 문서에 잇는다. **프로파일 근거 없는 최적화는 넣지 않는다.**
