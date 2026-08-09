#ifndef QUODLIBET_COMMON_H
#define QUODLIBET_COMMON_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(QL_BUILD_SHARED)
#    if defined(QL_BUILDING_LIBRARY)
#      define QL_API __declspec(dllexport)
#    else
#      define QL_API __declspec(dllimport)
#    endif
#  else
#    define QL_API
#  endif
#  define QL_PLUGIN_EXPORT __declspec(dllexport)
#  define QL_CALL __cdecl
#else
#  if defined(__GNUC__) || defined(__clang__)
#    define QL_API __attribute__((visibility("default")))
#    define QL_PLUGIN_EXPORT __attribute__((visibility("default")))
#  else
#    define QL_API
#    define QL_PLUGIN_EXPORT
#  endif
#  define QL_CALL
#endif

#ifdef __cplusplus
#  define QL_EXTERN_C_BEGIN extern "C" {
#  define QL_EXTERN_C_END }
#else
#  define QL_EXTERN_C_BEGIN
#  define QL_EXTERN_C_END
#endif

#define QL_ABI_VERSION 1u
#define QL_IR_SCHEMA_VERSION 1u
#define QL_PIPELINE_SCHEMA_VERSION 1u

#endif
