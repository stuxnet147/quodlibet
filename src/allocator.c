#include "quodlibet/allocator.h"

#include <stdlib.h>
#include <string.h>

#include "internal.h"

static void *QL_CALL default_allocate(void *user_data, size_t size) {
    (void)user_data;
    return malloc(size == 0u ? 1u : size);
}

static void *QL_CALL default_reallocate(void *user_data, void *pointer,
                                        size_t size) {
    (void)user_data;
    return realloc(pointer, size == 0u ? 1u : size);
}

static void QL_CALL default_deallocate(void *user_data, void *pointer) {
    (void)user_data;
    free(pointer);
}

static const ql_allocator default_allocator = {
    NULL, default_allocate, default_reallocate, default_deallocate
};

const ql_allocator *QL_CALL ql_default_allocator(void) {
    return &default_allocator;
}

uint32_t QL_CALL ql_allocator_is_valid(const ql_allocator *allocator) {
    return allocator != NULL && allocator->allocate != NULL &&
           allocator->reallocate != NULL && allocator->deallocate != NULL;
}

char *ql_internal_strdup(const ql_allocator *allocator, const char *text) {
    size_t length;
    char *copy;

    if (!ql_allocator_is_valid(allocator) || text == NULL) {
        return NULL;
    }
    length = strlen(text);
    if (length == SIZE_MAX) {
        return NULL;
    }
    copy = allocator->allocate(allocator->user_data, length + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, text, length + 1u);
    return copy;
}
