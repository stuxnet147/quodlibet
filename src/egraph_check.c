#include "quodlibet/egraph_check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Nothing in this file calls the e-graph engine. It consumes a term table and
   a merge log as data, rebuilds its own union-find, and discharges each
   record against the rule catalogue. The only engine entry points used are
   the catalogue accessors, which are static description, and the snapshot
   harvester at the bottom, which reads a live graph through the public view
   API before any checking starts. */

struct ql_egraph_check_report {
    ql_allocator allocator;
    uint64_t record_count;
    uint64_t justified_count;
    uint64_t assumed_count;
    uint64_t rejected_count;
    ql_egraph_check_finding_v1 *findings;
    size_t finding_count;
    ql_digest rule_catalogue_digest;
    /* Final union-find, indexed by class identifier minus one. */
    ql_egraph_class_id *parent;
    size_t class_capacity;
    /* Seed class per term, indexed by term identifier minus one. */
    ql_egraph_class_id *seed;
    size_t term_count;
    /* Owned copy of the snapshot when the report came from a live graph. */
    ql_egraph_check_term_v1 *owned_terms;
    ql_egraph_merge_record_v1 *owned_records;
};

typedef struct ql_egraph_check_state {
    const ql_egraph_check_input_v1 *input;
    ql_egraph_check_report *report;
    ql_egraph_class_id *parent;
    size_t class_capacity;
    ql_egraph_class_id *seed;
} ql_egraph_check_state;

static const ql_allocator *resolve_allocator(const ql_allocator *allocator) {
    if (allocator != NULL && ql_allocator_is_valid(allocator)) {
        return allocator;
    }
    return ql_default_allocator();
}

/* --- union-find -------------------------------------------------------- */

static ql_egraph_class_id find_root(ql_egraph_check_state *state,
                                    ql_egraph_class_id class_id) {
    ql_egraph_class_id root = class_id;
    while (state->parent[(size_t)root - 1u] != root) {
        root = state->parent[(size_t)root - 1u];
    }
    while (state->parent[(size_t)class_id - 1u] != root) {
        const ql_egraph_class_id next = state->parent[(size_t)class_id - 1u];
        state->parent[(size_t)class_id - 1u] = root;
        class_id = next;
    }
    return root;
}

/* The representative is the smallest identifier in the class. That is a
   canonical form derived here, not a copy of the engine's tie-breaking, and
   it is what lets the recorded class snapshots be checked at all. */
static void union_classes(ql_egraph_check_state *state,
                          ql_egraph_class_id left,
                          ql_egraph_class_id right) {
    const ql_egraph_class_id left_root = find_root(state, left);
    const ql_egraph_class_id right_root = find_root(state, right);
    if (left_root == right_root) {
        return;
    }
    if (left_root < right_root) {
        state->parent[(size_t)right_root - 1u] = left_root;
    } else {
        state->parent[(size_t)left_root - 1u] = right_root;
    }
}

static ql_egraph_class_id class_of(ql_egraph_check_state *state,
                                   ql_egraph_term_id term) {
    return find_root(state, state->seed[(size_t)term - 1u]);
}

/* --- term predicates --------------------------------------------------- */

static int types_equal(ql_egraph_type left, ql_egraph_type right) {
    return left.kind == right.kind && left.bit_width == right.bit_width;
}

static const ql_egraph_check_term_v1 *term_at(
    const ql_egraph_check_state *state, ql_egraph_term_id term) {
    return &state->input->terms[(size_t)term - 1u];
}

static int constant_is_zero(const ql_egraph_check_term_v1 *term) {
    size_t index;
    if (term->constant_size == 0u || term->constant_le == NULL) {
        return 0;
    }
    for (index = 0u; index < term->constant_size; ++index) {
        if (term->constant_le[index] != 0u) {
            return 0;
        }
    }
    return 1;
}

static int constant_is_one(const ql_egraph_check_term_v1 *term) {
    size_t index;
    if (term->constant_size == 0u || term->constant_le == NULL) {
        return 0;
    }
    if (term->constant_le[0] != 1u) {
        return 0;
    }
    for (index = 1u; index < term->constant_size; ++index) {
        if (term->constant_le[index] != 0u) {
            return 0;
        }
    }
    return 1;
}

static int constant_is_ones(const ql_egraph_check_term_v1 *term) {
    const uint32_t used = term->type.bit_width & 7u;
    const uint8_t top =
        used == 0u ? UINT8_MAX : (uint8_t)((UINT32_C(1) << used) - 1u);
    size_t index;
    if (term->constant_size == 0u || term->constant_le == NULL) {
        return 0;
    }
    for (index = 0u; index + 1u < term->constant_size; ++index) {
        if (term->constant_le[index] != UINT8_MAX) {
            return 0;
        }
    }
    return term->constant_le[term->constant_size - 1u] == top;
}

static int matches_constant(const ql_egraph_check_term_v1 *term,
                            ql_egraph_rule_constant wanted,
                            ql_egraph_type type) {
    switch (wanted) {
    case QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE:
        return term->op == QL_EGRAPH_OP_BOOL_CONSTANT &&
               term->constant_size == 1u && term->constant_le != NULL &&
               term->constant_le[0] == 0u;
    case QL_EGRAPH_RULE_CONSTANT_BOOL_TRUE:
        return term->op == QL_EGRAPH_OP_BOOL_CONSTANT &&
               term->constant_size == 1u && term->constant_le != NULL &&
               term->constant_le[0] == 1u;
    case QL_EGRAPH_RULE_CONSTANT_BV_ZERO:
        return term->op == QL_EGRAPH_OP_BV_CONSTANT &&
               types_equal(term->type, type) && constant_is_zero(term);
    case QL_EGRAPH_RULE_CONSTANT_BV_ONE:
        return term->op == QL_EGRAPH_OP_BV_CONSTANT &&
               types_equal(term->type, type) && constant_is_one(term);
    case QL_EGRAPH_RULE_CONSTANT_BV_ONES:
        return term->op == QL_EGRAPH_OP_BV_CONSTANT &&
               types_equal(term->type, type) && constant_is_ones(term);
    default:
        return 0;
    }
}

/* Scans the whole term table because the checker keeps no index of its own.
   A class is small relative to the log, and correctness here outranks the
   constant factor. */
static int class_holds_constant(ql_egraph_check_state *state,
                                ql_egraph_class_id class_id,
                                ql_egraph_rule_constant wanted,
                                ql_egraph_type type) {
    const ql_egraph_class_id root = find_root(state, class_id);
    size_t index;
    if (wanted == QL_EGRAPH_RULE_CONSTANT_NONE) {
        return 0;
    }
    for (index = 0u; index < state->input->term_count; ++index) {
        const ql_egraph_check_term_v1 *term = &state->input->terms[index];
        if (find_root(state, term->initial_class) != root) {
            continue;
        }
        if (matches_constant(term, wanted, type)) {
            return 1;
        }
    }
    return 0;
}

/* An involution needs a unary application of op inside the operand class
   whose own operand lands in the recorded result class. Several such
   applications may exist, and any one of them justifies the merge, so all are
   tried rather than only the first. */
static int class_holds_application(ql_egraph_check_state *state,
                                   ql_egraph_class_id class_id,
                                   ql_egraph_operator op,
                                   ql_egraph_class_id wanted_operand_class) {
    const ql_egraph_class_id root = find_root(state, class_id);
    size_t index;
    for (index = 0u; index < state->input->term_count; ++index) {
        const ql_egraph_check_term_v1 *term = &state->input->terms[index];
        if (term->op != op || term->operand_count != 1u) {
            continue;
        }
        if (find_root(state, term->initial_class) != root) {
            continue;
        }
        if (class_of(state, term->operands[0]) == wanted_operand_class) {
            return 1;
        }
    }
    return 0;
}

/* --- findings ---------------------------------------------------------- */

static void set_finding(ql_egraph_check_finding_v1 *finding,
                        uint64_t sequence, ql_egraph_check_verdict verdict,
                        ql_egraph_check_code code, const char *rule_name,
                        const char *format, ...) {
    va_list arguments;
    memset(finding, 0, sizeof(*finding));
    finding->sequence = sequence;
    finding->verdict = verdict;
    finding->code = code;
    if (rule_name != NULL) {
        size_t length = strlen(rule_name);
        if (length >= sizeof(finding->rule_name)) {
            length = sizeof(finding->rule_name) - 1u;
        }
        memcpy(finding->rule_name, rule_name, length);
    }
    va_start(arguments, format);
    vsnprintf(finding->detail, sizeof(finding->detail), format, arguments);
    va_end(arguments);
}

/* --- rule discharge ---------------------------------------------------- */

typedef struct ql_egraph_check_outcome {
    ql_egraph_check_code code;
    char detail[QL_EGRAPH_CHECK_DETAIL_CAPACITY];
} ql_egraph_check_outcome;

static void reject(ql_egraph_check_outcome *outcome,
                   ql_egraph_check_code code, const char *format, ...) {
    va_list arguments;
    outcome->code = code;
    va_start(arguments, format);
    vsnprintf(outcome->detail, sizeof(outcome->detail), format, arguments);
    va_end(arguments);
}

static int operator_admitted(const ql_egraph_rule_descriptor_v1 *rule,
                             ql_egraph_operator op) {
    uint32_t slot;
    for (slot = 0u; slot < rule->subject_op_count; ++slot) {
        if (rule->subject_ops[slot] == op) {
            return 1;
        }
    }
    return 0;
}

static uint32_t resolve_result_operand(
    const ql_egraph_rule_descriptor_v1 *rule, uint32_t witness_index) {
    if (rule->result_operand == QL_EGRAPH_RULE_OPERAND_OTHER) {
        return witness_index == 0u ? 1u : 0u;
    }
    return rule->result_operand;
}

/* Premises that do not depend on which operand carried the witness. */
static int check_static_premises(ql_egraph_check_state *state,
                                 const ql_egraph_rule_descriptor_v1 *rule,
                                 const ql_egraph_check_term_v1 *subject,
                                 ql_egraph_check_outcome *outcome) {
    uint32_t index;
    if (!operator_admitted(rule, subject->op)) {
        reject(outcome, QL_EGRAPH_CHECK_CODE_WRONG_OPERATOR,
               "rule '%s' does not admit operator %u", rule->rule_name,
               (unsigned)subject->op);
        return 0;
    }
    if (subject->operand_count != rule->subject_arity) {
        reject(outcome, QL_EGRAPH_CHECK_CODE_WRONG_ARITY,
               "rule '%s' expects arity %u but the subject has %u",
               rule->rule_name, (unsigned)rule->subject_arity,
               (unsigned)subject->operand_count);
        return 0;
    }
    if (rule->minimum_bit_width != 0u &&
        subject->type.bit_width < rule->minimum_bit_width) {
        reject(outcome, QL_EGRAPH_CHECK_CODE_WIDTH_OUT_OF_RANGE,
               "rule '%s' needs at least %u bits but the subject has %u",
               rule->rule_name, (unsigned)rule->minimum_bit_width,
               (unsigned)subject->type.bit_width);
        return 0;
    }
    if (rule->maximum_bit_width != 0u &&
        subject->type.bit_width > rule->maximum_bit_width) {
        reject(outcome, QL_EGRAPH_CHECK_CODE_WIDTH_OUT_OF_RANGE,
               "rule '%s' allows at most %u bits but the subject has %u",
               rule->rule_name, (unsigned)rule->maximum_bit_width,
               (unsigned)subject->type.bit_width);
        return 0;
    }
    if ((rule->conditions & QL_EGRAPH_RULE_COND_OPERAND_SORT) != 0u) {
        for (index = 0u; index < subject->operand_count; ++index) {
            const ql_egraph_check_term_v1 *operand =
                term_at(state, subject->operands[index]);
            if (!types_equal(operand->type, subject->type)) {
                reject(outcome, QL_EGRAPH_CHECK_CODE_SIDE_CONDITION,
                       "rule '%s' requires operand %u to carry the subject "
                       "sort", rule->rule_name, (unsigned)index);
                return 0;
            }
        }
    }
    if ((rule->conditions & QL_EGRAPH_RULE_COND_OPERANDS_SAME_CLASS) != 0u) {
        const ql_egraph_class_id left =
            class_of(state, subject->operands[rule->equal_operands[0]]);
        const ql_egraph_class_id right =
            class_of(state, subject->operands[rule->equal_operands[1]]);
        if (left != right) {
            reject(outcome, QL_EGRAPH_CHECK_CODE_SIDE_CONDITION,
                   "rule '%s' requires operands %u and %u to already share a "
                   "class, but they are %u and %u",
                   rule->rule_name, (unsigned)rule->equal_operands[0],
                   (unsigned)rule->equal_operands[1], (unsigned)left,
                   (unsigned)right);
            return 0;
        }
    }
    return 1;
}

/* The shape check for one choice of witness operand. */
static int check_shape(ql_egraph_check_state *state,
                       const ql_egraph_rule_descriptor_v1 *rule,
                       const ql_egraph_check_term_v1 *subject,
                       const ql_egraph_check_term_v1 *result,
                       ql_egraph_class_id result_class,
                       uint32_t witness_index,
                       ql_egraph_check_outcome *outcome) {
    uint32_t result_index;
    switch (rule->shape) {
    case QL_EGRAPH_RULE_SHAPE_COMMUTATIVE:
        if (result->op != subject->op ||
            result->operand_count != subject->operand_count ||
            subject->operand_count != 2u) {
            reject(outcome, QL_EGRAPH_CHECK_CODE_RESULT_MISMATCH,
                   "rule '%s' rewrites to the same operator with two "
                   "operands", rule->rule_name);
            return 0;
        }
        if (class_of(state, result->operands[0]) !=
                class_of(state, subject->operands[1]) ||
            class_of(state, result->operands[1]) !=
                class_of(state, subject->operands[0])) {
            reject(outcome, QL_EGRAPH_CHECK_CODE_RESULT_MISMATCH,
                   "rule '%s' requires the operands to be exchanged",
                   rule->rule_name);
            return 0;
        }
        return 1;
    case QL_EGRAPH_RULE_SHAPE_INVOLUTION:
        if (!class_holds_application(state,
                                     class_of(state, subject->operands[0]),
                                     subject->op, result_class)) {
            reject(outcome, QL_EGRAPH_CHECK_CODE_SIDE_CONDITION,
                   "rule '%s' needs a nested application of operator %u "
                   "whose own operand lands in class %u",
                   rule->rule_name, (unsigned)subject->op,
                   (unsigned)result_class);
            return 0;
        }
        return 1;
    case QL_EGRAPH_RULE_SHAPE_IDEMPOTENT:
    case QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT:
    case QL_EGRAPH_RULE_SHAPE_SELECT_BRANCH:
    case QL_EGRAPH_RULE_SHAPE_SELECT_SAME:
        result_index = resolve_result_operand(rule, witness_index);
        if (result_index >= subject->operand_count) {
            reject(outcome, QL_EGRAPH_CHECK_CODE_RESULT_MISMATCH,
                   "rule '%s' names operand %u of a %u-operand subject",
                   rule->rule_name, (unsigned)result_index,
                   (unsigned)subject->operand_count);
            return 0;
        }
        if (class_of(state, subject->operands[result_index]) !=
            result_class) {
            reject(outcome, QL_EGRAPH_CHECK_CODE_RESULT_MISMATCH,
                   "rule '%s' rewrites to operand %u, whose class is %u, but "
                   "the record names class %u",
                   rule->rule_name, (unsigned)result_index,
                   (unsigned)class_of(state,
                                      subject->operands[result_index]),
                   (unsigned)result_class);
            return 0;
        }
        return 1;
    case QL_EGRAPH_RULE_SHAPE_ABSORBING_ELEMENT:
    case QL_EGRAPH_RULE_SHAPE_SELF_ANNIHILATION:
    case QL_EGRAPH_RULE_SHAPE_CONSTANT_FOLD:
    case QL_EGRAPH_RULE_SHAPE_REFLEXIVE:
        if (!class_holds_constant(state, result_class, rule->result_constant,
                                  subject->type)) {
            reject(outcome, QL_EGRAPH_CHECK_CODE_RESULT_MISMATCH,
                   "rule '%s' rewrites to a %s constant and the recorded "
                   "class does not hold one",
                   rule->rule_name,
                   ql_egraph_rule_constant_string(rule->result_constant));
            return 0;
        }
        return 1;
    default:
        reject(outcome, QL_EGRAPH_CHECK_CODE_UNKNOWN_RULE,
               "rule '%s' has shape %u, which this checker does not know",
               rule->rule_name, (unsigned)rule->shape);
        return 0;
    }
}

static void discharge_rewrite(ql_egraph_check_state *state,
                              const ql_egraph_merge_record_v1 *record,
                              ql_egraph_check_outcome *outcome) {
    ql_egraph_rule_descriptor_v1 rule;
    ql_error ignored;
    const ql_egraph_check_term_v1 *subject = term_at(state, record->lhs_term);
    const ql_egraph_check_term_v1 *result = term_at(state, record->rhs_term);
    const ql_egraph_class_id result_class = class_of(state, record->rhs_term);
    uint32_t candidate;
    int witness_found = 0;

    if (ql_egraph_rule_lookup(record->reason, &rule, &ignored) !=
        QL_STATUS_OK) {
        reject(outcome, QL_EGRAPH_CHECK_CODE_UNKNOWN_RULE,
               "no catalogue rule is named '%s'", record->reason);
        return;
    }
    if (!check_static_premises(state, &rule, subject, outcome)) {
        return;
    }
    if ((rule.conditions & QL_EGRAPH_RULE_COND_WITNESS_CONSTANT) == 0u) {
        if (check_shape(state, &rule, subject, result, result_class, 0u,
                        outcome)) {
            outcome->code = QL_EGRAPH_CHECK_CODE_NONE;
        }
        return;
    }
    /* Both operand classes may hold the witness, and the engine may have
       rewritten through either one. Any choice that discharges the rule
       justifies the merge, so every candidate is tried before rejecting. */
    for (candidate = 0u; candidate < subject->operand_count; ++candidate) {
        if (rule.witness_operand != QL_EGRAPH_RULE_OPERAND_ANY &&
            rule.witness_operand != candidate) {
            continue;
        }
        if (!class_holds_constant(state,
                                  class_of(state, subject->operands[candidate]),
                                  rule.witness_constant, subject->type)) {
            continue;
        }
        witness_found = 1;
        if (check_shape(state, &rule, subject, result, result_class,
                        candidate, outcome)) {
            outcome->code = QL_EGRAPH_CHECK_CODE_NONE;
            return;
        }
    }
    if (!witness_found) {
        reject(outcome, QL_EGRAPH_CHECK_CODE_SIDE_CONDITION,
               "rule '%s' needs a %s witness in an operand class and none is "
               "there", rule.rule_name,
               ql_egraph_rule_constant_string(rule.witness_constant));
    }
}

static void discharge_congruence(ql_egraph_check_state *state,
                                 const ql_egraph_merge_record_v1 *record,
                                 ql_egraph_check_outcome *outcome) {
    const ql_egraph_check_term_v1 *left = term_at(state, record->lhs_term);
    const ql_egraph_check_term_v1 *right = term_at(state, record->rhs_term);
    uint32_t index;

    if (strcmp(record->reason, "congruence") != 0) {
        reject(outcome, QL_EGRAPH_CHECK_CODE_CONGRUENCE_MISMATCH,
               "a congruence record must give the reason 'congruence', not "
               "'%s'", record->reason);
        return;
    }
    if (left->op != right->op ||
        left->operand_count != right->operand_count) {
        reject(outcome, QL_EGRAPH_CHECK_CODE_CONGRUENCE_MISMATCH,
               "congruence needs the same operator and arity on both sides");
        return;
    }
    if (left->operand_count == 0u) {
        /* Two distinct leaves are never congruent: a variable is identified
           by its symbol and a constant by its bytes, and the engine hash
           conses those, so a record here means the log is wrong. */
        reject(outcome, QL_EGRAPH_CHECK_CODE_CONGRUENCE_MISMATCH,
               "congruence does not apply to two distinct leaf terms");
        return;
    }
    for (index = 0u; index < left->operand_count; ++index) {
        const ql_egraph_class_id left_class =
            class_of(state, left->operands[index]);
        const ql_egraph_class_id right_class =
            class_of(state, right->operands[index]);
        if (left_class != right_class) {
            reject(outcome, QL_EGRAPH_CHECK_CODE_CONGRUENCE_MISMATCH,
                   "congruence needs operand %u to already be equal, but the "
                   "classes are %u and %u",
                   (unsigned)index, (unsigned)left_class,
                   (unsigned)right_class);
            return;
        }
    }
    outcome->code = QL_EGRAPH_CHECK_CODE_NONE;
}

/* --- input validation -------------------------------------------------- */

static ql_status validate_input(const ql_egraph_check_input_v1 *input,
                                ql_error *error) {
    ql_digest expected;
    size_t index;

    if (input == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph check input is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (input->struct_size != sizeof(*input)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "e-graph check input struct_size is %zu, expected %zu",
                     input->struct_size, sizeof(*input));
        return QL_STATUS_ABI_MISMATCH;
    }
    if (input->abi_version != QL_ABI_VERSION) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "e-graph check input abi_version is %u, expected %u",
                     input->abi_version, (unsigned)QL_ABI_VERSION);
        return QL_STATUS_ABI_MISMATCH;
    }
    if (input->schema_version != QL_EGRAPH_CHECK_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "e-graph check schema version is %u, expected %u",
                     input->schema_version,
                     (unsigned)QL_EGRAPH_CHECK_SCHEMA_VERSION);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (input->rule_catalogue_version != QL_EGRAPH_RULE_CATALOGUE_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "the log names rewrite catalogue version %u and this "
                     "build carries %u; the side conditions would come from "
                     "a different rule set",
                     input->rule_catalogue_version,
                     (unsigned)QL_EGRAPH_RULE_CATALOGUE_VERSION);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (ql_egraph_rule_catalogue_digest(&expected, error) != QL_STATUS_OK) {
        return QL_STATUS_INTERNAL_ERROR;
    }
    if (!ql_digest_equal(&expected, &input->rule_catalogue_digest)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "the log's rewrite catalogue digest does not match this "
                     "build's catalogue");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (input->term_count == 0u || input->terms == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph check input needs at least one term");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (input->record_count != 0u && input->records == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph check input claims %zu records and has none",
                     input->record_count);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < input->term_count; ++index) {
        const ql_egraph_check_term_v1 *term = &input->terms[index];
        uint32_t operand;
        if (term->struct_size != sizeof(*term)) {
            ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                         "term %zu has struct_size %zu, expected %zu", index,
                         term->struct_size, sizeof(*term));
            return QL_STATUS_ABI_MISMATCH;
        }
        if (term->term != (ql_egraph_term_id)(index + 1u)) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "the term table must be dense and ordered; slot %zu "
                         "names term %u", index, term->term);
            return QL_STATUS_INVALID_ARGUMENT;
        }
        if (term->initial_class == QL_EGRAPH_INVALID_CLASS ||
            (size_t)term->initial_class > input->term_count) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "term %u has initial class %u, which is out of "
                         "range", term->term, term->initial_class);
            return QL_STATUS_INVALID_ARGUMENT;
        }
        if (term->operand_count > QL_EGRAPH_MAX_ARITY) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "term %u has arity %u", term->term,
                         term->operand_count);
            return QL_STATUS_INVALID_ARGUMENT;
        }
        for (operand = 0u; operand < term->operand_count; ++operand) {
            /* Operands must name earlier terms. That is what makes the table
               acyclic, and a cyclic table would let the checker loop. */
            if (term->operands[operand] == QL_EGRAPH_INVALID_TERM ||
                term->operands[operand] >= term->term) {
                ql_error_set(error, QL_STATUS_CYCLE,
                             "term %u names operand %u, which is not an "
                             "earlier term", term->term,
                             term->operands[operand]);
                return QL_STATUS_CYCLE;
            }
        }
    }
    for (index = 0u; index < input->record_count; ++index) {
        const ql_egraph_merge_record_v1 *record = &input->records[index];
        if (record->lhs_term == QL_EGRAPH_INVALID_TERM ||
            (size_t)record->lhs_term > input->term_count ||
            record->rhs_term == QL_EGRAPH_INVALID_TERM ||
            (size_t)record->rhs_term > input->term_count) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "record %zu names a term outside the table", index);
            return QL_STATUS_INVALID_ARGUMENT;
        }
    }
    return QL_STATUS_OK;
}

/* --- replay ------------------------------------------------------------ */

static int check_operand_snapshot(ql_egraph_check_state *state,
                                  const ql_egraph_check_term_v1 *term,
                                  uint32_t recorded_count,
                                  const ql_egraph_class_id *recorded,
                                  ql_egraph_check_outcome *outcome,
                                  const char *side) {
    uint32_t index;
    if (recorded_count != term->operand_count) {
        reject(outcome, QL_EGRAPH_CHECK_CODE_OPERAND_SNAPSHOT,
               "the %s operand count is %u but term %u has %u", side,
               (unsigned)recorded_count, term->term,
               (unsigned)term->operand_count);
        return 0;
    }
    for (index = 0u; index < term->operand_count; ++index) {
        const ql_egraph_class_id actual =
            class_of(state, term->operands[index]);
        if (recorded[index] != actual) {
            reject(outcome, QL_EGRAPH_CHECK_CODE_OPERAND_SNAPSHOT,
                   "the %s operand %u snapshot is class %u but the replay "
                   "has %u", side, (unsigned)index,
                   (unsigned)recorded[index], (unsigned)actual);
            return 0;
        }
    }
    return 1;
}

static ql_status run_replay(ql_egraph_check_state *state, ql_error *error) {
    const ql_egraph_check_input_v1 *input = state->input;
    ql_egraph_check_report *report = state->report;
    size_t index;

    for (index = 0u; index < input->record_count; ++index) {
        const ql_egraph_merge_record_v1 *record = &input->records[index];
        const ql_egraph_check_term_v1 *left;
        const ql_egraph_check_term_v1 *right;
        ql_egraph_check_outcome outcome;
        ql_egraph_check_verdict verdict = QL_EGRAPH_CHECK_VERDICT_JUSTIFIED;
        ql_egraph_class_id left_class;
        ql_egraph_class_id right_class;

        memset(&outcome, 0, sizeof(outcome));
        left = term_at(state, record->lhs_term);
        right = term_at(state, record->rhs_term);
        left_class = class_of(state, record->lhs_term);
        right_class = class_of(state, record->rhs_term);

        if (record->sequence != (uint64_t)(index + 1u)) {
            reject(&outcome, QL_EGRAPH_CHECK_CODE_SEQUENCE_GAP,
                   "record %zu carries sequence %llu", index,
                   (unsigned long long)record->sequence);
        } else if (!types_equal(left->type, right->type)) {
            reject(&outcome, QL_EGRAPH_CHECK_CODE_TYPE_MISMATCH,
                   "terms %u and %u have different types", left->term,
                   right->term);
        } else if (record->lhs_class != left_class ||
                   record->rhs_class != right_class) {
            reject(&outcome, QL_EGRAPH_CHECK_CODE_STALE_CLASS,
                   "the record snapshots classes %u and %u but the replay "
                   "has %u and %u",
                   (unsigned)record->lhs_class, (unsigned)record->rhs_class,
                   (unsigned)left_class, (unsigned)right_class);
        } else if (left_class == right_class) {
            reject(&outcome, QL_EGRAPH_CHECK_CODE_ALREADY_MERGED,
                   "the two terms are already in class %u, so the record "
                   "does not describe a merge", (unsigned)left_class);
        } else if (!check_operand_snapshot(state, left,
                                           record->lhs_operand_count,
                                           record->lhs_operands, &outcome,
                                           "left") ||
                   !check_operand_snapshot(state, right,
                                           record->rhs_operand_count,
                                           record->rhs_operands, &outcome,
                                           "right")) {
            /* check_operand_snapshot filled the outcome. */
        } else {
            switch (record->kind) {
            case QL_EGRAPH_MERGE_AXIOM:
                verdict = QL_EGRAPH_CHECK_VERDICT_ASSUMED;
                reject(&outcome, QL_EGRAPH_CHECK_CODE_AXIOM,
                       "trusted equality '%s'; nothing in the log justifies "
                       "it", record->reason);
                break;
            case QL_EGRAPH_MERGE_REWRITE:
                discharge_rewrite(state, record, &outcome);
                break;
            case QL_EGRAPH_MERGE_CONGRUENCE:
                discharge_congruence(state, record, &outcome);
                break;
            default:
                reject(&outcome, QL_EGRAPH_CHECK_CODE_UNKNOWN_MERGE_KIND,
                       "merge kind %u is not one this checker knows",
                       (unsigned)record->kind);
                break;
            }
        }

        if (verdict != QL_EGRAPH_CHECK_VERDICT_ASSUMED &&
            outcome.code != QL_EGRAPH_CHECK_CODE_NONE) {
            verdict = QL_EGRAPH_CHECK_VERDICT_REJECTED;
        }
        if (verdict == QL_EGRAPH_CHECK_VERDICT_JUSTIFIED) {
            ++report->justified_count;
        } else {
            set_finding(&report->findings[report->finding_count],
                        record->sequence, verdict, outcome.code,
                        record->reason, "%s", outcome.detail);
            ++report->finding_count;
            if (verdict == QL_EGRAPH_CHECK_VERDICT_ASSUMED) {
                ++report->assumed_count;
            } else {
                ++report->rejected_count;
            }
        }

        /* A rejected merge is still applied so the remaining records are
           checked against the state the engine actually had. Stopping early
           would hide every later defect behind the first one. */
        union_classes(state, left_class, right_class);
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static void destroy_report(ql_egraph_check_report *report) {
    ql_allocator allocator;
    if (report == NULL) {
        return;
    }
    allocator = report->allocator;
    allocator.deallocate(allocator.user_data, report->findings);
    allocator.deallocate(allocator.user_data, report->parent);
    allocator.deallocate(allocator.user_data, report->seed);
    allocator.deallocate(allocator.user_data, report->owned_terms);
    allocator.deallocate(allocator.user_data, report->owned_records);
    allocator.deallocate(allocator.user_data, report);
}

void QL_CALL ql_egraph_check_input_init(ql_egraph_check_input_v1 *input) {
    if (input == NULL) {
        return;
    }
    memset(input, 0, sizeof(*input));
    input->struct_size = sizeof(*input);
    input->abi_version = QL_ABI_VERSION;
    input->schema_version = QL_EGRAPH_CHECK_SCHEMA_VERSION;
    input->rule_catalogue_version = QL_EGRAPH_RULE_CATALOGUE_VERSION;
    (void)ql_egraph_rule_catalogue_digest(&input->rule_catalogue_digest,
                                          NULL);
}

ql_status QL_CALL ql_egraph_check_replay(
    const ql_allocator *allocator, const ql_egraph_check_input_v1 *input,
    ql_egraph_check_report **report, ql_error *error) {
    const ql_allocator *actual = resolve_allocator(allocator);
    ql_egraph_check_report *created;
    ql_egraph_check_state state;
    ql_status status;
    size_t index;

    if (report == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph check report output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *report = NULL;
    status = validate_input(input, error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    created = actual->allocate(actual->user_data, sizeof(*created));
    if (created == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate the e-graph check report");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(created, 0, sizeof(*created));
    created->allocator = *actual;
    created->record_count = (uint64_t)input->record_count;
    created->term_count = input->term_count;
    created->class_capacity = input->term_count;
    created->rule_catalogue_digest = input->rule_catalogue_digest;

    created->parent = actual->allocate(
        actual->user_data, created->class_capacity * sizeof(*created->parent));
    created->seed = actual->allocate(
        actual->user_data, input->term_count * sizeof(*created->seed));
    if (input->record_count != 0u) {
        created->findings = actual->allocate(
            actual->user_data,
            input->record_count * sizeof(*created->findings));
    }
    if (created->parent == NULL || created->seed == NULL ||
        (input->record_count != 0u && created->findings == NULL)) {
        destroy_report(created);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate the e-graph replay state");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    for (index = 0u; index < created->class_capacity; ++index) {
        created->parent[index] = (ql_egraph_class_id)(index + 1u);
    }
    for (index = 0u; index < input->term_count; ++index) {
        created->seed[index] = input->terms[index].initial_class;
    }

    state.input = input;
    state.report = created;
    state.parent = created->parent;
    state.class_capacity = created->class_capacity;
    state.seed = created->seed;

    status = run_replay(&state, error);
    if (status != QL_STATUS_OK) {
        destroy_report(created);
        return status;
    }
    *report = created;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_egraph_check_graph(
    const ql_allocator *allocator, const ql_egraph *graph,
    ql_egraph_check_report **report, ql_error *error) {
    const ql_allocator *actual = resolve_allocator(allocator);
    ql_egraph_check_input_v1 input;
    ql_egraph_evidence_view_v1 evidence;
    ql_egraph_check_term_v1 *terms = NULL;
    ql_egraph_merge_record_v1 *records = NULL;
    const uint64_t term_count = ql_egraph_term_count(graph);
    ql_status status;
    uint64_t index;

    if (report == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph check report output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *report = NULL;
    if (graph == NULL || term_count == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a graph with at least one term is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = ql_egraph_evidence(graph, &evidence, error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    terms = actual->allocate(actual->user_data,
                             (size_t)term_count * sizeof(*terms));
    if (evidence.count != 0u) {
        records = actual->allocate(actual->user_data,
                                   evidence.count * sizeof(*records));
    }
    if (terms == NULL || (evidence.count != 0u && records == NULL)) {
        actual->deallocate(actual->user_data, terms);
        actual->deallocate(actual->user_data, records);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate the e-graph snapshot");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    for (index = 0u; index < term_count; ++index) {
        const ql_egraph_term_id term = (ql_egraph_term_id)(index + 1u);
        ql_egraph_term_view_v1 view;
        ql_egraph_class_id seed = QL_EGRAPH_INVALID_CLASS;
        uint32_t operand;
        status = ql_egraph_get_term(graph, term, &view, error);
        if (status == QL_STATUS_OK) {
            status = ql_egraph_term_initial_class(graph, term, &seed, error);
        }
        if (status != QL_STATUS_OK) {
            actual->deallocate(actual->user_data, terms);
            actual->deallocate(actual->user_data, records);
            return status;
        }
        memset(&terms[index], 0, sizeof(terms[index]));
        terms[index].struct_size = sizeof(terms[index]);
        terms[index].term = term;
        terms[index].initial_class = seed;
        terms[index].type = view.type;
        terms[index].op = view.op;
        terms[index].operand_count = view.operand_count;
        for (operand = 0u; operand < QL_EGRAPH_MAX_ARITY; ++operand) {
            terms[index].operands[operand] = view.operands[operand];
        }
        terms[index].symbol = view.symbol;
        terms[index].constant_le = view.constant_le;
        terms[index].constant_size = view.constant_size;
    }
    if (evidence.count != 0u) {
        memcpy(records, evidence.records,
               evidence.count * sizeof(*records));
    }

    ql_egraph_check_input_init(&input);
    input.terms = terms;
    input.term_count = (size_t)term_count;
    input.records = records;
    input.record_count = evidence.count;

    status = ql_egraph_check_replay(actual, &input, report, error);
    if (status != QL_STATUS_OK) {
        actual->deallocate(actual->user_data, terms);
        actual->deallocate(actual->user_data, records);
        return status;
    }
    /* The report keeps the snapshot alive because its term table is what the
       findings and the equivalence query refer to. */
    (*report)->owned_terms = terms;
    (*report)->owned_records = records;
    return QL_STATUS_OK;
}

void QL_CALL ql_egraph_check_report_release(ql_egraph_check_report *report) {
    destroy_report(report);
}

ql_status QL_CALL ql_egraph_check_report_get_view(
    const ql_egraph_check_report *report,
    ql_egraph_check_report_view_v1 *view, ql_error *error) {
    if (report == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph check report and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_EGRAPH_CHECK_SCHEMA_VERSION;
    view->all_merges_justified =
        (report->rejected_count == 0u && report->assumed_count == 0u) ? 1u
                                                                     : 0u;
    view->record_count = report->record_count;
    view->justified_count = report->justified_count;
    view->assumed_count = report->assumed_count;
    view->rejected_count = report->rejected_count;
    view->findings = report->findings;
    view->finding_count = report->finding_count;
    view->rule_catalogue_digest = report->rule_catalogue_digest;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_egraph_check_terms_equal(
    const ql_egraph_check_report *report, ql_egraph_term_id lhs,
    ql_egraph_term_id rhs, uint32_t *out_equal, ql_error *error) {
    ql_egraph_check_state state;
    if (report == NULL || out_equal == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "e-graph check report and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (lhs == QL_EGRAPH_INVALID_TERM || rhs == QL_EGRAPH_INVALID_TERM ||
        (size_t)lhs > report->term_count ||
        (size_t)rhs > report->term_count) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "term identifiers %u and %u are not both in the "
                     "replayed table", lhs, rhs);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&state, 0, sizeof(state));
    state.parent = report->parent;
    state.class_capacity = report->class_capacity;
    state.seed = report->seed;
    *out_equal = find_root(&state, report->seed[(size_t)lhs - 1u]) ==
                         find_root(&state, report->seed[(size_t)rhs - 1u])
                     ? 1u
                     : 0u;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

const char *QL_CALL ql_egraph_check_verdict_string(
    ql_egraph_check_verdict verdict) {
    switch (verdict) {
    case QL_EGRAPH_CHECK_VERDICT_JUSTIFIED:
        return "justified";
    case QL_EGRAPH_CHECK_VERDICT_ASSUMED:
        return "assumed";
    case QL_EGRAPH_CHECK_VERDICT_REJECTED:
        return "rejected";
    default:
        return "invalid";
    }
}

const char *QL_CALL ql_egraph_check_code_string(ql_egraph_check_code code) {
    switch (code) {
    case QL_EGRAPH_CHECK_CODE_AXIOM:
        return "axiom";
    case QL_EGRAPH_CHECK_CODE_SEQUENCE_GAP:
        return "sequence-gap";
    case QL_EGRAPH_CHECK_CODE_UNKNOWN_TERM:
        return "unknown-term";
    case QL_EGRAPH_CHECK_CODE_TYPE_MISMATCH:
        return "type-mismatch";
    case QL_EGRAPH_CHECK_CODE_STALE_CLASS:
        return "stale-class";
    case QL_EGRAPH_CHECK_CODE_ALREADY_MERGED:
        return "already-merged";
    case QL_EGRAPH_CHECK_CODE_OPERAND_SNAPSHOT:
        return "operand-snapshot";
    case QL_EGRAPH_CHECK_CODE_UNKNOWN_RULE:
        return "unknown-rule";
    case QL_EGRAPH_CHECK_CODE_WRONG_OPERATOR:
        return "wrong-operator";
    case QL_EGRAPH_CHECK_CODE_WRONG_ARITY:
        return "wrong-arity";
    case QL_EGRAPH_CHECK_CODE_SIDE_CONDITION:
        return "side-condition";
    case QL_EGRAPH_CHECK_CODE_RESULT_MISMATCH:
        return "result-mismatch";
    case QL_EGRAPH_CHECK_CODE_WIDTH_OUT_OF_RANGE:
        return "width-out-of-range";
    case QL_EGRAPH_CHECK_CODE_UNKNOWN_MERGE_KIND:
        return "unknown-merge-kind";
    case QL_EGRAPH_CHECK_CODE_CONGRUENCE_MISMATCH:
        return "congruence-mismatch";
    default:
        return "none";
    }
}
