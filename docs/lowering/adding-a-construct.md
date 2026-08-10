# C 구문을 로어링에 추가하는 절차

Quodlibet의 로어링 수용률은 정확성보다 앞서지 않습니다. 새 구문은
파서가 읽는다는 이유만으로 `SUPPORTED`가 되지 않습니다. 아래 절차는
그 구문의 C 의미론, IR 표현, 정의되지 않은 동작, 검증 증거를 한 작업
단위에 묶는 계약입니다.

## 1. 지원 경계를 먼저 적습니다

구현 전에 다음을 정합니다.

- 어떤 `ASM2C_GNU_V1` 구문과 타입 조합을 지원하는가
- 피연산자를 평가하는가, 단락하는가, 또는 전혀 평가하지 않는가
- integer promotion, usual arithmetic conversion, 저장 타입으로의
  되변환 중 무엇이 적용되는가
- 어떤 경우가 C의 undefined behaviour인가
- 아직 말할 수 없는 인접 형태를 어떤 진단 코드의 `UNKNOWN`으로
  남길 것인가

지원 경계가 불분명하면 구현하지 않습니다. 다른 구문처럼 보이는 값을
대입하거나 호스트 컴파일러의 우연한 동작을 의미론으로 사용하지 않습니다.

## 2. 실제 구문 트리를 확인합니다

Tree-sitter C grammar와 `ql_c_syntax_node_view`의 `kind`, `field_name`, 범위를
확인합니다. 같은 표기가 여러 tree shape로 해소되는 경우를 찾습니다.
구문을 인식하지 못한 입력은 기존 catch-all에서 `UNKNOWN`이어야 하며,
새 분기는 자신이 책임지는 shape만 받아야 합니다.

## 3. 타입과 정의성을 기존 한 곳에서 재사용합니다

새 구문은 가능한 한 다음 기존 경로를 사용합니다.

- 타입 철자와 typedef: `resolve_type_node`
- 정수 승격과 이항 변환: `integer_promote`,
  `usual_arithmetic_conversions`, `apply_binary_operator`
- lvalue 주소와 한 번만 평가되는 갱신: `lower_designator_address`,
  `resolve_assignment_target`
- UB 조건: ordinary Bool IR과 `UB_GUARD`
- 메모리 접근: `emit_load`, `emit_store`

같은 C 규칙을 새 함수에 복사하지 않습니다. 두 구현이 생기면 이후 한쪽만
고쳐져 의미론이 갈라집니다.

## 4. 표현할 수 없는 이웃을 좁게 거부합니다

지원하는 형태와 바로 인접한 미지원 형태를 시험에 넣습니다. 로어링 API는
그 입력에 `QL_STATUS_OK`와 `QL_C_LOWER_UNKNOWN`을 반환하고, 하나의 구체적인
진단 코드와 source range를 제공해야 합니다. 의미 한계를 status failure,
임의 상수, 또는 더 좁은 질문으로 바꾸지 않습니다.

## 5. 세 종류의 시험을 같은 변경에 넣습니다

1. 정의된 입력의 differential 시험은 같은 소스에서 만든 실제 컴파일 함수와
   IR interpreter 결과를 무작위 입력 및 경계값에서 비교합니다.
2. UB가 있는 구문은 UB 입력을 실제 C 함수로 실행하지 않고 interpreter가
   `UNDEFINED_BEHAVIOR`를 내는지 별도로 확인합니다.
3. 미지원 이웃은 `UNKNOWN`과 정확한 진단을 확인합니다.

피연산자를 평가하지 않는 구문은 평가했을 때 UB가 되는 피연산자를 사용해
비평가를 고정합니다. 각 `SUPPORTED` 결과는 `ql_ir_verify`를 통과해야 합니다.

## 6. 전체 관문을 확인합니다

변경을 완료하려면 다음을 순서대로 확인합니다.

```sh
cmake --build --preset windows-clang --parallel
ctest --preset windows-clang
./out/build/windows-clang/quodlibet.exe coverage \
    out/corpus/val/units.txt out/corpus/val/detail.tsv
git diff --check
```

커버리지에서 `definitions_lower_failed_status`와
`definitions_lower_verification_failed`가 모두 0이어야 합니다. 수용률의
전후 수치와 이동한 첫 차단 진단을 `docs/coverage/`에 기록합니다.

Linux 전용 sanitizer 또는 fuzzer 표면을 바꿨으면 `linux-sanitize`와
`linux-fuzz`도 실행합니다. 새 parser/decoder 표면은 결정적 fuzz target과
coverage-guided campaign 양쪽에 들어가야 합니다.

## 7. 작업 단위를 추적 가능한 커밋으로 남깁니다

한 커밋에는 다음이 함께 있어야 합니다.

- 로어링 구현
- differential, UB, `UNKNOWN` 경계 시험
- 필요하면 interpreter와 verifier 확장
- 커버리지 전후 기록

커밋 메시지는 추가한 C 구문을 이름으로 식별해야 합니다. 완료 기록은
그 커밋을 실제 예로 가리킵니다. 이렇게 해야 문서만 있고 적용 사례가 없는
상태를 G8 확장성 완료로 세지 않습니다.

## 첫 적용 예

이 절차의 첫 적용 대상은 `sizeof(type)`입니다. `sizeof`는 피연산자를
평가하지 않고 대상 ABI에서 타입의 객체 크기를 `size_t` 값으로 냅니다.
첫 단위는 scalar, pointer, 정의가 있는 struct/union type descriptor만
지원합니다. 배열 type descriptor와 `sizeof expression`은 별도의 정적 타입
질의가 필요하므로 명시적 `UNKNOWN`으로 남깁니다.

적용 커밋과 측정 결과는 구현이 통합된 뒤 이 절에 기록합니다.
