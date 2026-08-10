/* The smallest program that asks the two questions an install has to answer.
 *
 * It is compiled against the INSTALLED headers and linked against the
 * INSTALLED library, then run from the installed bin directory.
 *
 *   - Can a consumer link at all against what was installed? If the vendored
 *     dependencies are neither bundled into the archive nor installed beside
 *     it, this fails at link time and the install is not self-contained.
 *
 *   - Does the relocatable Bitwuzla lookup find the copy beside the running
 *     executable? It prints the build-time default so the two can be told
 *     apart: the default still exists in a developer's tree, so a lookup that
 *     silently fell back to it would otherwise look like success.
 *
 * Exit codes are distinct so scripts/check-install.sh can tell a lookup
 * failure from a crash.
 */

#include "quodlibet/solver.h"

#include <stdio.h>

int main(void) {
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    ql_solver *solver = NULL;
    ql_error error;

    printf("build-time default: %s\n", ql_bitwuzla_executable_path());
    if (descriptor == NULL) {
        printf("no Bitwuzla descriptor\n");
        return 2;
    }
    if (descriptor->capability.availability == QL_SOLVER_UNAVAILABLE) {
        printf("Bitwuzla support is disabled in this build\n");
        return 3;
    }
    if (ql_solver_create(NULL, descriptor, NULL, &solver, &error) !=
        QL_STATUS_OK) {
        printf("create failed: %s\n", error.message);
        return 1;
    }
    printf("created ok\n");
    ql_solver_destroy(solver);
    return 0;
}
