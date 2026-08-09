# W10 진행 기록: AIG/SAT backend 와 certificate checker

브랜치: `stuxnet147/w10-aig-sat`
지시서: `docs/workstreams/W10.md`

## 지금 하는 것

1단계(조사) 결과를 `docs/notes/sat-backend-and-proof-checker.md` 에 남기고 조율자 확인을 기다립니다.

## 끝난 작업 단위

### 1. SAT solver / proof 형식 / checker 후보 비교 (커밋 대기)

`docs/notes/sat-backend-and-proof-checker.md`.

권고는 **경로 A** 입니다.

- solver: Kissat rel-4.0.4 (MIT, C99, DRAT 만 냄)
- elaborator: drat-trim (MIT, C 한 파일). **신뢰하지 않습니다**
- checker: `lrat-check` (drat-trim 저장소 동봉, MIT, C 한 파일 약 18KB). **여기에만 신뢰가 남습니다**

핵심 논거는 drat-trim 의 DRAT -> LRAT elaborate 출력을 원본 CNF 에 대해 `lrat-check` 로 다시 검사하면 drat-trim 이 틀려도 통과하지 못한다는 것입니다. 그래서 Kissat 을 유지하면서도 TCB 가 `lrat-check.c` 한 파일로 줄어듭니다. CaDiCaL 의 native LRAT(경로 B)도 TCB 가 같은 크기인데, 이미 `DEPENDENCIES.md` 와 `THIRD_PARTY_NOTICES.md` 에 적힌 Kissat pin 을 뒤집을 만큼의 이득이 없습니다.

cake_lpr(경로 C, HOL4 로 기계어까지 검증)은 첫 절단에 넣지 않고, **checker 를 method option 으로 교체 가능한 경계로 설계**해서 나중에 붙일 수 있게 둡니다.

## 내린 설계 결정

- **신뢰는 checker 한 곳으로 모읍니다.** solver 도 elaborator 도 신뢰하지 않습니다. 이것이 이 워크스트림이 `prove.smt-product` 와 다른 유일한 이유입니다
- **LRAT 를 씁니다.** DRAT checker 는 전파 탐색을 스스로 해야 해서 커집니다. LRAT 는 antecedent 가 붙어 있어 checker 가 재생만 하면 되고, 그래서 감사 가능한 크기가 됩니다
- **solver 와 checker 는 실행 파일입니다.** `libquodlibet` 은 둘 다 링크하지 않습니다. 기존 Bitwuzla adapter(`src/solver.c`)의 snapshot / deadline / 출력 상한 패턴을 그대로 따릅니다
- **checker 는 교체 가능합니다.** method option 이 실행 파일과 형식을 받고, envelope 이 어떤 checker 가 무엇을 검사했는지 기록합니다

## 막힌 것

**조율자 확인 대기 3건.** `docs/notes/sat-backend-and-proof-checker.md` 마지막 절과 같습니다.

1. 경로 A / B / C 중 무엇인가
2. `scripts/vendor.sh` 와 `third_party/CMakeLists.txt` 에 새 벤더를 W10 이 직접 넣어도 되는가. 둘 다 W10 소유가 아닙니다
3. **`CMakeLists.txt` 의 `QL_OPTIONAL_CORE_SOURCES` 에 `src/aig.c`, `src/proof_aigsat.c`, `src/proof_diff.c` 가 없습니다.** `docs/workstreams/README.md` 는 지시서에 적힌 파일 이름이 이미 등록되어 있다고 하지만 실제로는 세 개 다 빠져 있습니다. 조율자가 `CMakeLists.txt` 를 소유하므로 추가를 요청합니다

3번이 닫히기 전에는 코드가 빌드에 들어가지 않습니다. 다만 **2단계(AIG 하강)와 5단계(concrete differential)는 벤더가 필요 없으므로** 1번과 2번 답을 기다리는 동안 그쪽부터 씁니다.

## Windows 이식 위험

Kissat 과 drat-trim 은 둘 다 `unistd.h`, `sys/resource.h`, `sys/time.h` 를 씁니다. `windows-clang` 프리셋은 MSVC ABI 이므로 그대로는 빌드되지 않습니다. tree-sitter 처럼 자체 CMake target 으로 컴파일하면서 얇은 이식 shim 을 넣는 방향으로 봅니다. 실제 규모는 벤더링해 본 뒤 여기에 적습니다.

## 다음에 할 것

1. 조율자 답을 받는다
2. 2단계. loop-free scalar IR 의 AIG 하강(`src/aig.c`). 메모리를 쓰는 IR 은 인코딩하지 않고 `UNKNOWN`
3. 5단계. `refute.concrete-differential`(`src/proof_diff.c`). `ir_interp` 로 좌우를 돌려 반례를 찾음. 반례는 정의상 replay 된 것이므로 바로 `COUNTEREXAMPLE`, 못 찾으면 증거가 아닌 `UNKNOWN`
4. 3단계와 4단계. miter -> CNF, solver 와 checker 실행, checker 통과 뒤에만 `checked_proof=true`
