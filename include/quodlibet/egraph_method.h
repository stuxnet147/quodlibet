#ifndef QUODLIBET_EGRAPH_METHOD_H
#define QUODLIBET_EGRAPH_METHOD_H

#include "quodlibet/registry.h"

QL_EXTERN_C_BEGIN

#define QL_EGRAPH_METHOD_NAME "normalize.egraph"

/* Registers the checked pure-expression normalizer. The method consumes one
   verified, acyclic, single-block quodlibet.ir artifact and emits another
   quodlibet.ir artifact. It never claims a proof or counterexample. */
QL_API ql_status QL_CALL ql_register_egraph_method(ql_registry *registry,
                                                   ql_error *error);

QL_EXTERN_C_END

#endif
