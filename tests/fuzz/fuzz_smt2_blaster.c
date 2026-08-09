/* libFuzzer entry point. The target itself lives in fuzz_blaster_target.h so
   that this driver and the deterministic campaign in
   tests/test_proof_aigsat.cpp exercise the same code. */

#include "fuzz_blaster_target.h"

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    ql_fuzz_smt2_blaster(data, size);
    return 0;
}
