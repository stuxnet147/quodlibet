#ifndef QUODLIBET_ALLOCATOR_H
#define QUODLIBET_ALLOCATOR_H

#include "quodlibet/common.h"

QL_EXTERN_C_BEGIN

typedef void *(QL_CALL *ql_allocate_fn)(void *user_data, size_t size);
typedef void *(QL_CALL *ql_reallocate_fn)(void *user_data, void *pointer,
                                         size_t size);
typedef void (QL_CALL *ql_deallocate_fn)(void *user_data, void *pointer);

/* Callbacks follow malloc/realloc/free semantics. In particular, reallocation
   accepts a null pointer and deallocation accepts a null pointer. */
typedef struct ql_allocator {
    void *user_data;
    ql_allocate_fn allocate;
    ql_reallocate_fn reallocate;
    ql_deallocate_fn deallocate;
} ql_allocator;

QL_API const ql_allocator *QL_CALL ql_default_allocator(void);
QL_API uint32_t QL_CALL ql_allocator_is_valid(const ql_allocator *allocator);

QL_EXTERN_C_END

#endif
