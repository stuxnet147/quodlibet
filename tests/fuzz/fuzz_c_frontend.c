/* libFuzzer entry point. The target itself lives in fuzz_targets.h so that
   this driver and the deterministic campaign in tests/test_fuzz.cpp exercise
   the same code. */

#include "fuzz_targets.h"

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    ql_fuzz_frontend(data, size);
    return 0;
}
