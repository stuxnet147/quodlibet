#include "quodlibet/policy.h"

#include "quodlibet/budget.h"

#include <stdio.h>
#include <string.h>

#include "yyjson.h"

#define QL_POLICY_MAX_DESCRIPTION 255u
#define QL_POLICY_MAX_WEAKEN 8u
#define QL_POLICY_VERDICT_COUNT 6u

typedef struct ql_policy_class {
    char name[QL_POLICY_MAX_NAME + 1u];
    ql_policy_disposition disposition;
    ql_policy_claims claims;
    ql_policy_proof_trust proof_trust;
    uint32_t require_replayed_witness;
    ql_verdict verdicts[QL_POLICY_VERDICT_COUNT];
    uint32_t verdict_count;
    double score;
} ql_policy_class;

typedef struct ql_policy_weaken {
    ql_verdict from;
    ql_verdict to;
} ql_policy_weaken;

struct ql_policy {
    ql_allocator allocator;
    char name[QL_POLICY_MAX_NAME + 1u];
    char description[QL_POLICY_MAX_DESCRIPTION + 1u];
    ql_policy_class classes[QL_POLICY_MAX_CLASSES];
    size_t class_count;
    ql_policy_weaken weaken[QL_POLICY_MAX_WEAKEN];
    size_t weaken_count;
    char trusted_backends[QL_POLICY_MAX_TRUSTED_BACKENDS]
                         [QL_POLICY_MAX_NAME + 1u];
    size_t trusted_backend_count;
    size_t default_class;
};

static const ql_verdict all_verdicts[QL_POLICY_VERDICT_COUNT] = {
    QL_VERDICT_UNKNOWN,
    QL_VERDICT_PROVED_EQUIVALENT,
    QL_VERDICT_PROVED_LEFT_REFINES_RIGHT,
    QL_VERDICT_PROVED_RIGHT_REFINES_LEFT,
    QL_VERDICT_COUNTEREXAMPLE,
    QL_VERDICT_BOUNDED_CLEAN
};

const char *QL_CALL ql_policy_verdict_name(ql_verdict verdict) {
    switch (verdict) {
    case QL_VERDICT_UNKNOWN:
        return "unknown";
    case QL_VERDICT_PROVED_EQUIVALENT:
        return "proved_equivalent";
    case QL_VERDICT_PROVED_LEFT_REFINES_RIGHT:
        return "proved_left_refines_right";
    case QL_VERDICT_PROVED_RIGHT_REFINES_LEFT:
        return "proved_right_refines_left";
    case QL_VERDICT_COUNTEREXAMPLE:
        return "counterexample";
    case QL_VERDICT_BOUNDED_CLEAN:
        return "bounded_clean";
    default:
        return "invalid";
    }
}

ql_status QL_CALL ql_policy_verdict_parse(const char *text,
                                          ql_verdict *verdict,
                                          ql_error *error) {
    size_t index;

    if (text == NULL || verdict == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "verdict text and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < QL_POLICY_VERDICT_COUNT; ++index) {
        if (strcmp(text, ql_policy_verdict_name(all_verdicts[index])) == 0) {
            *verdict = all_verdicts[index];
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
    }
    ql_error_set(error, QL_STATUS_PARSE_ERROR, "unknown verdict '%s'", text);
    return QL_STATUS_PARSE_ERROR;
}

const char *QL_CALL ql_policy_disposition_string(
    ql_policy_disposition disposition) {
    switch (disposition) {
    case QL_POLICY_DISPOSITION_PASS:
        return "pass";
    case QL_POLICY_DISPOSITION_FAIL:
        return "fail";
    case QL_POLICY_DISPOSITION_ABSTAIN:
        return "abstain";
    default:
        return "invalid";
    }
}

const char *QL_CALL ql_policy_claims_string(ql_policy_claims claims) {
    switch (claims) {
    case QL_POLICY_CLAIMS_NONE:
        return "none";
    case QL_POLICY_CLAIMS_HEURISTIC:
        return "heuristic";
    case QL_POLICY_CLAIMS_EVIDENCE:
        return "evidence";
    case QL_POLICY_CLAIMS_PROOF:
        return "proof";
    default:
        return "invalid";
    }
}

const char *QL_CALL ql_policy_proof_trust_string(ql_policy_proof_trust trust) {
    switch (trust) {
    case QL_POLICY_PROOF_TRUST_UNSET:
        return "unset";
    case QL_POLICY_PROOF_TRUST_CHECKED:
        return "checked";
    case QL_POLICY_PROOF_TRUST_TRUSTED_BACKEND:
        return "trusted_backend";
    default:
        return "invalid";
    }
}

void QL_CALL ql_policy_evidence_init(ql_policy_evidence_v1 *evidence) {
    if (evidence == NULL) {
        return;
    }
    memset(evidence, 0, sizeof(*evidence));
    evidence->struct_size = sizeof(*evidence);
    evidence->abi_version = QL_POLICY_ABI_VERSION;
}

void QL_CALL ql_policy_result_init(ql_policy_result_v1 *result) {
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
    result->abi_version = QL_POLICY_ABI_VERSION;
    result->schema_version = QL_POLICY_RESULT_SCHEMA_VERSION;
    result->disposition = QL_POLICY_DISPOSITION_ABSTAIN;
}

static uint32_t verdict_is_proved(ql_verdict verdict) {
    return verdict == QL_VERDICT_PROVED_EQUIVALENT ||
                   verdict == QL_VERDICT_PROVED_LEFT_REFINES_RIGHT ||
                   verdict == QL_VERDICT_PROVED_RIGHT_REFINES_LEFT
               ? 1u
               : 0u;
}

/* Parsing */

typedef struct ql_policy_parser {
    ql_policy *policy;
    ql_error *error;
} ql_policy_parser;

static ql_status reject(ql_policy_parser *parser, const char *path,
                        const char *reason) {
    ql_error_set(parser->error, QL_STATUS_PARSE_ERROR, "%s: %s", path, reason);
    return QL_STATUS_PARSE_ERROR;
}

static ql_status reject_at(ql_policy_parser *parser, const char *path,
                           const char *reason, const char *detail) {
    ql_error_set(parser->error, QL_STATUS_PARSE_ERROR, "%s: %s '%s'", path,
                 reason, detail);
    return QL_STATUS_PARSE_ERROR;
}

static ql_status copy_bounded(ql_policy_parser *parser, const char *path,
                              const char *what, const char *text, char *target,
                              size_t capacity) {
    size_t length;

    if (text == NULL) {
        return reject(parser, path, "expects a string");
    }
    length = strlen(text);
    if (length == 0u) {
        ql_error_set(parser->error, QL_STATUS_PARSE_ERROR,
                     "%s: %s must not be empty", path, what);
        return QL_STATUS_PARSE_ERROR;
    }
    if (length >= capacity) {
        ql_error_set(parser->error, QL_STATUS_PARSE_ERROR,
                     "%s: %s exceeds %zu characters", path, what,
                     capacity - 1u);
        return QL_STATUS_PARSE_ERROR;
    }
    memcpy(target, text, length + 1u);
    return QL_STATUS_OK;
}

static ql_status require_only_keys(ql_policy_parser *parser, const char *path,
                                   yyjson_val *object,
                                   const char *const *allowed,
                                   size_t allowed_count) {
    yyjson_obj_iter iterator;
    yyjson_val *key;
    size_t index;

    yyjson_obj_iter_init(object, &iterator);
    while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
        const char *name = yyjson_get_str(key);
        uint32_t known = 0u;
        for (index = 0u; index < allowed_count; ++index) {
            if (name != NULL && strcmp(name, allowed[index]) == 0) {
                known = 1u;
                break;
            }
        }
        if (!known) {
            return reject_at(parser, path, "unknown key",
                             name != NULL ? name : "?");
        }
    }
    return QL_STATUS_OK;
}

static ql_status parse_disposition(ql_policy_parser *parser, const char *path,
                                   const char *text,
                                   ql_policy_disposition *output) {
    if (text != NULL && strcmp(text, "pass") == 0) {
        *output = QL_POLICY_DISPOSITION_PASS;
        return QL_STATUS_OK;
    }
    if (text != NULL && strcmp(text, "fail") == 0) {
        *output = QL_POLICY_DISPOSITION_FAIL;
        return QL_STATUS_OK;
    }
    if (text != NULL && strcmp(text, "abstain") == 0) {
        *output = QL_POLICY_DISPOSITION_ABSTAIN;
        return QL_STATUS_OK;
    }
    return reject_at(parser, path,
                     "disposition must be pass, fail or abstain",
                     text != NULL ? text : "?");
}

static ql_status parse_claims(ql_policy_parser *parser, const char *path,
                              const char *text, ql_policy_claims *output) {
    static const char *const names[] = {"none", "heuristic", "evidence",
                                        "proof"};
    size_t index;

    for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (text != NULL && strcmp(text, names[index]) == 0) {
            *output = (ql_policy_claims)index;
            return QL_STATUS_OK;
        }
    }
    return reject_at(parser, path,
                     "claims must be none, heuristic, evidence or proof",
                     text != NULL ? text : "?");
}

static ql_status parse_proof_trust(ql_policy_parser *parser, const char *path,
                                   const char *text,
                                   ql_policy_proof_trust *output) {
    if (text != NULL && strcmp(text, "checked") == 0) {
        *output = QL_POLICY_PROOF_TRUST_CHECKED;
        return QL_STATUS_OK;
    }
    if (text != NULL && strcmp(text, "trusted_backend") == 0) {
        *output = QL_POLICY_PROOF_TRUST_TRUSTED_BACKEND;
        return QL_STATUS_OK;
    }
    return reject_at(parser, path,
                     "proof_trust must be checked or trusted_backend",
                     text != NULL ? text : "?");
}

static ql_status parse_class(ql_policy_parser *parser, size_t class_index,
                             yyjson_val *value, uint32_t *claimed_verdicts) {
    static const char *const allowed[] = {
        "name",         "disposition",  "claims", "verdicts",
        "proof_trust",  "require_replayed_witness", "score"
    };
    ql_policy_class *target = &parser->policy->classes[class_index];
    char path[96];
    char member[128];
    yyjson_val *item;
    yyjson_val *element;
    yyjson_arr_iter iterator;
    size_t existing;
    size_t position = 0u;
    ql_status status;
    uint32_t has_proved = 0u;
    uint32_t has_counterexample = 0u;

    (void)snprintf(path, sizeof(path), "/classes/%zu", class_index);
    if (!yyjson_is_obj(value)) {
        return reject(parser, path, "expects an object");
    }
    status = require_only_keys(parser, path, value, allowed,
                               sizeof(allowed) / sizeof(allowed[0]));
    if (status != QL_STATUS_OK) {
        return status;
    }

    (void)snprintf(member, sizeof(member), "%s/name", path);
    status = copy_bounded(parser, member, "a class name",
                          yyjson_get_str(yyjson_obj_get(value, "name")),
                          target->name, sizeof(target->name));
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (existing = 0u; existing < class_index; ++existing) {
        if (strcmp(parser->policy->classes[existing].name, target->name) ==
            0) {
            return reject_at(parser, member, "duplicate class name",
                             target->name);
        }
    }

    (void)snprintf(member, sizeof(member), "%s/disposition", path);
    status = parse_disposition(
        parser, member, yyjson_get_str(yyjson_obj_get(value, "disposition")),
        &target->disposition);
    if (status != QL_STATUS_OK) {
        return status;
    }

    (void)snprintf(member, sizeof(member), "%s/claims", path);
    status = parse_claims(parser, member,
                          yyjson_get_str(yyjson_obj_get(value, "claims")),
                          &target->claims);
    if (status != QL_STATUS_OK) {
        return status;
    }

    item = yyjson_obj_get(value, "verdicts");
    (void)snprintf(member, sizeof(member), "%s/verdicts", path);
    if (!yyjson_is_arr(item) || yyjson_arr_size(item) == 0u) {
        return reject(parser, member, "expects a non-empty array");
    }
    yyjson_arr_iter_init(item, &iterator);
    while ((element = yyjson_arr_iter_next(&iterator)) != NULL) {
        char entry[160];
        ql_verdict verdict = QL_VERDICT_UNKNOWN;
        const char *text = yyjson_get_str(element);

        (void)snprintf(entry, sizeof(entry), "%s/%zu", member, position);
        if (position >= QL_POLICY_VERDICT_COUNT) {
            return reject(parser, entry, "lists more verdicts than exist");
        }
        if (ql_policy_verdict_parse(text, &verdict, NULL) != QL_STATUS_OK) {
            return reject_at(parser, entry, "unknown verdict",
                             text != NULL ? text : "?");
        }
        if ((claimed_verdicts[verdict] & 1u) != 0u) {
            return reject_at(parser, entry,
                             "verdict is already claimed by another class",
                             ql_policy_verdict_name(verdict));
        }
        claimed_verdicts[verdict] |= 1u;
        target->verdicts[position] = verdict;
        if (verdict_is_proved(verdict)) {
            has_proved = 1u;
        }
        if (verdict == QL_VERDICT_COUNTEREXAMPLE) {
            has_counterexample = 1u;
        }
        ++position;
    }
    target->verdict_count = (uint32_t)position;

    item = yyjson_obj_get(value, "require_replayed_witness");
    if (item != NULL) {
        if (!yyjson_is_bool(item)) {
            (void)snprintf(member, sizeof(member),
                           "%s/require_replayed_witness", path);
            return reject(parser, member, "expects a boolean");
        }
        target->require_replayed_witness = yyjson_get_bool(item) ? 1u : 0u;
    }

    item = yyjson_obj_get(value, "proof_trust");
    if (item != NULL) {
        (void)snprintf(member, sizeof(member), "%s/proof_trust", path);
        status = parse_proof_trust(parser, member, yyjson_get_str(item),
                                   &target->proof_trust);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }

    item = yyjson_obj_get(value, "score");
    if (item != NULL) {
        if (!yyjson_is_num(item)) {
            (void)snprintf(member, sizeof(member), "%s/score", path);
            return reject(parser, member, "expects a number");
        }
        target->score = yyjson_get_num(item);
    }

    /* A policy names already-justified verdicts. It never manufactures one, so
       a class may not attach a proof claim to a verdict that is not a proof. */
    if (target->claims == QL_POLICY_CLAIMS_PROOF) {
        for (existing = 0u; existing < target->verdict_count; ++existing) {
            if (!verdict_is_proved(target->verdicts[existing])) {
                (void)snprintf(member, sizeof(member), "%s/claims", path);
                return reject_at(
                    parser, member,
                    "a class claiming proof may only list proved verdicts, not",
                    ql_policy_verdict_name(target->verdicts[existing]));
            }
        }
    }
    if (has_counterexample && target->require_replayed_witness == 0u) {
        (void)snprintf(member, sizeof(member), "%s/require_replayed_witness",
                       path);
        return reject(parser, member,
                      "a class accepting counterexample must set it to true; "
                      "an unreplayed SAT model is not a counterexample");
    }
    /* Only a class that actually banks on a proved verdict, by counting it as
       a pass or by claiming proof strength, has to say why it trusts it. A
       class that abstains on proved_* promotes nothing. */
    if (has_proved && target->proof_trust == QL_POLICY_PROOF_TRUST_UNSET &&
        (target->disposition == QL_POLICY_DISPOSITION_PASS ||
         target->claims == QL_POLICY_CLAIMS_PROOF)) {
        (void)snprintf(member, sizeof(member), "%s/proof_trust", path);
        return reject(parser, member,
                      "a class accepting a proved verdict must state checked "
                      "or trusted_backend; a raw solver UNSAT is not a proof");
    }
    if (!has_proved && target->proof_trust != QL_POLICY_PROOF_TRUST_UNSET) {
        (void)snprintf(member, sizeof(member), "%s/proof_trust", path);
        return reject(parser, member,
                      "proof_trust belongs only to a class that accepts a "
                      "proved verdict");
    }
    return QL_STATUS_OK;
}

static ql_status parse_weaken(ql_policy_parser *parser, yyjson_val *array) {
    static const char *const allowed[] = {"from", "to"};
    yyjson_val *element;
    yyjson_arr_iter iterator;
    size_t position = 0u;
    ql_status status;

    if (array == NULL) {
        return QL_STATUS_OK;
    }
    if (!yyjson_is_arr(array)) {
        return reject(parser, "/weaken", "expects an array");
    }
    if (yyjson_arr_size(array) > QL_POLICY_MAX_WEAKEN) {
        return reject(parser, "/weaken", "lists too many entries");
    }
    yyjson_arr_iter_init(array, &iterator);
    while ((element = yyjson_arr_iter_next(&iterator)) != NULL) {
        char path[64];
        char member[128];
        ql_verdict from = QL_VERDICT_UNKNOWN;
        ql_verdict to = QL_VERDICT_UNKNOWN;
        const char *text;

        (void)snprintf(path, sizeof(path), "/weaken/%zu", position);
        if (!yyjson_is_obj(element)) {
            return reject(parser, path, "expects an object");
        }
        status = require_only_keys(parser, path, element, allowed, 2u);
        if (status != QL_STATUS_OK) {
            return status;
        }
        text = yyjson_get_str(yyjson_obj_get(element, "from"));
        (void)snprintf(member, sizeof(member), "%s/from", path);
        if (ql_policy_verdict_parse(text, &from, NULL) != QL_STATUS_OK) {
            return reject_at(parser, member, "unknown verdict",
                             text != NULL ? text : "?");
        }
        text = yyjson_get_str(yyjson_obj_get(element, "to"));
        (void)snprintf(member, sizeof(member), "%s/to", path);
        if (ql_policy_verdict_parse(text, &to, NULL) != QL_STATUS_OK) {
            return reject_at(parser, member, "unknown verdict",
                             text != NULL ? text : "?");
        }
        /* Weakening is the only rewrite a policy may perform. Anything else
           would be the policy inventing a verdict the core did not reach. */
        if (to != QL_VERDICT_UNKNOWN && to != from) {
            ql_error_set(parser->error, QL_STATUS_PARSE_ERROR,
                         "%s: a policy may weaken a verdict to unknown but "
                         "never strengthen '%s' into '%s'",
                         member, ql_policy_verdict_name(from),
                         ql_policy_verdict_name(to));
            return QL_STATUS_PARSE_ERROR;
        }
        parser->policy->weaken[position].from = from;
        parser->policy->weaken[position].to = to;
        ++position;
    }
    parser->policy->weaken_count = position;
    return QL_STATUS_OK;
}

static ql_status parse_trust(ql_policy_parser *parser, yyjson_val *object) {
    static const char *const allowed[] = {"trusted_backends"};
    yyjson_val *backends;
    yyjson_val *element;
    yyjson_arr_iter iterator;
    size_t position = 0u;
    ql_status status;

    if (object == NULL) {
        return QL_STATUS_OK;
    }
    if (!yyjson_is_obj(object)) {
        return reject(parser, "/trust", "expects an object");
    }
    status = require_only_keys(parser, "/trust", object, allowed, 1u);
    if (status != QL_STATUS_OK) {
        return status;
    }
    backends = yyjson_obj_get(object, "trusted_backends");
    if (backends == NULL) {
        return QL_STATUS_OK;
    }
    if (!yyjson_is_arr(backends)) {
        return reject(parser, "/trust/trusted_backends", "expects an array");
    }
    if (yyjson_arr_size(backends) > QL_POLICY_MAX_TRUSTED_BACKENDS) {
        return reject(parser, "/trust/trusted_backends",
                      "lists too many backends");
    }
    yyjson_arr_iter_init(backends, &iterator);
    while ((element = yyjson_arr_iter_next(&iterator)) != NULL) {
        char path[96];

        (void)snprintf(path, sizeof(path), "/trust/trusted_backends/%zu",
                       position);
        status = copy_bounded(parser, path, "a backend identity",
                              yyjson_get_str(element),
                              parser->policy->trusted_backends[position],
                              QL_POLICY_MAX_NAME + 1u);
        if (status != QL_STATUS_OK) {
            return status;
        }
        ++position;
    }
    parser->policy->trusted_backend_count = position;
    return QL_STATUS_OK;
}

static ql_status finish_policy(ql_policy_parser *parser,
                               const char *default_class) {
    size_t index;
    size_t class_index;

    for (index = 0u; index < parser->policy->class_count; ++index) {
        if (strcmp(parser->policy->classes[index].name, default_class) == 0) {
            parser->policy->default_class = index;
            break;
        }
    }
    if (index == parser->policy->class_count) {
        return reject_at(parser, "/default_class",
                         "names no declared class", default_class);
    }
    /* A trusted_backend class without a trusted backend list would trust
       nothing, which is the raw-UNSAT promotion wearing a different hat. */
    for (class_index = 0u; class_index < parser->policy->class_count;
         ++class_index) {
        if (parser->policy->classes[class_index].proof_trust ==
                QL_POLICY_PROOF_TRUST_TRUSTED_BACKEND &&
            parser->policy->trusted_backend_count == 0u) {
            char path[96];
            (void)snprintf(path, sizeof(path), "/classes/%zu/proof_trust",
                           class_index);
            return reject(parser, path,
                          "trusted_backend requires a non-empty "
                          "trust.trusted_backends list");
        }
    }
    return QL_STATUS_OK;
}

static void *json_allocate(void *context, size_t size) {
    ql_allocator *allocator = context;
    return allocator->allocate(allocator->user_data, size);
}

static void *json_reallocate(void *context, void *pointer, size_t old_size,
                             size_t size) {
    ql_allocator *allocator = context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = context;
    allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc make_json_allocator(ql_allocator *allocator) {
    yyjson_alc result;
    result.malloc = json_allocate;
    result.realloc = json_reallocate;
    result.free = json_deallocate;
    result.ctx = allocator;
    return result;
}

ql_status QL_CALL ql_policy_parse(const ql_allocator *allocator,
                                  const char *json, size_t json_size,
                                  ql_policy **output, ql_error *error) {
    static const char *const allowed[] = {"schema_version", "name",
                                          "description",    "classes",
                                          "default_class",  "weaken",
                                          "trust"};
    const ql_allocator *selected =
        ql_allocator_is_valid(allocator) ? allocator : ql_default_allocator();
    ql_allocator json_owner;
    yyjson_alc json_allocator;
    yyjson_read_err read_error;
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *item;
    yyjson_val *element;
    yyjson_arr_iter iterator;
    ql_policy *policy = NULL;
    ql_policy_parser parser;
    uint32_t claimed[QL_POLICY_VERDICT_COUNT + 1u];
    const char *default_class;
    ql_status status;
    size_t position = 0u;

    if (output == NULL || json == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "policy json and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (json_size == 0u) {
        json_size = strlen(json);
    }
    policy = selected->allocate(selected->user_data, sizeof(*policy));
    if (policy == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(policy, 0, sizeof(*policy));
    memset(claimed, 0, sizeof(claimed));
    policy->allocator = *selected;
    parser.policy = policy;
    parser.error = error;

    json_owner = *selected;
    json_allocator = make_json_allocator(&json_owner);
    document = yyjson_read_opts((char *)(uintptr_t)json, json_size, 0u,
                                &json_allocator, &read_error);
    if (document == NULL) {
        status = read_error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
                     ? QL_STATUS_OUT_OF_MEMORY
                     : QL_STATUS_PARSE_ERROR;
        ql_error_set(error, status,
                     "policy json is not valid JSON at byte %zu: %s",
                     read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        goto failure;
    }
    root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
        status = reject(&parser, "", "policy json root must be an object");
        goto failure;
    }
    status = require_only_keys(&parser, "", root, allowed,
                               sizeof(allowed) / sizeof(allowed[0]));
    if (status != QL_STATUS_OK) {
        goto failure;
    }

    item = yyjson_obj_get(root, "schema_version");
    if (!yyjson_is_uint(item) ||
        yyjson_get_uint(item) != QL_POLICY_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "/schema_version: policy schema version must be %u",
                     QL_POLICY_SCHEMA_VERSION);
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto failure;
    }

    status = copy_bounded(&parser, "/name", "a policy name",
                          yyjson_get_str(yyjson_obj_get(root, "name")),
                          policy->name, sizeof(policy->name));
    if (status != QL_STATUS_OK) {
        goto failure;
    }
    item = yyjson_obj_get(root, "description");
    if (item != NULL) {
        status = copy_bounded(&parser, "/description", "a description",
                              yyjson_get_str(item), policy->description,
                              sizeof(policy->description));
        if (status != QL_STATUS_OK) {
            goto failure;
        }
    }

    item = yyjson_obj_get(root, "classes");
    if (!yyjson_is_arr(item) || yyjson_arr_size(item) == 0u) {
        status = reject(&parser, "/classes", "expects a non-empty array");
        goto failure;
    }
    if (yyjson_arr_size(item) > QL_POLICY_MAX_CLASSES) {
        status = reject(&parser, "/classes", "declares too many classes");
        goto failure;
    }
    yyjson_arr_iter_init(item, &iterator);
    while ((element = yyjson_arr_iter_next(&iterator)) != NULL) {
        status = parse_class(&parser, position, element, claimed);
        if (status != QL_STATUS_OK) {
            goto failure;
        }
        ++position;
    }
    policy->class_count = position;

    status = parse_weaken(&parser, yyjson_obj_get(root, "weaken"));
    if (status != QL_STATUS_OK) {
        goto failure;
    }
    status = parse_trust(&parser, yyjson_obj_get(root, "trust"));
    if (status != QL_STATUS_OK) {
        goto failure;
    }

    default_class = yyjson_get_str(yyjson_obj_get(root, "default_class"));
    if (default_class == NULL) {
        status = reject(&parser, "/default_class", "expects a string");
        goto failure;
    }
    status = finish_policy(&parser, default_class);
    if (status != QL_STATUS_OK) {
        goto failure;
    }

    yyjson_doc_free(document);
    *output = policy;
    ql_error_clear(error);
    return QL_STATUS_OK;

failure:
    yyjson_doc_free(document);
    selected->deallocate(selected->user_data, policy);
    return status;
}

void QL_CALL ql_policy_destroy(ql_policy *policy) {
    ql_allocator allocator;

    if (policy == NULL) {
        return;
    }
    allocator = policy->allocator;
    allocator.deallocate(allocator.user_data, policy);
}

const char *QL_CALL ql_policy_name(const ql_policy *policy) {
    return policy != NULL ? policy->name : NULL;
}

const char *QL_CALL ql_policy_description(const ql_policy *policy) {
    return policy != NULL ? policy->description : NULL;
}

size_t QL_CALL ql_policy_class_count(const ql_policy *policy) {
    return policy != NULL ? policy->class_count : 0u;
}

ql_status QL_CALL ql_policy_class_at(const ql_policy *policy, size_t index,
                                     ql_policy_class_view_v1 *view,
                                     ql_error *error) {
    const ql_policy_class *entry;

    if (policy == NULL || view == NULL || view->struct_size < sizeof(*view) ||
        index >= policy->class_count) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "policy, a class view v1 and a valid index are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    entry = &policy->classes[index];
    view->abi_version = QL_POLICY_ABI_VERSION;
    view->disposition = entry->disposition;
    view->claims = entry->claims;
    view->proof_trust = entry->proof_trust;
    view->require_replayed_witness = entry->require_replayed_witness;
    view->verdict_count = entry->verdict_count;
    view->name = entry->name;
    view->verdicts = entry->verdicts;
    view->score = entry->score;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* Evaluation */

static const ql_policy_class *class_for(const ql_policy *policy,
                                        ql_verdict verdict) {
    size_t index;
    size_t position;

    for (index = 0u; index < policy->class_count; ++index) {
        for (position = 0u; position < policy->classes[index].verdict_count;
             ++position) {
            if (policy->classes[index].verdicts[position] == verdict) {
                return &policy->classes[index];
            }
        }
    }
    return &policy->classes[policy->default_class];
}

static uint32_t backend_is_trusted(const ql_policy *policy,
                                   const char *identity) {
    size_t index;

    if (identity == NULL) {
        return 0u;
    }
    for (index = 0u; index < policy->trusted_backend_count; ++index) {
        if (strcmp(policy->trusted_backends[index], identity) == 0) {
            return 1u;
        }
    }
    return 0u;
}

static void set_gate(ql_policy_result_v1 *result, const char *reason) {
    result->effective_verdict = QL_VERDICT_UNKNOWN;
    result->checked_bound = 0u;
    result->gated = 1u;
    (void)snprintf(result->gate_reason, sizeof(result->gate_reason), "%s",
                   reason);
}

ql_status QL_CALL ql_policy_evaluate(const ql_policy *policy,
                                     const ql_outcome_v1 *outcome,
                                     const ql_policy_evidence_v1 *evidence,
                                     ql_policy_result_v1 *result,
                                     ql_error *error) {
    ql_policy_evidence_v1 facts;
    const ql_policy_class *entry;
    size_t index;

    if (policy == NULL || outcome == NULL ||
        outcome->struct_size < sizeof(*outcome) || result == NULL ||
        result->struct_size < sizeof(*result)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "policy, outcome v1 and policy result v1 are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (evidence != NULL && evidence->struct_size < sizeof(*evidence)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "policy evidence v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    ql_policy_evidence_init(&facts);
    if (evidence != NULL) {
        facts = *evidence;
    }

    ql_policy_result_init(result);
    (void)snprintf(result->policy_name, sizeof(result->policy_name), "%s",
                   policy->name);
    result->reported_verdict = outcome->verdict;
    result->effective_verdict = outcome->verdict;
    result->checked_bound = outcome->checked_bound;

    /* The core's soundness rules run before the policy sees anything, so no
       policy can classify a verdict the evidence does not support. */
    if (facts.budget_exhausted != 0u ||
        (outcome->flags & QL_OUTCOME_FLAG_BUDGET_EXHAUSTED) != 0u) {
        set_gate(result, "budget-exhausted");
    } else if (outcome->verdict == QL_VERDICT_COUNTEREXAMPLE &&
               facts.witness_replayed == 0u) {
        set_gate(result, "unreplayed-sat-model");
    } else if (verdict_is_proved(outcome->verdict)) {
        entry = class_for(policy, outcome->verdict);
        if (entry->proof_trust == QL_POLICY_PROOF_TRUST_CHECKED &&
            facts.proof_checked == 0u) {
            set_gate(result, "unchecked-proof");
        } else if (entry->proof_trust ==
                       QL_POLICY_PROOF_TRUST_TRUSTED_BACKEND &&
                   !backend_is_trusted(policy, facts.backend_identity)) {
            set_gate(result, "untrusted-backend");
        }
        /* A class with no stated proof trust neither passes the verdict nor
           claims proof strength, so nothing is being promoted and there is
           nothing to withdraw. */
    }

    if (result->gated == 0u) {
        for (index = 0u; index < policy->weaken_count; ++index) {
            if (policy->weaken[index].from == result->effective_verdict) {
                result->effective_verdict = policy->weaken[index].to;
                result->weakened = 1u;
                if (result->effective_verdict == QL_VERDICT_UNKNOWN) {
                    result->checked_bound = 0u;
                }
                break;
            }
        }
    }

    entry = class_for(policy, result->effective_verdict);
    (void)snprintf(result->class_name, sizeof(result->class_name), "%s",
                   entry->name);
    result->disposition = entry->disposition;
    result->claims = entry->claims;
    result->score = entry->score;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* Serialization */

typedef struct ql_policy_writer {
    char *buffer;
    size_t capacity;
    size_t needed;
    uint32_t overflow;
} ql_policy_writer;

static void writer_append(ql_policy_writer *writer, const char *text) {
    size_t length = strlen(text);

    if (writer->needed + length < writer->capacity) {
        memcpy(writer->buffer + writer->needed, text, length);
    } else {
        writer->overflow = 1u;
    }
    writer->needed += length;
}

static void writer_append_escaped(ql_policy_writer *writer,
                                  const char *text) {
    char escape[8];
    const char *cursor;

    writer_append(writer, "\"");
    for (cursor = text; *cursor != '\0'; ++cursor) {
        unsigned char value = (unsigned char)*cursor;
        if (value == '"' || value == '\\') {
            escape[0] = '\\';
            escape[1] = (char)value;
            escape[2] = '\0';
            writer_append(writer, escape);
        } else if (value < 0x20u) {
            (void)snprintf(escape, sizeof(escape), "\\u%04x",
                           (unsigned int)value);
            writer_append(writer, escape);
        } else {
            escape[0] = (char)value;
            escape[1] = '\0';
            writer_append(writer, escape);
        }
    }
    writer_append(writer, "\"");
}

/* A canonical form has to be a fixed point: writing it, reading it back and
   writing it again must reach the same bytes, or the same policy acquires two
   identities and a store keyed on the text answers for the wrong one.

   Negative zero was the one value that broke it. "%.17g" writes -0.0 as "-0",
   the JSON reader takes "-0" as the integer zero and loses the sign, and the
   next write produces "0". Collapsing both zeros here is not a rounding: -0.0
   and 0.0 are the same number, and a canonical form is exactly where two
   spellings of one number become one text. Every other double this writes
   already round-trips, because "%.17g" carries enough digits to name a double
   uniquely. */
static void writer_append_number(ql_policy_writer *writer, double value) {
    char text[64];

    if (value == 0.0) {
        writer_append(writer, "0");
        return;
    }
    (void)snprintf(text, sizeof(text), "%.17g", value);
    writer_append(writer, text);
}

static void writer_append_uint(ql_policy_writer *writer, uint64_t value) {
    char text[32];

    (void)snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
    writer_append(writer, text);
}

static ql_status writer_finish(ql_policy_writer *writer, char *buffer,
                               size_t capacity, size_t *written,
                               ql_error *error) {
    if (written != NULL) {
        *written = writer->needed + 1u;
    }
    if (buffer == NULL || writer->overflow != 0u ||
        writer->needed + 1u > capacity) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a buffer of %zu bytes is required", writer->needed + 1u);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    buffer[writer->needed] = '\0';
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_policy_serialize(const ql_policy *policy, char *buffer,
                                      size_t capacity, size_t *written,
                                      ql_error *error) {
    ql_policy_writer writer;
    size_t index;
    size_t position;

    if (policy == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "policy is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&writer, 0, sizeof(writer));
    writer.buffer = buffer;
    writer.capacity = buffer != NULL ? capacity : 0u;

    writer_append(&writer, "{\"schema_version\":");
    writer_append_uint(&writer, QL_POLICY_SCHEMA_VERSION);
    writer_append(&writer, ",\"name\":");
    writer_append_escaped(&writer, policy->name);
    if (policy->description[0] != '\0') {
        writer_append(&writer, ",\"description\":");
        writer_append_escaped(&writer, policy->description);
    }
    writer_append(&writer, ",\"classes\":[");
    for (index = 0u; index < policy->class_count; ++index) {
        const ql_policy_class *entry = &policy->classes[index];
        if (index != 0u) {
            writer_append(&writer, ",");
        }
        writer_append(&writer, "{\"name\":");
        writer_append_escaped(&writer, entry->name);
        writer_append(&writer, ",\"disposition\":");
        writer_append_escaped(&writer,
                              ql_policy_disposition_string(entry->disposition));
        writer_append(&writer, ",\"claims\":");
        writer_append_escaped(&writer, ql_policy_claims_string(entry->claims));
        writer_append(&writer, ",\"verdicts\":[");
        for (position = 0u; position < entry->verdict_count; ++position) {
            if (position != 0u) {
                writer_append(&writer, ",");
            }
            writer_append_escaped(
                &writer, ql_policy_verdict_name(entry->verdicts[position]));
        }
        writer_append(&writer, "]");
        if (entry->proof_trust != QL_POLICY_PROOF_TRUST_UNSET) {
            writer_append(&writer, ",\"proof_trust\":");
            writer_append_escaped(
                &writer, ql_policy_proof_trust_string(entry->proof_trust));
        }
        if (entry->require_replayed_witness != 0u) {
            writer_append(&writer, ",\"require_replayed_witness\":true");
        }
        writer_append(&writer, ",\"score\":");
        writer_append_number(&writer, entry->score);
        writer_append(&writer, "}");
    }
    writer_append(&writer, "],\"default_class\":");
    writer_append_escaped(&writer,
                          policy->classes[policy->default_class].name);
    if (policy->weaken_count != 0u) {
        writer_append(&writer, ",\"weaken\":[");
        for (index = 0u; index < policy->weaken_count; ++index) {
            if (index != 0u) {
                writer_append(&writer, ",");
            }
            writer_append(&writer, "{\"from\":");
            writer_append_escaped(
                &writer, ql_policy_verdict_name(policy->weaken[index].from));
            writer_append(&writer, ",\"to\":");
            writer_append_escaped(
                &writer, ql_policy_verdict_name(policy->weaken[index].to));
            writer_append(&writer, "}");
        }
        writer_append(&writer, "]");
    }
    if (policy->trusted_backend_count != 0u) {
        writer_append(&writer, ",\"trust\":{\"trusted_backends\":[");
        for (index = 0u; index < policy->trusted_backend_count; ++index) {
            if (index != 0u) {
                writer_append(&writer, ",");
            }
            writer_append_escaped(&writer, policy->trusted_backends[index]);
        }
        writer_append(&writer, "]}");
    }
    writer_append(&writer, "}");
    return writer_finish(&writer, buffer, capacity, written, error);
}

ql_status QL_CALL ql_policy_result_serialize(const ql_policy_result_v1 *result,
                                             char *buffer, size_t capacity,
                                             size_t *written,
                                             ql_error *error) {
    ql_policy_writer writer;

    if (result == NULL || result->struct_size < sizeof(*result)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "policy result v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&writer, 0, sizeof(writer));
    writer.buffer = buffer;
    writer.capacity = buffer != NULL ? capacity : 0u;

    writer_append(&writer, "{\"schema_version\":");
    writer_append_uint(&writer, QL_POLICY_RESULT_SCHEMA_VERSION);
    writer_append(&writer, ",\"policy\":");
    writer_append_escaped(&writer, result->policy_name);
    writer_append(&writer, ",\"class\":");
    writer_append_escaped(&writer, result->class_name);
    writer_append(&writer, ",\"disposition\":");
    writer_append_escaped(&writer,
                          ql_policy_disposition_string(result->disposition));
    writer_append(&writer, ",\"claims\":");
    writer_append_escaped(&writer, ql_policy_claims_string(result->claims));
    writer_append(&writer, ",\"reported_verdict\":");
    writer_append_escaped(&writer,
                          ql_policy_verdict_name(result->reported_verdict));
    writer_append(&writer, ",\"effective_verdict\":");
    writer_append_escaped(&writer,
                          ql_policy_verdict_name(result->effective_verdict));
    writer_append(&writer, ",\"weakened\":");
    writer_append(&writer, result->weakened != 0u ? "true" : "false");
    writer_append(&writer, ",\"gated\":");
    writer_append(&writer, result->gated != 0u ? "true" : "false");
    writer_append(&writer, ",\"gate_reason\":");
    writer_append_escaped(&writer, result->gate_reason);
    writer_append(&writer, ",\"checked_bound\":");
    writer_append_uint(&writer, result->checked_bound);
    writer_append(&writer, ",\"score\":");
    writer_append_number(&writer, result->score);
    writer_append(&writer, "}");
    return writer_finish(&writer, buffer, capacity, written, error);
}

static ql_status parse_result_enum(const char *text, const char *what,
                                   const char *const *names, size_t count,
                                   uint32_t *output, ql_error *error) {
    size_t index;

    for (index = 0u; index < count; ++index) {
        if (text != NULL && strcmp(text, names[index]) == 0) {
            *output = (uint32_t)index;
            return QL_STATUS_OK;
        }
    }
    ql_error_set(error, QL_STATUS_PARSE_ERROR, "/%s: unknown value '%s'", what,
                 text != NULL ? text : "?");
    return QL_STATUS_PARSE_ERROR;
}

ql_status QL_CALL ql_policy_result_parse(const char *json, size_t json_size,
                                         ql_policy_result_v1 *result,
                                         ql_error *error) {
    static const char *const dispositions[] = {"pass", "fail", "abstain"};
    static const char *const claims[] = {"none", "heuristic", "evidence",
                                         "proof"};
    ql_allocator json_owner;
    yyjson_alc json_allocator;
    yyjson_read_err read_error;
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *item;
    uint32_t scratch = 0u;
    ql_status status;

    if (json == NULL || result == NULL ||
        result->struct_size < sizeof(*result)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "policy result json and a result v1 output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (json_size == 0u) {
        json_size = strlen(json);
    }
    json_owner = *ql_default_allocator();
    json_allocator = make_json_allocator(&json_owner);
    document = yyjson_read_opts((char *)(uintptr_t)json, json_size, 0u,
                                &json_allocator, &read_error);
    if (document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "policy result json is not valid JSON at byte %zu: %s",
                     read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    root = yyjson_doc_get_root(document);
    if (!yyjson_is_obj(root)) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "policy result json root must be an object");
        status = QL_STATUS_PARSE_ERROR;
        goto done;
    }
    item = yyjson_obj_get(root, "schema_version");
    if (!yyjson_is_uint(item) ||
        yyjson_get_uint(item) != QL_POLICY_RESULT_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "/schema_version: policy result schema version must be %u",
                     QL_POLICY_RESULT_SCHEMA_VERSION);
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto done;
    }

    ql_policy_result_init(result);
    (void)snprintf(result->policy_name, sizeof(result->policy_name), "%s",
                   yyjson_get_str(yyjson_obj_get(root, "policy")) != NULL
                       ? yyjson_get_str(yyjson_obj_get(root, "policy"))
                       : "");
    (void)snprintf(result->class_name, sizeof(result->class_name), "%s",
                   yyjson_get_str(yyjson_obj_get(root, "class")) != NULL
                       ? yyjson_get_str(yyjson_obj_get(root, "class"))
                       : "");
    (void)snprintf(result->gate_reason, sizeof(result->gate_reason), "%s",
                   yyjson_get_str(yyjson_obj_get(root, "gate_reason")) != NULL
                       ? yyjson_get_str(yyjson_obj_get(root, "gate_reason"))
                       : "");

    status = parse_result_enum(
        yyjson_get_str(yyjson_obj_get(root, "disposition")), "disposition",
        dispositions, 3u, &scratch, error);
    if (status != QL_STATUS_OK) {
        goto done;
    }
    result->disposition = (ql_policy_disposition)scratch;
    status = parse_result_enum(yyjson_get_str(yyjson_obj_get(root, "claims")),
                               "claims", claims, 4u, &scratch, error);
    if (status != QL_STATUS_OK) {
        goto done;
    }
    result->claims = (ql_policy_claims)scratch;

    status = ql_policy_verdict_parse(
        yyjson_get_str(yyjson_obj_get(root, "reported_verdict")),
        &result->reported_verdict, error);
    if (status != QL_STATUS_OK) {
        goto done;
    }
    status = ql_policy_verdict_parse(
        yyjson_get_str(yyjson_obj_get(root, "effective_verdict")),
        &result->effective_verdict, error);
    if (status != QL_STATUS_OK) {
        goto done;
    }

    item = yyjson_obj_get(root, "weakened");
    result->weakened = yyjson_is_bool(item) && yyjson_get_bool(item) ? 1u : 0u;
    item = yyjson_obj_get(root, "gated");
    result->gated = yyjson_is_bool(item) && yyjson_get_bool(item) ? 1u : 0u;
    item = yyjson_obj_get(root, "checked_bound");
    result->checked_bound = yyjson_is_uint(item) ? yyjson_get_uint(item) : 0u;
    item = yyjson_obj_get(root, "score");
    result->score = yyjson_is_num(item) ? yyjson_get_num(item) : 0.0;

    ql_error_clear(error);
    status = QL_STATUS_OK;

done:
    yyjson_doc_free(document);
    return status;
}
