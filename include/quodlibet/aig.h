#ifndef QUODLIBET_AIG_H
#define QUODLIBET_AIG_H

#include "quodlibet/artifact.h"

QL_EXTERN_C_BEGIN

#define QL_AIG_SCHEMA_VERSION 1u
#define QL_ARTIFACT_KIND_DIMACS_CNF "quodlibet.dimacs-cnf"

/* An and-inverter graph: one two-input AND node type, with inversion carried
   on the edges rather than in a node.

   A literal is a node index shifted left by one, with bit zero set when the
   edge is inverted. Node zero is the constant false, so literal 0 is false and
   literal 1 is true. Complementing a literal is an exclusive-or with one and
   allocates nothing, which is the entire reason this representation is used
   for bit-blasting: the negations that a bit-level encoding produces in bulk
   cost no nodes at all.

   Every construction goes through ql_aig_and, which folds constants, collapses
   the four one-operand identities, and structurally hashes the result. Two
   syntactically identical subcircuits therefore share one node, and the shared
   input wires of a miter make the equal halves of two similar functions
   collapse into each other before the solver ever runs. */

typedef uint32_t ql_aig_lit;

#define QL_AIG_LIT_FALSE UINT32_C(0)
#define QL_AIG_LIT_TRUE UINT32_C(1)
#define QL_AIG_LIT_INVALID UINT32_MAX

/* A bit-vector is an array of `width` literals, least significant bit first.
   Widths are bounded so that a lowering cannot ask for a circuit no machine
   can build. */
#define QL_AIG_MAX_BIT_WIDTH 256u

typedef struct ql_aig ql_aig;

typedef struct ql_aig_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    uint32_t reserved_alignment;
    /* Structurally distinct AND nodes, excluding the constant node. */
    uint64_t and_count;
    uint64_t input_count;
    /* AND nodes a structural-hash hit avoided allocating. */
    uint64_t shared_count;
    /* Constructions folded to a constant or to an existing operand. */
    uint64_t folded_count;
    uint64_t reserved[4];
} ql_aig_view_v1;

QL_API void QL_CALL ql_aig_view_init(ql_aig_view_v1 *view);

QL_API ql_status QL_CALL ql_aig_create(const ql_allocator *allocator,
                                       ql_aig **output, ql_error *error);
QL_API void QL_CALL ql_aig_destroy(ql_aig *aig);
QL_API ql_status QL_CALL ql_aig_get_view(const ql_aig *aig,
                                         ql_aig_view_v1 *view,
                                         ql_error *error);

/* Bounds the graph. A construction that would exceed the limit fails with
   QL_STATUS_METHOD_ERROR instead of exhausting memory, so a caller can turn an
   oversized circuit into UNKNOWN rather than a crash. Zero means no limit. */
QL_API void QL_CALL ql_aig_set_node_limit(ql_aig *aig, uint64_t limit);

/* --- Literals ------------------------------------------------------------- */

QL_API ql_aig_lit QL_CALL ql_aig_not(ql_aig_lit literal);
QL_API uint32_t QL_CALL ql_aig_is_constant(ql_aig_lit literal);
QL_API uint32_t QL_CALL ql_aig_is_inverted(ql_aig_lit literal);
QL_API uint32_t QL_CALL ql_aig_literal_is_valid(const ql_aig *aig,
                                                ql_aig_lit literal);

/* --- Construction --------------------------------------------------------- */

QL_API ql_status QL_CALL ql_aig_add_input(ql_aig *aig, ql_aig_lit *output,
                                          ql_error *error);
/* The index an input was allocated with, or QL_AIG_LIT_INVALID when the
   literal is not an uninverted input. A SAT model is decoded through this
   index, so it is part of the public surface rather than an internal
   detail. */
QL_API uint32_t QL_CALL ql_aig_input_index(const ql_aig *aig,
                                           ql_aig_lit literal);
QL_API ql_status QL_CALL ql_aig_input_literal(const ql_aig *aig,
                                              uint32_t index,
                                              ql_aig_lit *output,
                                              ql_error *error);

QL_API ql_status QL_CALL ql_aig_and(ql_aig *aig, ql_aig_lit left,
                                    ql_aig_lit right, ql_aig_lit *output,
                                    ql_error *error);
QL_API ql_status QL_CALL ql_aig_or(ql_aig *aig, ql_aig_lit left,
                                   ql_aig_lit right, ql_aig_lit *output,
                                   ql_error *error);
QL_API ql_status QL_CALL ql_aig_xor(ql_aig *aig, ql_aig_lit left,
                                    ql_aig_lit right, ql_aig_lit *output,
                                    ql_error *error);
QL_API ql_status QL_CALL ql_aig_xnor(ql_aig *aig, ql_aig_lit left,
                                     ql_aig_lit right, ql_aig_lit *output,
                                     ql_error *error);
QL_API ql_status QL_CALL ql_aig_implies(ql_aig *aig, ql_aig_lit antecedent,
                                        ql_aig_lit consequent,
                                        ql_aig_lit *output, ql_error *error);
/* condition ? when_true : when_false */
QL_API ql_status QL_CALL ql_aig_mux(ql_aig *aig, ql_aig_lit condition,
                                    ql_aig_lit when_true,
                                    ql_aig_lit when_false, ql_aig_lit *output,
                                    ql_error *error);

/* --- Bit-vector construction ---------------------------------------------- */

/* Every bit-vector entry point writes exactly `width` literals into `output`,
   least significant bit first, and never reads `output` before writing it, so
   an in-place call is safe only when the documentation says so. Operand and
   result arrays may otherwise alias freely because each operation reads all of
   its operands before writing any result. */

/* Exact-width canonical little-endian bytes, as IR constants use. The last
   byte's unused high bits must be zero. */
QL_API ql_status QL_CALL ql_aig_bv_constant(ql_aig *aig, const void *bytes,
                                            size_t size, uint32_t width,
                                            ql_aig_lit *output,
                                            ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_input(ql_aig *aig, uint32_t width,
                                         ql_aig_lit *output, ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_fill(ql_aig_lit value, uint32_t width,
                                        ql_aig_lit *output);
QL_API ql_status QL_CALL ql_aig_bv_not(ql_aig *aig, const ql_aig_lit *operand,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_and(ql_aig *aig, const ql_aig_lit *left,
                                       const ql_aig_lit *right,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_or(ql_aig *aig, const ql_aig_lit *left,
                                      const ql_aig_lit *right, uint32_t width,
                                      ql_aig_lit *output, ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_xor(ql_aig *aig, const ql_aig_lit *left,
                                       const ql_aig_lit *right,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
/* Ripple-carry. `carry_out` may be null. */
QL_API ql_status QL_CALL ql_aig_bv_add(ql_aig *aig, const ql_aig_lit *left,
                                       const ql_aig_lit *right,
                                       ql_aig_lit carry_in, uint32_t width,
                                       ql_aig_lit *output,
                                       ql_aig_lit *carry_out,
                                       ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_sub(ql_aig *aig, const ql_aig_lit *left,
                                       const ql_aig_lit *right,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_neg(ql_aig *aig, const ql_aig_lit *operand,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
/* Shift-and-add, truncated to `width`. C multiplication is width-preserving
   for both signednesses, so one implementation serves both. */
QL_API ql_status QL_CALL ql_aig_bv_mul(ql_aig *aig, const ql_aig_lit *left,
                                       const ql_aig_lit *right,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
/* Restoring division. `quotient` and `remainder` may each be null.

   A zero divisor is not undefined here: this produces the SMT-LIB
   totalization, an all-ones quotient and the dividend as the remainder, so
   that the circuit is total. Rejecting the division by zero is the UB guard's
   job, exactly as it is in the SMT encoding, and this function must never be
   read as deciding it. */
QL_API ql_status QL_CALL ql_aig_bv_udivrem(ql_aig *aig,
                                           const ql_aig_lit *dividend,
                                           const ql_aig_lit *divisor,
                                           uint32_t width,
                                           ql_aig_lit *quotient,
                                           ql_aig_lit *remainder,
                                           ql_error *error);
/* Truncated toward zero, matching C and SMT-LIB bvsdiv/bvsrem. */
QL_API ql_status QL_CALL ql_aig_bv_sdivrem(ql_aig *aig,
                                           const ql_aig_lit *dividend,
                                           const ql_aig_lit *divisor,
                                           uint32_t width,
                                           ql_aig_lit *quotient,
                                           ql_aig_lit *remainder,
                                           ql_error *error);
/* Barrel shifters over a same-width shift amount. A shift amount at or above
   the width yields zero, zero, and the sign bit respectively, matching
   SMT-LIB. C's undefined over-shift is again the UB guard's business. */
QL_API ql_status QL_CALL ql_aig_bv_shl(ql_aig *aig, const ql_aig_lit *operand,
                                       const ql_aig_lit *amount,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_lshr(ql_aig *aig, const ql_aig_lit *operand,
                                        const ql_aig_lit *amount,
                                        uint32_t width, ql_aig_lit *output,
                                        ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_ashr(ql_aig *aig, const ql_aig_lit *operand,
                                        const ql_aig_lit *amount,
                                        uint32_t width, ql_aig_lit *output,
                                        ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_mux(ql_aig *aig, ql_aig_lit condition,
                                       const ql_aig_lit *when_true,
                                       const ql_aig_lit *when_false,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_zext(ql_aig *aig, const ql_aig_lit *operand,
                                        uint32_t from_width,
                                        uint32_t to_width, ql_aig_lit *output,
                                        ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_sext(ql_aig *aig, const ql_aig_lit *operand,
                                        uint32_t from_width,
                                        uint32_t to_width, ql_aig_lit *output,
                                        ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_trunc(ql_aig *aig,
                                         const ql_aig_lit *operand,
                                         uint32_t from_width,
                                         uint32_t to_width,
                                         ql_aig_lit *output, ql_error *error);

/* --- Bit-vector predicates ------------------------------------------------ */

QL_API ql_status QL_CALL ql_aig_bv_eq(ql_aig *aig, const ql_aig_lit *left,
                                      const ql_aig_lit *right, uint32_t width,
                                      ql_aig_lit *output, ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_ult(ql_aig *aig, const ql_aig_lit *left,
                                       const ql_aig_lit *right,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_ule(ql_aig *aig, const ql_aig_lit *left,
                                       const ql_aig_lit *right,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_slt(ql_aig *aig, const ql_aig_lit *left,
                                       const ql_aig_lit *right,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_sle(ql_aig *aig, const ql_aig_lit *left,
                                       const ql_aig_lit *right,
                                       uint32_t width, ql_aig_lit *output,
                                       ql_error *error);
/* One exactly when some bit, or every bit, of the operand is set. */
QL_API ql_status QL_CALL ql_aig_bv_reduce_or(ql_aig *aig,
                                             const ql_aig_lit *operand,
                                             uint32_t width,
                                             ql_aig_lit *output,
                                             ql_error *error);
QL_API ql_status QL_CALL ql_aig_bv_reduce_and(ql_aig *aig,
                                              const ql_aig_lit *operand,
                                              uint32_t width,
                                              ql_aig_lit *output,
                                              ql_error *error);

/* --- Concrete evaluation -------------------------------------------------- */

/* Evaluates the cone under `literal` on a concrete input assignment, one byte
   per input, each zero or one. This exists so a test can hold the circuit
   against an independent computation, and so a decoded SAT assignment can be
   checked against the circuit it came from before anything is claimed about
   it. */
QL_API ql_status QL_CALL ql_aig_evaluate(const ql_aig *aig,
                                         const uint8_t *input_values,
                                         size_t input_count,
                                         ql_aig_lit literal, uint32_t *output,
                                         ql_error *error);

/* --- CNF ------------------------------------------------------------------ */

typedef struct ql_aig_cnf ql_aig_cnf;

typedef struct ql_aig_cnf_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    uint32_t reserved_alignment;
    uint64_t variable_count;
    uint64_t clause_count;
    /* Zero when the root folded to the constant true, so the formula is
       trivially satisfiable and no solver is needed; one when it folded to
       false, so the formula is trivially unsatisfiable. A trivial answer is
       still an answer this encoding produced, never a checked proof. */
    uint32_t trivially_true;
    uint32_t trivially_false;
    uint64_t reserved[4];
} ql_aig_cnf_view_v1;

/* Tseitin-encodes the cone under `root` and asserts it. Only nodes the root
   actually reaches get a variable, so an unused half of a wide circuit costs
   nothing. */
QL_API ql_status QL_CALL ql_aig_cnf_create(const ql_allocator *allocator,
                                           const ql_aig *aig, ql_aig_lit root,
                                           ql_aig_cnf **output,
                                           ql_error *error);
QL_API void QL_CALL ql_aig_cnf_destroy(ql_aig_cnf *cnf);
QL_API ql_status QL_CALL ql_aig_cnf_get_view(const ql_aig_cnf *cnf,
                                             ql_aig_cnf_view_v1 *view,
                                             ql_error *error);
/* The DIMACS variable an AIG input was assigned, or zero when the root's cone
   does not reach that input. A model assigns nothing to an unreached input,
   and the caller must treat it as unconstrained rather than as zero. */
QL_API uint32_t QL_CALL ql_aig_cnf_input_variable(const ql_aig_cnf *cnf,
                                                  uint32_t input_index);
/* Deterministic DIMACS bytes. The same AIG and root always produce the same
   text, which is what lets the query digest identify the question. */
QL_API ql_status QL_CALL ql_aig_cnf_artifact_create(
    const ql_allocator *allocator, const ql_aig_cnf *cnf,
    ql_artifact **output, ql_error *error);

QL_EXTERN_C_END

#endif
