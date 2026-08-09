# 성능 기준선

시작 2026-08-10. G6 의 기준선 문서입니다. 수치를 갱신할 때는 아래 재현 명령을 그대로 다시 돌리고 이 문서에 날짜와 함께 잇습니다. 2026-08-10 부터 W8 이 소유합니다(`docs/workstreams/W8.md`).

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

**solver 프로세스 spawn 은 원인이 아니다.** 별도 프로브로 격리했다(`bitwuzla --version` 48회).

| | workers=1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| 9p (`/mnt/c`) | 5.45 ms | 2.15 | 2.12 | 0.87 | 0.52 |
| 네이티브 (`/tmp`) | 0.92 ms | 0.48 | 0.27 | 0.32 | 0.31 |

spawn 은 9p 에서 순차 5.45ms 로 비싸지만 **병렬화가 잘 되고**(16 워커에서 0.52ms) 판정당 87ms 대비 작다. 직렬화 지점이 아니다. solver 는 파이프로 붙고 임시 파일을 쓰지 않는다(`src/solver.c:1660` 부근). 남은 후보는 Bitwuzla 자체의 메모리 대역폭/SMT 경합과 24 논리 코어의 물리 12 코어 뿐이다. **측정 전에는 단정하지 않는다.** 이 구분은 W9 의 VM threading 리포트가 답할 몫이다.

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
