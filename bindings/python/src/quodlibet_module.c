/* The CPython C extension for Quodlibet.

   This is a real extension module compiled against the CPython headers, not a
   runtime redeclaration of the ABI through ctypes or cffi. It is built against
   the stable ABI (abi3, 3.11 and later), so one wheel serves every later
   CPython minor version.

   The module deliberately holds no state and defines no static types. It
   converts arguments into ql_py_spec, releases the GIL for the whole
   judgement, and converts ql_py_result into a plain dict. The friendly
   surface (result objects, enum spellings, the batch path) lives in
   quodlibet/__init__.py, which keeps the amount of C that touches interpreter
   objects small enough to audit. */

#define PY_SSIZE_T_CLEAN
#ifndef Py_LIMITED_API
#define Py_LIMITED_API 0x030B0000
#endif

#include <Python.h>

#include "ql_check.h"

#include "quodlibet/solver.h"
#include "quodlibet/version.h"

#include <stdlib.h>
#include <string.h>

static PyObject *quodlibet_error = NULL;

/* Sets `name` on `object` from a value this function owns, so no caller has to
   track the temporary. */
static int set_attribute(PyObject *object, const char *name, PyObject *value) {
    int result;

    if (value == NULL) {
        return -1;
    }
    result = PyObject_SetAttrString(object, name, value);
    Py_DECREF(value);
    return result;
}

static int raise_from_result(const ql_py_result *result) {
    PyObject *instance =
        PyObject_CallFunction(quodlibet_error, "s", result->message);

    if (instance == NULL) {
        return -1;
    }
    if (set_attribute(instance, "status",
                      PyLong_FromLong((long)result->status)) != 0 ||
        set_attribute(instance, "status_name",
                      PyUnicode_FromString(
                          ql_status_string(result->status))) != 0) {
        Py_DECREF(instance);
        return -1;
    }
    PyErr_SetObject(quodlibet_error, instance);
    Py_DECREF(instance);
    return -1;
}

/* dict helpers. Each steals the reference it is given so that a failure path
   never leaks the value it was about to store. */
static int dict_set(PyObject *dict, const char *key, PyObject *value) {
    int result;

    if (value == NULL) {
        return -1;
    }
    result = PyDict_SetItemString(dict, key, value);
    Py_DECREF(value);
    return result;
}

static int dict_set_u64(PyObject *dict, const char *key, uint64_t value) {
    return dict_set(dict, key, PyLong_FromUnsignedLongLong(value));
}

static int dict_set_i(PyObject *dict, const char *key, long value) {
    return dict_set(dict, key, PyLong_FromLong(value));
}

static int dict_set_str(PyObject *dict, const char *key, const char *value) {
    return dict_set(dict, key, PyUnicode_FromString(value));
}

/* Reads an optional [(left, right), ...] argument correspondence. Only the
   abstract sequence protocol is used, because the fast-sequence macros are
   outside the stable ABI. */
static int read_bindings(PyObject *object, ql_py_spec *spec) {
    Py_ssize_t count;
    Py_ssize_t index;

    if (object == Py_None) {
        return 0;
    }
    if (!PySequence_Check(object)) {
        PyErr_SetString(PyExc_TypeError,
                        "argument_bindings must be a sequence or None");
        return -1;
    }
    count = PySequence_Size(object);
    if (count < 0) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    spec->bindings = calloc((size_t)count, sizeof(*spec->bindings));
    if (spec->bindings == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    spec->binding_count = (size_t)count;
    for (index = 0; index < count; ++index) {
        PyObject *pair = PySequence_GetItem(object, index);
        PyObject *left_item;
        PyObject *right_item;
        unsigned long left;
        unsigned long right;

        if (pair == NULL) {
            return -1;
        }
        if (!PySequence_Check(pair) || PySequence_Size(pair) != 2) {
            Py_DECREF(pair);
            PyErr_SetString(PyExc_ValueError,
                            "each argument binding must be a (left, right) "
                            "pair of indices");
            return -1;
        }
        left_item = PySequence_GetItem(pair, 0);
        right_item = PySequence_GetItem(pair, 1);
        Py_DECREF(pair);
        if (left_item == NULL || right_item == NULL) {
            Py_XDECREF(left_item);
            Py_XDECREF(right_item);
            return -1;
        }
        left = PyLong_AsUnsignedLong(left_item);
        right = PyLong_AsUnsignedLong(right_item);
        Py_DECREF(left_item);
        Py_DECREF(right_item);
        if (PyErr_Occurred()) {
            return -1;
        }
        spec->bindings[index].left_index = (uint32_t)left;
        spec->bindings[index].right_index = (uint32_t)right;
    }
    return 0;
}

static PyObject *build_policy_dict(const ql_policy_result_v1 *policy) {
    PyObject *dict = PyDict_New();

    if (dict == NULL) {
        return NULL;
    }
    if (dict_set_str(dict, "policy_name", policy->policy_name) != 0 ||
        dict_set_str(dict, "class_name", policy->class_name) != 0 ||
        dict_set_i(dict, "reported_verdict", (long)policy->reported_verdict) !=
            0 ||
        dict_set_i(dict, "effective_verdict",
                   (long)policy->effective_verdict) != 0 ||
        dict_set_str(dict, "disposition",
                     ql_policy_disposition_string(policy->disposition)) != 0 ||
        dict_set_str(dict, "claims",
                     ql_policy_claims_string(policy->claims)) != 0 ||
        dict_set(dict, "score", PyFloat_FromDouble(policy->score)) != 0 ||
        dict_set(dict, "weakened",
                 PyBool_FromLong((long)policy->weakened)) != 0 ||
        dict_set(dict, "gated", PyBool_FromLong((long)policy->gated)) != 0 ||
        dict_set_str(dict, "gate_reason", policy->gate_reason) != 0 ||
        dict_set_u64(dict, "checked_bound", policy->checked_bound) != 0) {
        Py_DECREF(dict);
        return NULL;
    }
    return dict;
}

static PyObject *build_usage_dict(const ql_budget_usage_v1 *usage) {
    PyObject *dict = PyDict_New();

    if (dict == NULL) {
        return NULL;
    }
    if (dict_set_u64(dict, "elapsed_ns", usage->elapsed_ns) != 0 ||
        dict_set_u64(dict, "memory_current_bytes",
                     usage->memory_current_bytes) != 0 ||
        dict_set_u64(dict, "memory_peak_bytes", usage->memory_peak_bytes) !=
            0 ||
        dict_set_u64(dict, "live_allocation_count",
                     usage->live_allocation_count) != 0 ||
        dict_set_u64(dict, "allocation_count", usage->allocation_count) != 0 ||
        dict_set_u64(dict, "deallocation_count", usage->deallocation_count) !=
            0 ||
        dict_set_u64(dict, "denied_allocation_count",
                     usage->denied_allocation_count) != 0) {
        Py_DECREF(dict);
        return NULL;
    }
    return dict;
}

static PyObject *build_result_dict(const ql_py_result *result) {
    PyObject *dict = PyDict_New();
    PyObject *counterexample;
    PyObject *policy;
    PyObject *usage;

    if (dict == NULL) {
        return NULL;
    }
    if (dict_set_i(dict, "verdict", (long)result->verdict) != 0 ||
        dict_set_str(dict, "verdict_name",
                     ql_policy_verdict_name(result->verdict)) != 0 ||
        dict_set_str(dict, "verdict_display",
                     ql_verdict_string(result->verdict)) != 0 ||
        dict_set_i(dict, "outcome_kind", (long)result->outcome_kind) != 0 ||
        dict_set_i(dict, "evidence_class", (long)result->evidence_class) !=
            0 ||
        dict_set_str(dict, "evidence_class_name",
                     ql_evidence_class_string(result->evidence_class)) != 0 ||
        dict_set_i(dict, "unsat_promotion", (long)result->unsat_promotion) !=
            0 ||
        dict_set_i(dict, "violation_answer", (long)result->violation_answer) !=
            0 ||
        dict_set_i(dict, "domain_answer", (long)result->domain_answer) != 0 ||
        dict_set(dict, "checked_proof",
                 PyBool_FromLong((long)result->checked_proof)) != 0 ||
        dict_set(dict, "replay_confirmed",
                 PyBool_FromLong((long)result->replay_confirmed)) != 0 ||
        dict_set(dict, "budget_exhausted",
                 PyBool_FromLong((long)result->budget_exhausted)) != 0 ||
        dict_set_str(dict, "budget_state",
                     ql_budget_state_string(result->budget_state)) != 0 ||
        dict_set_str(dict, "diagnostic", result->diagnostic) != 0 ||
        dict_set_str(dict, "problem_digest", result->problem_digest) != 0 ||
        dict_set_str(dict, "solver_query_digest",
                     result->solver_query_digest) != 0 ||
        dict_set_str(dict, "solver_binary_digest",
                     result->solver_binary_digest) != 0 ||
        dict_set_str(dict, "cache_key", result->cache_key) != 0 ||
        dict_set_str(dict, "counterexample_digest",
                     result->counterexample_digest) != 0) {
        Py_DECREF(dict);
        return NULL;
    }

    if (result->counterexample_json != NULL) {
        counterexample = PyUnicode_FromStringAndSize(
            result->counterexample_json,
            (Py_ssize_t)result->counterexample_json_size);
    } else {
        counterexample = Py_None;
        Py_INCREF(counterexample);
    }
    if (dict_set(dict, "counterexample_json", counterexample) != 0) {
        Py_DECREF(dict);
        return NULL;
    }

    if (result->has_policy != 0u) {
        policy = build_policy_dict(&result->policy);
    } else {
        policy = Py_None;
        Py_INCREF(policy);
    }
    if (dict_set(dict, "policy", policy) != 0) {
        Py_DECREF(dict);
        return NULL;
    }

    usage = build_usage_dict(&result->usage);
    if (dict_set(dict, "usage", usage) != 0) {
        Py_DECREF(dict);
        return NULL;
    }
    return dict;
}

static char *kwlist[] = {"left_source",
                         "left_function",
                         "right_source",
                         "right_function",
                         "relation",
                         "ub_policy",
                         "observations",
                         "memory_observation",
                         "external_call_observation",
                         "precondition_json",
                         "trust_smt_backend",
                         "solver_timeout_ms",
                         "solver_memory_limit_mb",
                         "solver_executable",
                         "total_wall_clock_ns",
                         "node_wall_clock_ns",
                         "solver_wall_clock_ns",
                         "memory_bytes",
                         "single_allocation_bytes",
                         "policy_json",
                         "argument_bindings",
                         NULL};

static PyObject *quodlibet_check(PyObject *self, PyObject *args,
                                 PyObject *keywords) {
    const char *left_source = NULL;
    Py_ssize_t left_source_size = 0;
    const char *left_function = NULL;
    const char *right_source = NULL;
    Py_ssize_t right_source_size = 0;
    const char *right_function = NULL;
    int relation = 0;
    int ub_policy = 0;
    unsigned long long observations = 0u;
    int memory_observation = 0;
    int external_call_observation = 0;
    const char *precondition_json = NULL;
    Py_ssize_t precondition_json_size = 0;
    int trust_smt_backend = 0;
    unsigned long long solver_timeout_ms = 0u;
    unsigned long long solver_memory_limit_mb = 0u;
    const char *solver_executable = NULL;
    unsigned long long total_ns = 0u;
    unsigned long long node_ns = 0u;
    unsigned long long solver_ns = 0u;
    unsigned long long memory_bytes = 0u;
    unsigned long long single_allocation_bytes = 0u;
    const char *policy_json = NULL;
    Py_ssize_t policy_json_size = 0;
    PyObject *argument_bindings = NULL;
    ql_py_spec spec;
    ql_py_result result;
    PyObject *dict;

    (void)self;
    if (!PyArg_ParseTupleAndKeywords(
            args, keywords, "s#ss#siiKiiz#pKKzKKKKKz#O:check", kwlist,
            &left_source, &left_source_size, &left_function, &right_source,
            &right_source_size, &right_function, &relation, &ub_policy,
            &observations, &memory_observation, &external_call_observation,
            &precondition_json, &precondition_json_size, &trust_smt_backend,
            &solver_timeout_ms, &solver_memory_limit_mb, &solver_executable,
            &total_ns, &node_ns, &solver_ns, &memory_bytes,
            &single_allocation_bytes, &policy_json, &policy_json_size,
            &argument_bindings)) {
        return NULL;
    }

    ql_py_spec_init(&spec);
    spec.left_source = ql_py_strdup(left_source, (size_t)left_source_size);
    spec.left_source_size = (size_t)left_source_size;
    spec.left_function = ql_py_strdup(left_function, strlen(left_function));
    spec.right_source = ql_py_strdup(right_source, (size_t)right_source_size);
    spec.right_source_size = (size_t)right_source_size;
    spec.right_function = ql_py_strdup(right_function, strlen(right_function));
    if (precondition_json != NULL) {
        spec.precondition_json =
            ql_py_strdup(precondition_json, (size_t)precondition_json_size);
        spec.precondition_json_size = (size_t)precondition_json_size;
        if (spec.precondition_json == NULL) {
            ql_py_spec_dispose(&spec);
            return PyErr_NoMemory();
        }
    }
    if (policy_json != NULL) {
        spec.policy_json = ql_py_strdup(policy_json, (size_t)policy_json_size);
        spec.policy_json_size = (size_t)policy_json_size;
        if (spec.policy_json == NULL) {
            ql_py_spec_dispose(&spec);
            return PyErr_NoMemory();
        }
    }
    if (solver_executable != NULL) {
        spec.solver_executable =
            ql_py_strdup(solver_executable, strlen(solver_executable));
        if (spec.solver_executable == NULL) {
            ql_py_spec_dispose(&spec);
            return PyErr_NoMemory();
        }
    }
    if (spec.left_source == NULL || spec.left_function == NULL ||
        spec.right_source == NULL || spec.right_function == NULL) {
        ql_py_spec_dispose(&spec);
        return PyErr_NoMemory();
    }
    if (read_bindings(argument_bindings, &spec) != 0) {
        ql_py_spec_dispose(&spec);
        return NULL;
    }

    spec.relation = (ql_relation)relation;
    spec.ub_policy = (ql_ub_policy)ub_policy;
    spec.observations = (uint64_t)observations;
    spec.memory_observation = (ql_memory_observation)memory_observation;
    spec.external_call_observation =
        (ql_external_call_observation)external_call_observation;
    spec.trust_smt_backend = trust_smt_backend != 0 ? 1u : 0u;
    spec.solver_timeout_ms = (uint64_t)solver_timeout_ms;
    spec.solver_memory_limit_mb = (uint64_t)solver_memory_limit_mb;
    spec.limits.total_wall_clock_ns = (uint64_t)total_ns;
    spec.limits.node_wall_clock_ns = (uint64_t)node_ns;
    spec.limits.solver_wall_clock_ns = (uint64_t)solver_ns;
    spec.limits.memory_bytes = (uint64_t)memory_bytes;
    spec.limits.single_allocation_bytes = (uint64_t)single_allocation_bytes;

    /* Nothing below this point touches a Python object until the GIL is
       reacquired, which is why a reinforcement-learning loop's other threads
       keep running while a solver is being waited on. */
    Py_BEGIN_ALLOW_THREADS
    ql_py_check(&spec, &result);
    Py_END_ALLOW_THREADS

    ql_py_spec_dispose(&spec);
    if (result.ok == 0u) {
        (void)raise_from_result(&result);
        ql_py_result_dispose(&result);
        return NULL;
    }
    dict = build_result_dict(&result);
    ql_py_result_dispose(&result);
    return dict;
}

static PyObject *quodlibet_backend_info(PyObject *self, PyObject *unused) {
    const ql_solver_descriptor_v1 *descriptor = ql_bitwuzla_solver_descriptor();
    const char *path = ql_py_backend_path();
    PyObject *dict = PyDict_New();

    (void)self;
    (void)unused;
    if (dict == NULL) {
        return NULL;
    }
    if (dict_set(dict, "available",
                 PyBool_FromLong((long)ql_py_backend_available())) != 0 ||
        dict_set_str(dict, "name", descriptor->name) != 0 ||
        dict_set_str(dict, "version", descriptor->version) != 0 ||
        dict_set_str(dict, "executable", path != NULL ? path : "") != 0) {
        Py_DECREF(dict);
        return NULL;
    }
    return dict;
}

static PyObject *quodlibet_core_version(PyObject *self, PyObject *unused) {
    (void)self;
    (void)unused;
    return PyUnicode_FromString(ql_version_string());
}

static PyMethodDef methods[] = {
    {"check", (PyCFunction)(void (*)(void))quodlibet_check,
     METH_VARARGS | METH_KEYWORDS,
     "Run one equivalence check. Every argument is required; the friendly "
     "surface lives in the quodlibet package."},
    {"backend_info", quodlibet_backend_info, METH_NOARGS,
     "Describe the pinned SMT backend this extension was built against."},
    {"core_version", quodlibet_core_version, METH_NOARGS,
     "The linked Quodlibet core version string."},
    {NULL, NULL, 0, NULL}};

static int add_constants(PyObject *module) {
    struct {
        const char *name;
        long value;
    } constants[] = {
        {"RELATION_EQUIVALENCE", (long)QL_RELATION_EQUIVALENCE},
        {"RELATION_LEFT_REFINES_RIGHT",
         (long)QL_RELATION_LEFT_REFINES_RIGHT},
        {"RELATION_RIGHT_REFINES_LEFT",
         (long)QL_RELATION_RIGHT_REFINES_LEFT},
        {"UB_MUST_MATCH", (long)QL_UB_MUST_MATCH},
        {"UB_LANGUAGE_REFINEMENT", (long)QL_UB_LANGUAGE_REFINEMENT},
        {"UB_COMPARE_WHERE_BOTH_DEFINED",
         (long)QL_UB_COMPARE_WHERE_BOTH_DEFINED},
        {"OBSERVE_RETURN_VALUE", (long)QL_OBSERVE_RETURN_VALUE},
        {"OBSERVE_MEMORY", (long)QL_OBSERVE_MEMORY},
        {"OBSERVE_TERMINATION", (long)QL_OBSERVE_TERMINATION},
        {"OBSERVE_VOLATILE", (long)QL_OBSERVE_VOLATILE},
        {"OBSERVE_ATOMICS", (long)QL_OBSERVE_ATOMICS},
        {"OBSERVE_IO", (long)QL_OBSERVE_IO},
        {"OBSERVE_TRAPS", (long)QL_OBSERVE_TRAPS},
        {"OBSERVE_UNDEFINED_BEHAVIOR", (long)QL_OBSERVE_UNDEFINED_BEHAVIOR},
        {"OBSERVE_EXTERNAL_CALLS", (long)QL_OBSERVE_EXTERNAL_CALLS},
        {"OBSERVE_ALL", (long)QL_OBSERVE_ALL},
        {"MEMORY_IGNORE", (long)QL_MEMORY_IGNORE},
        {"MEMORY_FINAL_REACHABLE_STATE",
         (long)QL_MEMORY_FINAL_REACHABLE_STATE},
        {"MEMORY_ORDERED_WRITES", (long)QL_MEMORY_ORDERED_WRITES},
        {"MEMORY_FULL_TRACE", (long)QL_MEMORY_FULL_TRACE},
        {"EXTERNAL_CALLS_IGNORE", (long)QL_EXTERNAL_CALLS_IGNORE},
        {"EXTERNAL_CALLS_ORDERED_TRACE",
         (long)QL_EXTERNAL_CALLS_ORDERED_TRACE},
        {"VERDICT_UNKNOWN", (long)QL_VERDICT_UNKNOWN},
        {"VERDICT_PROVED_EQUIVALENT", (long)QL_VERDICT_PROVED_EQUIVALENT},
        {"VERDICT_PROVED_LEFT_REFINES_RIGHT",
         (long)QL_VERDICT_PROVED_LEFT_REFINES_RIGHT},
        {"VERDICT_PROVED_RIGHT_REFINES_LEFT",
         (long)QL_VERDICT_PROVED_RIGHT_REFINES_LEFT},
        {"VERDICT_COUNTEREXAMPLE", (long)QL_VERDICT_COUNTEREXAMPLE},
        {"VERDICT_BOUNDED_CLEAN", (long)QL_VERDICT_BOUNDED_CLEAN},
        {"EVIDENCE_PROOF", (long)QL_EVIDENCE_PROOF},
        {"EVIDENCE_COUNTEREXAMPLE", (long)QL_EVIDENCE_COUNTEREXAMPLE},
        {"EVIDENCE_BOUNDED", (long)QL_EVIDENCE_BOUNDED},
        {"EVIDENCE_UNKNOWN", (long)QL_EVIDENCE_UNKNOWN},
        {"OUTCOME_METHOD", (long)QL_PY_OUTCOME_METHOD},
        {"OUTCOME_UNSUPPORTED", (long)QL_PY_OUTCOME_UNSUPPORTED},
        {"OUTCOME_BUDGET", (long)QL_PY_OUTCOME_BUDGET},
        {"ANSWER_NOT_QUERIED", (long)QL_SMT_PRODUCT_ANSWER_NOT_QUERIED},
        {"ANSWER_SAT", (long)QL_SMT_PRODUCT_ANSWER_SAT},
        {"ANSWER_UNSAT", (long)QL_SMT_PRODUCT_ANSWER_UNSAT},
        {"ANSWER_UNKNOWN", (long)QL_SMT_PRODUCT_ANSWER_UNKNOWN},
        {NULL, 0}};
    size_t index;

    for (index = 0u; constants[index].name != NULL; ++index) {
        if (PyModule_AddIntConstant(module, constants[index].name,
                                    constants[index].value) != 0) {
            return -1;
        }
    }
    return PyModule_AddStringConstant(module, "ABI_TAG", "abi3");
}

static struct PyModuleDef module_definition = {
    PyModuleDef_HEAD_INIT,
    "quodlibet._quodlibet",
    "CPython C extension over the Quodlibet equivalence engine.",
    -1,
    methods,
    NULL,
    NULL,
    NULL,
    NULL};

PyMODINIT_FUNC PyInit__quodlibet(void) {
    PyObject *module = PyModule_Create(&module_definition);

    if (module == NULL) {
        return NULL;
    }
    if (quodlibet_error == NULL) {
        quodlibet_error = PyErr_NewExceptionWithDoc(
            "quodlibet._quodlibet.QuodlibetError",
            "A Quodlibet core call failed. `status` carries the ql_status "
            "value and `status_name` its spelling.",
            NULL, NULL);
        if (quodlibet_error == NULL) {
            Py_DECREF(module);
            return NULL;
        }
    }
    Py_INCREF(quodlibet_error);
    if (PyModule_AddObject(module, "QuodlibetError", quodlibet_error) != 0) {
        Py_DECREF(quodlibet_error);
        Py_DECREF(module);
        return NULL;
    }
    if (add_constants(module) != 0) {
        Py_DECREF(module);
        return NULL;
    }
    return module;
}
