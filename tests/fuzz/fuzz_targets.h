#ifndef QUODLIBET_TESTS_FUZZ_TARGETS_H
#define QUODLIBET_TESTS_FUZZ_TARGETS_H

/* The fuzz targets for the C parser, the lowering, and the IR decoder.

   These live in a header so that the libFuzzer drivers in this directory and
   the deterministic campaign in tests/test_fuzz.cpp run exactly the same
   code. A target that drifted between the coverage-guided run and the run
   that happens on every ctest would leave the two disagreeing about what has
   been tested.

   Not crashing is the weakest property here. The targets also assert the
   invariants that make the correctness devices mean something:

     - a module the decoder accepts must survive verification, pass or fail,
       without crashing;
     - a lowering that reports SUPPORTED must verify, because G8 says the
       verifier passes on every lowering output and a mutated source is still
       a source;
     - a module that verifies must run in the interpreter without crashing.

   Both includers override the two hooks below: the drivers abort so libFuzzer
   records a crash, and the campaign records the violation and counts how
   often each target was actually reached. */

#include "quodlibet/artifact.h"
#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(QL_FUZZ_REQUIRE)
#define QL_FUZZ_REQUIRE(condition_, message_)                                  \
  do {                                                                         \
    if (!(condition_)) {                                                       \
      fprintf(stderr, "quodlibet fuzz invariant violated: %s\n", (message_));  \
      abort();                                                                 \
    }                                                                          \
  } while (0)
#endif

/* `kind_` is one of parsed, lowered, decoded, verified. */
#if !defined(QL_FUZZ_REACHED)
#define QL_FUZZ_REACHED(kind_) ((void)0)
#endif

/* Each driver uses one target, so the other two are unused there. */
#if defined(__GNUC__) || defined(__clang__)
#define QL_FUZZ_MAYBE_UNUSED __attribute__((unused))
#else
#define QL_FUZZ_MAYBE_UNUSED
#endif

/* A module with more parameters than this is not run. The bound keeps the
   targets free of allocation, and no lowering this profile produces comes
   close to it. */
#define QL_FUZZ_MAX_PARAMETERS 64u

/* Runs the module on zeroed inputs. What it computes does not matter; that a
   verified module cannot crash the interpreter does. */
QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_run_interpreter(ql_ir *ir) {
  ql_ir_view_v1 view;
  ql_ir_interp_input_v1 inputs[QL_FUZZ_MAX_PARAMETERS];
  unsigned char storage[QL_FUZZ_MAX_PARAMETERS][QL_IR_INTERP_VALUE_CAPACITY];
  ql_ir_interp_result_v1 result;
  ql_ir_interp_options_v1 options;
  ql_error error;
  size_t count = 0u;
  size_t index;

  memset(&view, 0, sizeof(view));
  view.struct_size = sizeof(view);
  if (ql_ir_get_view(ir, &view, &error) != QL_STATUS_OK) {
    return;
  }
  for (index = 0u; index < view.value_count; ++index) {
    ql_ir_value_view_v1 value;
    ql_ir_type_view_v1 type;
    uint32_t width;

    memset(&value, 0, sizeof(value));
    value.struct_size = sizeof(value);
    if (ql_ir_value_at(ir, index, &value, &error) != QL_STATUS_OK) {
      return;
    }
    if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
      continue;
    }
    memset(&type, 0, sizeof(type));
    type.struct_size = sizeof(type);
    if (ql_ir_type_at(ir, value.type, &type, &error) != QL_STATUS_OK) {
      return;
    }
    width = type.kind == QL_IR_TYPE_BOOL ? 1u : type.bit_width;
    if (width == 0u || width > QL_IR_INTERP_MAX_BIT_WIDTH ||
        count >= QL_FUZZ_MAX_PARAMETERS) {
      return;
    }
    memset(storage[count], 0, sizeof(storage[count]));
    ql_ir_interp_input_init(&inputs[count]);
    inputs[count].value = value.id;
    inputs[count].data = storage[count];
    inputs[count].size = (size_t)((width + 7u) / 8u);
    ++count;
  }
  ql_ir_interp_options_init(&options);
  /* A cyclic seed may legitimately run forever. The fuzz target checks
     crash safety rather than completion, so keep the established acyclic
     budget and cap only cyclic inputs tightly enough for the campaign. */
  options.step_limit = view.cfg_kind == QL_IR_CFG_CYCLIC ? 64u : 100000u;
  memset(&result, 0, sizeof(result));
  result.struct_size = sizeof(result);
  (void)ql_ir_interp_run(NULL, ir, count == 0u ? NULL : inputs, count, &options,
                         &result, &error);
}

QL_FUZZ_MAYBE_UNUSED static void
ql_fuzz_verify_and_run(ql_ir *ir, int require_verification,
                       const char *reason) {
  ql_ir_verify_report_v1 report;
  ql_error error;
  ql_status status;

  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  status = ql_ir_verify(NULL, ir, &report, &error);
  if (require_verification) {
    QL_FUZZ_REQUIRE(status == QL_STATUS_OK, reason);
  }
  if (status == QL_STATUS_OK) {
    QL_FUZZ_REACHED(verified);
    ql_fuzz_run_interpreter(ir);
  }
}

QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_frontend(const unsigned char *data,
                                                  size_t size) {
  ql_c_frontend_unit *unit = NULL;
  ql_c_frontend_unit_view view;
  ql_error error;
  size_t index;

  if (ql_c_frontend_analyze(NULL, (const char *)data, size, &unit, &error) !=
      QL_STATUS_OK) {
    ql_c_frontend_unit_destroy(unit);
    return;
  }
  QL_FUZZ_REACHED(parsed);
  memset(&view, 0, sizeof(view));
  view.struct_size = sizeof(view);
  if (ql_c_frontend_unit_get_view(unit, &view, &error) == QL_STATUS_OK) {
    for (index = 0u; index < view.function_count; ++index) {
      ql_c_function_view function;
      size_t inner;
      memset(&function, 0, sizeof(function));
      function.struct_size = sizeof(function);
      if (ql_c_frontend_function_at(unit, index, &function, &error) !=
          QL_STATUS_OK) {
        continue;
      }
      for (inner = 0u; inner < function.parameter_count; ++inner) {
        ql_c_parameter_view parameter;
        memset(&parameter, 0, sizeof(parameter));
        parameter.struct_size = sizeof(parameter);
        (void)ql_c_frontend_parameter_at(unit, index, inner, &parameter,
                                         &error);
      }
      for (inner = 0u; inner < function.diagnostic_count; ++inner) {
        ql_c_frontend_diagnostic_view record;
        memset(&record, 0, sizeof(record));
        record.struct_size = sizeof(record);
        (void)ql_c_frontend_function_diagnostic_at(unit, index, inner, &record,
                                                   &error);
      }
    }
    for (index = 0u; index < view.diagnostic_count; ++index) {
      ql_c_frontend_diagnostic_view record;
      memset(&record, 0, sizeof(record));
      record.struct_size = sizeof(record);
      (void)ql_c_frontend_diagnostic_at(unit, index, &record, &error);
    }
  }
  ql_c_frontend_unit_destroy(unit);
}

QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_lowering(const unsigned char *data,
                                                  size_t size) {
  ql_c_frontend_unit *unit = NULL;
  ql_c_frontend_unit_view view;
  ql_error error;
  const char *source = (const char *)data;
  size_t index;

  if (ql_c_frontend_analyze(NULL, source, size, &unit, &error) !=
      QL_STATUS_OK) {
    ql_c_frontend_unit_destroy(unit);
    return;
  }
  memset(&view, 0, sizeof(view));
  view.struct_size = sizeof(view);
  if (ql_c_frontend_unit_get_view(unit, &view, &error) == QL_STATUS_OK) {
    for (index = 0u; index < view.function_count; ++index) {
      ql_c_function_view function;
      ql_c_lower_result *result = NULL;
      ql_c_lower_result_view_v1 lowered;

      memset(&function, 0, sizeof(function));
      function.struct_size = sizeof(function);
      if (ql_c_frontend_function_at(unit, index, &function, &error) !=
          QL_STATUS_OK) {
        continue;
      }
      if (ql_c_lower_selected_function(NULL, source, size, unit, &function,
                                       &result, &error) != QL_STATUS_OK) {
        ql_c_lower_result_destroy(result);
        continue;
      }
      memset(&lowered, 0, sizeof(lowered));
      lowered.struct_size = sizeof(lowered);
      if (ql_c_lower_result_get_view(result, &lowered, &error) ==
              QL_STATUS_OK &&
          lowered.support == QL_C_LOWER_SUPPORTED) {
        ql_ir *ir = NULL;
        QL_FUZZ_REACHED(lowered);
        QL_FUZZ_REQUIRE(lowered.ir_artifact != NULL,
                        "a SUPPORTED lowering produced no IR "
                        "artifact");
        if (ql_ir_open(NULL, lowered.ir_artifact, &ir, &error) ==
            QL_STATUS_OK) {
          ql_fuzz_verify_and_run(
              ir, 1,
              "the lowering reported SUPPORTED but its IR failed "
              "verification");
        } else {
          QL_FUZZ_REQUIRE(0, "a SUPPORTED lowering produced IR the "
                             "decoder rejected");
        }
        ql_ir_release(ir);
      }
      ql_c_lower_result_destroy(result);
    }
  }
  ql_c_frontend_unit_destroy(unit);
}

QL_FUZZ_MAYBE_UNUSED static void ql_fuzz_ir_decoder(const unsigned char *data,
                                                    size_t size) {
  ql_artifact *artifact = NULL;
  ql_ir *ir = NULL;
  ql_error error;

  if (ql_artifact_create(NULL, QL_ARTIFACT_KIND_IR,
                         QL_IR_ARTIFACT_SCHEMA_VERSION, data, size, &artifact,
                         &error) != QL_STATUS_OK) {
    return;
  }
  if (ql_ir_open(NULL, artifact, &ir, &error) == QL_STATUS_OK) {
    QL_FUZZ_REACHED(decoded);
    /* A decoded module need not verify: the decoder guarantees structure,
       not the guard obligations the verifier adds on top. What it must
       not do is crash the verifier. */
    ql_fuzz_verify_and_run(ir, 0, NULL);
    ql_ir_release(ir);
  }
  ql_artifact_release(artifact);
}

#endif
