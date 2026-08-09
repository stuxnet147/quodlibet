# SAT solver, proof 형식, proof checker 후보 비교

작성: 2026-08-10, 워크스트림 W10 (`docs/workstreams/W10.md` 1단계)

이 문서는 `prove.aig-sat` 이 벤더링할 SAT solver 와 UNSAT proof 형식과 독립 checker 를 고르기 위한 비교입니다. 결정은 조율자가 합니다.

## 왜 고르는지

`prove.smt-product` 의 `PROVED_*` 는 Bitwuzla 를 명시적으로 신뢰해서 나옵니다. Bitwuzla 0.9.1 은 proof object 를 내놓지 않으므로 검증할 certificate 자체가 없고, envelope 은 항상 `checked_proof=false` 를 기록합니다(`METHODS.md` UNSAT promotion boundary).

AIG/SAT 경로의 존재 이유는 그 신뢰를 **줄이는 것**입니다. 그래서 고르는 기준의 1순위는 solver 의 속도가 아니라 **최종 신뢰 기반(TCB)의 크기와 감사 가능성**입니다. solver 는 신뢰하지 않아도 되는 자리에 놓고, 신뢰는 checker 하나로 모읍니다.

## 판단 축

1. **라이선스.** 정적 링크와 재배포에 relink 의무가 없어야 합니다. GPL/LGPL 은 `DEPENDENCIES.md` 의 zf_log 선정 때와 같은 이유로 탈락입니다.
2. **TCB.** `PROVED_*` 가 무엇을 신뢰하는지. solver 본체를 신뢰해야 하면 Bitwuzla 와 다를 게 없습니다.
3. **크로스플랫폼.** Windows(clang, MSVC ABI)와 Linux 양쪽. 이것이 실제로 가장 위험한 축입니다.
4. **벤더 형태.** 소스 pin 후 자체 CMake target 으로 컴파일(tree-sitter 방식)인지, 공식 바이너리 번들 pin(Bitwuzla 방식)인지.
5. **크기.** 저장소가 작은 pin 된 dependency 만 벤더링하는 방침입니다.

## SAT solver 후보

| 후보 | 라이선스 | 언어 | 내는 proof | 공식 Windows 바이너리 | 비고 |
|---|---|---|---|---|---|
| Kissat rel-4.0.4 | MIT | C99 | **DRAT 만** (파일이면 binary, `-` 면 ASCII) | 없음 | `DEPENDENCIES.md` 가 이미 "initial SAT candidate" 로 적어 둔 후보. 순수 C |
| CaDiCaL 2.x | MIT | C++11 | DRAT, **native LRAT** (`--lrat`) | 없음 | LRAT 를 solver 안에서 직접 생성. LRAT 를 켜면 일부 inprocessing(BVE, hyper-ternary resolution, vivification)을 꺼야 함 |
| MiniSat / Glucose 계열 | MIT / 수정 MIT | C++ | DRUP/DRAT | 없음 | 성능이 두 세대 뒤. 고를 이유 없음 |
| CryptoMiniSat | MIT | C++ | DRAT | 있음 | 의존성(boost, m4ri 등)이 이 저장소 방침에 비해 큼 |

Kissat 문서가 말하는 proof 형식은 DRAT 뿐입니다(manpage: proof 파일을 주면 trace 를 쓰고, `-` 면 ASCII DRAT, 파일이면 binary). LRAT 언급은 없습니다.

CaDiCaL 은 Freiburg 그룹이 "native LRAT" 를 명시적으로 홍보하는 구현이고 `--lrat` 옵션이 그것입니다.

**자체 SAT solver 는 만들지 않습니다.** SMT 와 같은 원칙입니다(`AGENTS.MD`).

## proof 형식

- **DRAT.** solver 가 배운 절만 나열합니다. 검사할 때 checker 가 매 절마다 RUP 전파를 다시 돌려야 하므로 비싸고, checker 자체가 커집니다(drat-trim 은 약 60KB 짜리 C 한 파일).
- **LRAT.** 각 절에 그 절을 유도하는 **antecedent 절 번호 목록**이 붙습니다. checker 는 전파 탐색을 하지 않고 주어진 순서대로 unit propagation 을 재생만 하면 됩니다. 그래서 checker 가 작고(약 18KB 짜리 C 한 파일) 감사 가능합니다.
- **LPR.** LRAT 의 상위 집합. PR 절 추가를 지원합니다. cake_lpr 이 받는 형식이고 LRAT 를 그대로 받습니다.

**우리 목적에는 LRAT 가 맞습니다.** 신뢰를 checker 한 개로 모으는 것이 목적이고, LRAT checker 가 DRAT checker 보다 훨씬 작습니다.

## checker 후보

| 후보 | 라이선스 | 형태 | 받는 형식 | TCB 로서의 크기 |
|---|---|---|---|---|
| `drat-trim` | MIT | C 한 파일, 약 60KB | DRAT | 큼. 후진 검사, 타임아웃, 최적화가 들어 있음 |
| `lrat-check` (drat-trim 저장소 동봉) | MIT | C 한 파일, 약 18KB | LRAT | 작음. 원본 CNF 와 LRAT 를 받아 처음부터 재생 |
| `cake_lpr` | 저장소 확인 필요 | CakeML 이 뽑은 기계어 | LPR(LRAT 포함) | 가장 작음. HOL4 로 기계어까지 검증됨. 단 소스 벤더가 아니라 플랫폼별 바이너리 pin |

### drat-trim 을 신뢰하지 않고 쓰는 법

`drat-trim -L out.lrat cnf.cnf proof.drat` 는 DRAT 를 LRAT 로 **elaborate** 합니다. 그 출력 LRAT 를 원본 CNF 에 대해 `lrat-check` 로 다시 검사하면, drat-trim 이 틀렸거나 악의적이어도 검사는 통과하지 못합니다. 즉 **drat-trim 은 신뢰하지 않는 도우미**로 쓸 수 있고, 신뢰는 `lrat-check` 에만 남습니다.

이것이 중요한 이유는 Kissat 을 유지하면서도 TCB 를 `lrat-check` 하나로 줄일 수 있다는 뜻이기 때문입니다.

## 경로 세 가지

### 경로 A. Kissat(DRAT) + drat-trim(신뢰 안 함, elaborate) + lrat-check(신뢰)

- 벤더 단위 2개: `kissat`, `drat-trim`(안에 `drat-trim.c` 와 `lrat-check.c` 둘 다 있음)
- 전부 MIT, 전부 C
- `DEPENDENCIES.md` 와 `THIRD_PARTY_NOTICES.md` 에 이미 적힌 Kissat pin 을 그대로 지킴
- TCB = `lrat-check.c` 한 파일
- 비용: elaborate 단계가 추가 프로세스이고 큰 proof 에서 느립니다
- 위험: Kissat 과 drat-trim 둘 다 POSIX 헤더(`unistd.h`, `sys/resource.h`, `sys/time.h`)를 씁니다. Windows clang 빌드에 이식 shim 이 필요합니다

### 경로 B. CaDiCaL(native LRAT) + lrat-check(신뢰)

- 벤더 단위 2개: `cadical`, `drat-trim`(에서 `lrat-check.c` 만 씀)
- TCB = `lrat-check.c` 한 파일, 경로 A 와 같음
- elaborate 단계가 없어 파이프라인이 한 단계 짧습니다
- 비용: CaDiCaL 은 C++11 이라 solver 실행 파일이 C++ 런타임을 답니다. **subprocess 로만 쓰므로 `libquodlibet` 은 계속 C17 순수입니다**(Bitwuzla 와 같은 경계)
- 비용: `--lrat` 는 일부 inprocessing 을 끄므로 같은 문제에서 Kissat 보다 느릴 수 있습니다
- 이미 문서에 적힌 Kissat pin 을 바꿔야 하므로 `DEPENDENCIES.md` 와 `THIRD_PARTY_NOTICES.md` 수정이 따라옵니다

### 경로 C. 위 둘 중 하나 + cake_lpr 을 최종 checker 로

- TCB 가 형식 검증된 기계어로 줄어듭니다. 이 저장소가 낼 수 있는 가장 강한 `checked_proof=true`
- 비용: 소스 벤더가 아니라 플랫폼별 바이너리 pin 이고(Bitwuzla 방식), 라이선스와 Windows 바이너리 제공 여부를 저장소에서 직접 확인해야 합니다
- 첫 절단에 넣기에는 확인할 것이 많습니다. **checker 를 교체 가능한 옵션으로 설계해 두고 나중에 붙이는 것을 권합니다**

## 권고

**경로 A 를 권합니다.**

이유는 이렇습니다.

1. TCB 가 경로 B 와 동일하게 `lrat-check.c` 한 파일입니다. 신뢰 축소라는 이 워크스트림의 목적을 똑같이 달성합니다.
2. `DEPENDENCIES.md` 와 `THIRD_PARTY_NOTICES.md` 가 이미 Kissat rel-4.0.4 를 적어 두었습니다. 결정을 뒤집을 만큼의 이득이 경로 B 에 없습니다.
3. 벤더 전부가 순수 C 입니다. 저장소가 C17 코어를 유지하는 방침과 어긋나지 않고, Windows 이식 shim 도 C++ 런타임 없이 끝납니다.
4. drat-trim 을 신뢰하지 않아도 되므로, 그 크기가 판단에 들어오지 않습니다.

그리고 **checker 를 처음부터 교체 가능한 경계로 설계합니다.** method option 이 checker 실행 파일과 형식을 받고, envelope 이 어떤 checker 가 무엇을 검사했는지 기록합니다. 나중에 cake_lpr 을 붙일 때 method 를 다시 쓰지 않아도 됩니다(경로 C).

## 실행 형태

기존 Bitwuzla adapter(`src/solver.c`)의 패턴을 그대로 따릅니다.

- shell 없이 argument vector 와 pipe 로 실행
- snapshot 디렉터리에 CNF 와 proof 를 쓰고 끝나면 정리
- deadline 과 출력 상한을 걸고 초과는 `UNKNOWN`
- 실행 파일의 내용 digest 를 evidence 에 기록하고 cache key 에 넣음

`libquodlibet` 은 solver 도 checker 도 링크하지 않습니다. 둘 다 실행 파일입니다.

## 판정 규칙

- solver 가 `s UNSATISFIABLE` 을 냈고 **checker 가 그 LRAT 를 원본 CNF 에 대해 통과시킨 뒤에만** `PROVED_*` 이며 그때 `checked_proof=true` 입니다
- checker 가 없거나 실패하거나 시간 초과면 raw UNSAT 은 solver evidence 로만 남고 verdict 는 `UNKNOWN` 입니다
- solver 가 `s SATISFIABLE` 을 내면 배정을 typed input 으로 디코드해 `ir_interp` 로 replay 한 뒤에만 `COUNTEREXAMPLE` 입니다
- 메모리를 쓰는 IR, 루프, 재귀, 외부 호출은 첫 절단에서 인코딩하지 않고 `UNKNOWN` 입니다. 유한 절단을 썼으면 `BOUNDED_CLEAN` 이며 `PROVED_*` 로 올리지 않습니다

## 결정 기록: 경로 A 는 측정으로 기각되었고 CaDiCaL 로 갔다 (2026-08-10)

위의 권고는 **틀렸습니다.** 조율자가 경로 A 를 승인했고 Windows 이식을 재는 단계에서 Kissat 이 실격했습니다. 이 절은 그 근거를 남깁니다.

### Kissat rel-4.0.4 실격 근거

pin 은 tarball SHA-256 `bfe93eaa6323b48011e4b1fcf74b3f2e20f9de544767e728009e5b2018296193` 입니다.

**1. MSVC ABI 에서 자료구조가 깨진다.** `windows-clang` 프리셋으로 93개 소스 중 92개가 약 60줄 shim 으로 컴파일되지만, `-O0` 빌드가 kissat 자신의 assertion 에서 멈춥니다.

```
Assertion failed: sizeof (watch) == sizeof (unsigned), file inline.h, line 79
```

`watch` union 의 bitfield 가 GNU 배치를 전제합니다. MSVC ABI 는 bitfield 의 기반 타입이 바뀌면 새 저장 단위를 잡으므로 4바이트를 넘깁니다. `-DNDEBUG` 빌드는 그 검사가 없어 그대로 풀이에 들어가 SIGSEGV 로 죽습니다. `-mno-ms-bitfields` 는 Windows SDK 헤더가 `error: Itanium-compatible layout for the Microsoft C++ ABI is not yet supported` 로 거부합니다.

**2. MinGW 빌드는 틀린 답을 낸다.** `x86_64-w64-mingw32-gcc 13`(GNU bitfield 배치)으로는 서고 돌지만, 다음 식에 `s SATISFIABLE` 과 `v 1 2 0` 을 냅니다.

```
p cnf 2 4
1 2 0
-1 2 0
1 -2 0
-1 -2 0
```

네 절이 두 변수의 네 배정을 전부 막으므로 UNSAT 이고, 내놓은 model 은 네 번째 절을 만족시키지 않습니다. 원인은 특정하지 않았습니다.

**3. 왜 이것이 단순한 빌드 실패보다 무거운가.** checker 는 틀린 UNSAT proof 를 잡습니다. **틀린 SAT 답은 아무것도 잡지 않습니다.** replay 가 마지막에 걸러내기는 하지만, 그때는 이미 UNSAT 였어야 할 질문에 SAT 를 답한 solver 를 신뢰 사슬 안에 들인 뒤입니다. 신뢰를 줄이려고 만든 경로가 오답 solver 를 들이는 것은 자기모순입니다.

### CaDiCaL 2.2.1 채택

pin 은 tarball SHA-256 `16d24cc143632b9990a3fbe062e2858d5dd9599a0f369dc02a40c2a76036f931` 입니다. **벤더링 전에 같은 프로브를 양 플랫폼에서 돌렸습니다.**

| 프로브 | Windows (`windows-clang`, MSVC ABI) | Linux (WSL Ubuntu 24.04, gcc) |
|---|---|---|
| 사소 SAT (2변수 2절) | `s SATISFIABLE`, 코드 10 | 같음 |
| 사소 UNSAT (Kissat 이 틀린 그 식) | `s UNSATISFIABLE`, 코드 20 | 같음 |
| 중간 UNSAT (pigeonhole 9/8, 297절) | 코드 20, LRAT 70,701줄, `lrat-check` **VERIFIED** | 코드 20, LRAT 69,002줄, **VERIFIED** |
| 중간 SAT (무작위 3-SAT, 250변수 875절) | `s SATISFIABLE`, model 을 따로 검증해 미충족 절 0개 | `s SATISFIABLE` |

CaDiCaL 3.0.1(`0a8ea563b5a25f5aa064634814edab45cc0e45111ea0f5d412a565f806fd7e11`)도 같은 프로브를 전부 통과했습니다. 조율자가 2.x 를 지정했고 2.2.1 이 더 오래 검증된 계열이라 그쪽을 pin 했습니다.

### 사슬이 짧아졌다

CaDiCaL 이 LRAT 를 직접 내므로 **drat-trim 이 사슬에서 빠졌습니다.** 원래 경로 A 는 kissat(DRAT) -> drat-trim(elaborate) -> lrat-check 세 프로세스였고, 지금은 CaDiCaL(LRAT) -> lrat-check 두 프로세스입니다. TCB 는 그대로 `lrat-check.c` 한 파일입니다.

### Windows 이식 형태

pin 한 소스는 **고치지 않습니다.** `third_party/quodlibet-compat/` 이 Windows 에 없는 헤더 넷(`unistd.h`, `strings.h`, `sys/time.h`, `sys/resource.h`)과 prelude 하나를 대신 줍니다. pin 을 고치면 checksum 이 빌드되는 것과 다른 것을 가리키게 되므로, 없는 것을 채우는 쪽을 택했습니다. shim 이 채우는 시간과 메모리 값은 통계 출력에만 쓰이고 판정에 닿지 않습니다. 링크에는 32MB 스택이 필요합니다.

## 출처

- Kissat manpage (proof 형식): <https://manpages.debian.org/testing/kissat/kissat.1.en.html>
- Kissat 저장소: <https://github.com/arminbiere/kissat>
- CaDiCaL native LRAT: <https://cca.informatik.uni-freiburg.de/lrat/>
- Pollitt, Fleury, Biere, "Faster LRAT Checking Than Solving with CaDiCaL", SAT 2023: <https://drops.dagstuhl.de/entities/document/10.4230/LIPIcs.SAT.2023.21>
- drat-trim 저장소(`drat-trim.c`, `lrat-check.c`): <https://github.com/marijnheule/drat-trim>
- cake_lpr: <https://github.com/tanyongkiam/cake_lpr>, TACAS 2021 <https://cakeml.org/tacas21.pdf>
