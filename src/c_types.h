#ifndef QUODLIBET_SRC_C_TYPES_H
#define QUODLIBET_SRC_C_TYPES_H

#include "quodlibet/common.h"

/* The scalar C type model for the ASM2C_GNU_V1 profile, kept apart from the
   lowering so that the C11 conversion rules can be read and tested on their
   own. Nothing here knows about the IR.

   Widths follow the target ABI, x86_64 Linux LP64, not the host: `long` is 64
   bits. Ranks follow C11 6.3.1.1 so that char < short < int < long < long
   long, which is what the usual arithmetic conversions are defined over. */

typedef enum ql_c_scalar_kind {
    QL_C_SCALAR_INVALID = 0,
    QL_C_SCALAR_VOID,
    QL_C_SCALAR_BOOL,
    QL_C_SCALAR_INTEGER,
    /* A pointer under this profile is an address and nothing else, so its
       width and signedness are the address's. What it points at is carried
       beside it by the lowering, not here. */
    QL_C_SCALAR_POINTER,
    /* A struct or union. Its layout lives in the lowering's record table,
       which this header deliberately knows nothing about. */
    QL_C_SCALAR_RECORD
} ql_c_scalar_kind;

/* The target ABI is LP64, so an address is 64 bits wide. */
#define QL_C_POINTER_WIDTH 64u

typedef struct ql_c_scalar_type {
    ql_c_scalar_kind kind;
    uint32_t width;
    uint32_t rank;
    uint32_t is_signed;
} ql_c_scalar_type;

/* The longest spelling this profile accepts is "unsigned long long int"
   without separators, so the normalisation buffer is sized well past it and a
   longer spelling is refused rather than truncated. */
#define QL_C_SCALAR_SPELLING_CAPACITY 64u

ql_c_scalar_type ql_c_scalar_make_void(void);
ql_c_scalar_type ql_c_scalar_make_bool(void);
ql_c_scalar_type ql_c_scalar_make_integer(uint32_t width, uint32_t rank,
                                          uint32_t is_signed);

/* Strips whitespace from `spelling` and matches the result against the fixed
   profile vocabulary. Returns zero when the spelling names no type this
   profile defines, including when it is too long to normalise. A caller that
   cannot resolve a spelling must report it, never guess a meaning for it. */
int ql_c_scalar_from_spelling(const char *spelling, ql_c_scalar_type *output);

/* Typedefs the corpus's own sources spell out but that its extracted records
   drop, because extraction keeps only the type and callee context around a
   function and not the file preamble. Resolving a name from this table is not
   a guess about what it might mean: it is the definition the compiler that
   produced the corpus actually used. A name absent from the table stays
   unresolved and is refused, as before.

   Returns zero when the spelling is not one of them. */
int ql_c_scalar_from_corpus_typedef(const char *spelling,
                                    ql_c_scalar_type *output);

int ql_c_scalar_same(ql_c_scalar_type left, ql_c_scalar_type right);

/* C11 6.3.1.1: anything of lesser rank than int becomes int, because int
   represents every value of every narrower type at these widths. */
ql_c_scalar_type ql_c_scalar_promote(ql_c_scalar_type type);

/* C11 6.3.1.8, applied to already-promoted operands. */
ql_c_scalar_type ql_c_scalar_usual(ql_c_scalar_type left,
                                   ql_c_scalar_type right);

/* Whether `value`, read as a non-negative magnitude, fits in `type`. */
int ql_c_scalar_can_represent(ql_c_scalar_type type, uint64_t value);

#endif
