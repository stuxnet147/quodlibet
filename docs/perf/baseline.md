# 성능 기준선

시작 2026-08-10. G6 의 기준선 문서입니다. 수치를 갱신할 때는 아래 재현 명령을 그대로 다시 돌리고 이 문서에 날짜와 함께 잇습니다.

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

### 남은 알려진 중복: 프런트엔드와 로어링의 이중 파스

유닛당 아직 **두 번** 파싱한다. `ql_c_frontend_analyze` 와 `ql_c_lower_selected_function` 이 각각 자기 파서로 같은 소스를 파싱한다. 파싱이 CPU 의 절반 이상이므로 트리를 한 번 만들어 넘기는 API(append-only 로 `_with_tree` 변형 추가)가 다음으로 큰 단일 최적화다. **이 API 는 W1 소유 파일에 있으므로 W1 의 작업 단위로 넘긴다.** 포인터 로어링이 끝난 뒤가 맞다. 지금 그 파일들은 활발히 바뀌는 중이다.

## 아직 없는 것

- **threading 리포트.** 병렬 소비자가 아직 없다. 파이프라인 스케줄러는 시험에서만 돌고, 실제 병렬 워크로드는 W4 의 `check_batch` 가 생기면 그것으로 잰다. 그때 worker 점유율로 "병렬로 돌아야 하는 구간이 실제로 병렬로 돈다" 를 확인한다.
- **prove 경로 프로파일.** solver 왕복이 지배할 것으로 예상되지만 측정 전에는 적지 않는다.

## 재현

```sh
# WSL Ubuntu 24.04. 한 번만:
sudo sysctl -w kernel.yama.ptrace_scope=0

cd /mnt/d/projects/machine-model/python/quodlibet
cmake --build --preset linux-clang --parallel

# 유닛을 네이티브 디스크로
rm -rf /tmp/qlunits && cp -r out/corpus/val/units /tmp/qlunits
ls /tmp/qlunits | sed 's|^|/tmp/qlunits/|' > /tmp/qlunits.txt

VT=/opt/intel/oneapi/vtune/latest/bin64/vtune
rm -rf /tmp/vt-hs && $VT -collect hotspots -result-dir /tmp/vt-hs \
    -- ./out/build/linux-clang/quodlibet coverage /tmp/qlunits.txt
$VT -report summary -r /tmp/vt-hs | grep -E 'Elapsed|CPU Time'
```

회귀 판정은 같은 코퍼스에서 CPU Time 비교로 한다. 수집은 항상 볼 것만 좁혀서 백그라운드 + 120초 제한으로 건다.
