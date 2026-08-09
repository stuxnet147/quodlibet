/* libFuzzer entry point. The target itself lives in fuzz_contract_targets.h
   so that this driver and the deterministic campaign in
   tests/test_fuzz_contracts.cpp exercise the same code. */

#include "fuzz_contract_targets.h"

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    ql_fuzz_signature_decoder(data, size);
    return 0;
}
