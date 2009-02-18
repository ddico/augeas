/*
 * pathx.c: handling path expressions
 *
 * Copyright (C) 2007 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: David Lutterkort <dlutter@redhat.com>
 */

#include <config.h>
#include <internal.h>
#include <stdint.h>
#include <stdbool.h>
#include <memory.h>
#include <ctype.h>

static const char *const errcodes[] = {
    "no error",
    "empty name",
    "illegal string literal",
    "illegal number",
    "string missing ending ' or \"",
    "expected '='",
    "allocation failed",
    "unmatched ']'",
    "expected a '/'",
    "internal error",   /* PATHX_EINTERNAL */
    "type error"        /* PATHX_ETYPE */
};

/*
 * Path expressions are strings that use a notation modelled on XPath.
 */

enum type {
    T_NONE = 0,     /* Not a type */
    T_NODESET,
    T_BOOLEAN,
    T_NUMBER,
    T_STRING
};

enum expr_tag {
    E_LOCPATH,
    E_BINARY,
    E_VALUE,
    E_APP
};

enum binary_op {
    OP_EQ,         /* '='  */
    OP_NEQ,        /* '!=' */
    OP_PLUS,       /* '+'  */
    OP_MINUS,      /* '-'  */
    OP_STAR        /* '*'  */
};

struct pred {
    int               nexpr;
    struct expr     **exprs;
};

enum axis {
    SELF,
    CHILD,
    DESCENDANT,
    DESCENDANT_OR_SELF,
    PARENT,
    ANCESTOR,
    ROOT
};

/* This array is indexed by enum axis */
static const char *const axis_names[] = {
    "self",
    "child",
    "descendant",
    "descendant-or-self",
    "parent",
    "ancestor",
    "root"
};

static const char *const axis_sep = "::";

/* Doubly linked list of location steps. Besides the information from the
 * path expression, also contains information to iterate over a node set,
 * in particular, the context node CTX for the step, and the current node
 * CUR within that context.
 */
struct step {
    struct step *next;
    enum axis    axis;
    char        *name;              /* NULL to match any name */
    struct pred *predicates;
};

/* Iteration over the nodes on a step, ignoring the predicates */
static struct tree *step_first(struct step *step, struct tree *ctx);
static struct tree *step_next(struct step *step, struct tree *ctx,
                              struct tree *node);

struct pathx {
    struct state   *state;
    struct locpath *locpath;
    struct nodeset *nodeset;
    int             node;
    struct tree    *origin;
};

#define L_BRACK '['
#define R_BRACK ']'

struct locpath {
    struct step *steps;
};

struct nodeset {
    struct tree **nodes;
    size_t        used;
    size_t        size;
};

typedef uint32_t value_ind_t;

struct value {
    enum type tag;
    union {
        struct nodeset  *nodeset;     /* T_NODESET */
        int              number;      /* T_NUMBER  */
        char            *string;      /* T_STRING  */
        bool             boolval;     /* T_BOOLEAN */
    };
};

struct expr {
    enum expr_tag tag;
    enum type     type;
    union {
        struct locpath  *locpath;      /* E_LOCPATH */
        struct {                       /* E_BINARY */
            enum binary_op op;
            struct expr *left;
            struct expr *right;
        };
        value_ind_t      value_ind;    /* E_VALUE */
        struct {                       /* E_APP */
            const struct func *func;
            struct expr       *args[];
        };
    };
};

/* Internal state of the evaluator/parser */
struct state {
    pathx_errcode_t errcode;
    const char     *file;
    int             line;

    const char     *txt;  /* Entire expression */
    const char     *pos;  /* Current position within TXT during parsing */

    struct tree *ctx; /* The current node */
    uint            ctx_pos;
    uint            ctx_len;

    /* A table of all values. The table is dynamically reallocated, i.e.
     * pointers to struct value should not be used across calls that
     * might allocate new values
     *
     * value_pool[0] is always the boolean false, and value_pool[1]
     * always the boolean true
     */
    struct value  *value_pool;
    value_ind_t    value_pool_used;
    value_ind_t    value_pool_size;
    /* Stack of values (as indices into value_pool), with bottom of
       stack in values[0] */
    value_ind_t   *values;
    size_t         values_used;
    size_t         values_size;
    /* Stack of expressions, with bottom of stack in exprs[0] */
    struct expr  **exprs;
    size_t         exprs_used;
    size_t         exprs_size;
};

/* We consider NULL and the empty string to be equal */
ATTRIBUTE_PURE
static inline int streqx(const char *s1, const char *s2) {
    if (s1 == NULL)
        return (s2 == NULL || strlen(s2) == 0);
    if (s2 == NULL)
        return strlen(s1) == 0;
    return STREQ(s1, s2);
}

/* Functions */

typedef void (*func_impl_t)(struct state *state);

struct func {
    const char      *name;
    unsigned int     arity;
    enum type        type;
    const enum type *arg_types;
    func_impl_t      impl;
};

static void func_last(struct state *state);
static void func_position(struct state *state);

static const struct func builtin_funcs[] = {
    { .name = "last", .arity = 0, .type = T_NUMBER, .arg_types = NULL,
      .impl = func_last },
    { .name = "position", .arity = 0, .type = T_NUMBER, .arg_types = NULL,
      .impl = func_position }
};

#define CHECK_ERROR                                                     \
    if (state->errcode != PATHX_NOERROR) return

#define CHECK_ERROR_RET0                                                \
    if (state->errcode != PATHX_NOERROR) return 0

#define STATE_ERROR(state, err)                                         \
    do {                                                                \
        state->errcode = err;                                           \
        state->file = __FILE__;                                         \
        state->line = __LINE__;                                         \
    } while (0)

#define HAS_ERROR(state) (state->errcode != PATHX_NOERROR)

#define STATE_ENOMEM STATE_ERROR(state, PATHX_ENOMEM)

#define ENOMEM_ON_NULL(state, v)                                        \
    do {                                                                \
        if (v == NULL) {                                                \
            STATE_ERROR(state, PATHX_ENOMEM);                           \
            return NULL;                                                \
        }                                                               \
    } while (0);

/*
 * Free the various data structures
 */

static void free_expr(struct expr *expr);

static void free_pred(struct pred *pred) {
    if (pred == NULL)
        return;

    for (int i=0; i < pred->nexpr; i++) {
        free_expr(pred->exprs[i]);
    }
    free(pred->exprs);
    free(pred);
}

static void free_step(struct step *step) {
    while (step != NULL) {
        struct step *del = step;
        step = del->next;
        free(del->name);
        free_pred(del->predicates);
        free(del);
    }
}

static void free_locpath(struct locpath *locpath) {
    if (locpath == NULL)
        return;
    while (locpath->steps != NULL) {
        struct step *step = locpath->steps;
        locpath->steps = step->next;
        free(step->name);
        free_pred(step->predicates);
        free(step);
    }
    free(locpath);
}

static void free_expr(struct expr *expr) {
    if (expr == NULL)
        return;
    switch (expr->tag) {
    case E_LOCPATH:
        free_locpath(expr->locpath);
        break;
    case E_BINARY:
        free_expr(expr->left);
        free_expr(expr->right);
        break;
    case E_VALUE:
        break;
    case E_APP:
        for (int i=0; i < expr->func->arity; i++)
            free_expr(expr->args[i]);
        break;
    default:
        assert(0);
    }
    free(expr);
}

static void free_nodeset(struct nodeset *ns) {
    if (ns != NULL) {
        free(ns->nodes);
        free(ns);
    }
}

static void free_state(struct state *state) {
    if (state == NULL)
        return;

    for(int i=0; i < state->exprs_used; i++)
        free_expr(state->exprs[i]);
    free(state->exprs);

    for(int i=0; i < state->value_pool_used; i++) {
        struct value *v = state->value_pool + i;
        switch (v->tag) {
        case T_NODESET:
            free_nodeset(v->nodeset);
            break;
        case T_STRING:
            free(v->string);
            break;
        case T_BOOLEAN:
        case T_NUMBER:
            break;
        default:
            assert(0);
        }
    }
    free(state->value_pool);
    free(state->values);
    free(state);
}

void free_pathx(struct pathx *pathx) {
    if (pathx == NULL)
        return;
    free_state(pathx->state);
    free(pathx);
}

/*
 * Handling values
 */
static value_ind_t make_value(enum type tag, struct state *state) {
    assert(tag != T_BOOLEAN);

    if (state->value_pool_used >= state->value_pool_size) {
        value_ind_t new_size = 2*state->value_pool_size;
        if (new_size <= state->value_pool_size) {
            STATE_ENOMEM;
            return 0;
        }
        if (REALLOC_N(state->value_pool, new_size) < 0) {
            STATE_ENOMEM;
            return 0;
        }
        state->value_pool_size = new_size;
    }
    state->value_pool[state->value_pool_used].tag = tag;
    return state->value_pool_used++;
}

static value_ind_t pop_value_ind(struct state *state) {
    if (state->values_used > 0) {
        state->values_used -= 1;
        return state->values[state->values_used];
    } else {
        STATE_ERROR(state, PATHX_EINTERNAL);
        assert(0);
        return 0;
    }
}

static struct value *pop_value(struct state *state) {
    value_ind_t vind = pop_value_ind(state);
    if (HAS_ERROR(state))
        return NULL;
    return state->value_pool + vind;
}

static void push_value(value_ind_t vind, struct state *state) {
    if (state->values_used >= state->values_size) {
        size_t new_size = 2*state->values_size;
        if (new_size == 0) new_size = 8;
        if (REALLOC_N(state->values, new_size) < 0) {
            STATE_ENOMEM;
            return;
        }
        state->values_size = new_size;
    }
    state->values[state->values_used++] = vind;
}

static void push_boolean_value(int b, struct state *state) {
    push_value(b != 0, state);
}

ATTRIBUTE_PURE
static struct value *expr_value(struct expr *expr, struct state *state) {
    return state->value_pool + expr->value_ind;
}

/*************************************************************************
 * Evaluation
 ************************************************************************/
static void eval_expr(struct expr *expr, struct state *state);

static void func_last(struct state *state) {
    value_ind_t vind;

    vind = make_value(T_NUMBER, state);
    CHECK_ERROR;
    state->value_pool[vind].number = state->ctx_len;
    push_value(vind, state);
}

static void func_position(struct state *state) {
    value_ind_t vind;

    vind = make_value(T_NUMBER, state);
    CHECK_ERROR;
    state->value_pool[vind].number = state->ctx_pos;
    push_value(vind, state);
}

static int calc_eq_nodeset_nodeset(struct nodeset *ns1, struct nodeset *ns2,
                                   int neq) {
    for (int i1=0; i1 < ns1->used; i1++) {
        struct tree *t1 = ns1->nodes[i1];
        for (int i2=0; i2 < ns2->used; i2++) {
            struct tree *t2 = ns2->nodes[i2];
            if (neq) {
                if (!streqx(t1->value, t2->value))
                    return 1;
            } else {
                if (streqx(t1->value, t2->value))
                    return 1;
            }
        }
    }
    return 0;
}

static int calc_eq_nodeset_string(struct nodeset *ns, const char *s,
                                  int neq) {
    for (int i=0; i < ns->used; i++) {
        struct tree *t = ns->nodes[i];
        if (neq) {
            if (!streqx(t->value, s))
                return 1;
        } else {
            if (streqx(t->value, s))
                return 1;
        }
    }
    return 0;
}

static void eval_eq(struct state *state, int neq) {
    struct value *r = pop_value(state);
    struct value *l = pop_value(state);
    int res;

    if (l->tag == T_NODESET && r->tag == T_NODESET) {
        res = calc_eq_nodeset_nodeset(l->nodeset, r->nodeset, neq);
    } else if (l->tag == T_NODESET) {
        res = calc_eq_nodeset_string(l->nodeset, r->string, neq);
    } else if (r->tag == T_NODESET) {
        res = calc_eq_nodeset_string(r->nodeset, l->string, neq);
    } else if (l->tag == T_NUMBER && r->tag == T_NUMBER) {
        if (neq)
            res = (l->number != r->number);
        else
            res = (l->number == r->number);
    } else {
        assert(l->tag == T_STRING);
        assert(r->tag == T_STRING);
        res = streqx(l->string, r->string);
    }
    CHECK_ERROR;

    push_boolean_value(res, state);
}

static void eval_arith(struct state *state, enum binary_op op) {
    struct value *r = pop_value(state);
    struct value *l = pop_value(state);
    value_ind_t vind;
    int res;

    assert(l->tag == T_NUMBER);
    assert(r->tag == T_NUMBER);

    vind = make_value(T_NUMBER, state);
    CHECK_ERROR;

    if (op == OP_PLUS)
        res = l->number + r->number;
    else if (op == OP_MINUS)
        res = l->number - r->number;
    else if (op == OP_STAR)
        res = l->number * r->number;
    else
        assert(0);

    state->value_pool[vind].number = res;
    push_value(vind, state);
}

static void eval_binary(struct expr *expr, struct state *state) {
    eval_expr(expr->left, state);
    eval_expr(expr->right, state);
    CHECK_ERROR;

    switch (expr->op) {
    case OP_EQ:
        eval_eq(state, 0);
        break;
    case OP_NEQ:
        eval_eq(state, 1);
        break;
    case OP_MINUS:
    case OP_PLUS:
    case OP_STAR:
        eval_arith(state, expr->op);
        break;
    default:
        assert(0);
    }
}

static void eval_app(struct expr *expr, struct state *state) {
    assert(expr->tag == E_APP);

    for (int i=0; i < expr->func->arity; i++) {
        eval_expr(expr->args[i], state);
        CHECK_ERROR;
    }
    expr->func->impl(state);
}

static int eval_pred(struct expr *expr, struct state *state) {
    eval_expr(expr, state);
    struct value *v = pop_value(state);
    switch(v->tag) {
    case T_BOOLEAN:
        return v->boolval;
    case T_NUMBER:
        return (state->ctx_pos == v->number);
    case T_NODESET:
        return v->nodeset->used > 0;
    default:
        assert(0);
    }
}

static struct nodeset *make_nodeset(struct state *state) {
    struct nodeset *result;
    if (ALLOC(result) < 0)
        STATE_ENOMEM;
    return result;
}

static void ns_add(struct nodeset *ns, struct tree *node,
                   struct state *state) {
    if (ns->used >= ns->size) {
        size_t size = 2 * ns->size;
        if (size < 10) size = 10;
        if (REALLOC_N(ns->nodes, size) < 0)
            STATE_ENOMEM;
        ns->size = size;
    }
    ns->nodes[ns->used] = node;
    ns->used += 1;
}

/* Return an array of nodesets, one for each step in the locpath.
 *
 * On return, (*NS)[0] will contain state->ctx, and (*NS)[*MAXNS] will
 * contain the nodes that matched the entire locpath
 */
static void ns_from_locpath(struct locpath *lp, uint *maxns,
                            struct nodeset ***ns,
                            struct state *state) {
    struct tree *old_ctx = state->ctx;
    uint old_ctx_len = state->ctx_len;
    uint old_ctx_pos = state->ctx_pos;

    *maxns = 0;
    *ns = NULL;
    list_for_each(step, lp->steps)
        *maxns += 1;
    if (ALLOC_N(*ns, *maxns+1) < 0) {
        STATE_ERROR(state, PATHX_ENOMEM);
        goto error;
    }
    for (int i=0; i <= *maxns; i++) {
        (*ns)[i] = make_nodeset(state);
        if (HAS_ERROR(state))
            goto error;
    }
    ns_add((*ns)[0], state->ctx, state);

    uint cur_ns = 0;
    list_for_each(step, lp->steps) {
        struct nodeset *work = (*ns)[cur_ns];
        struct nodeset *next = (*ns)[cur_ns + 1];
        for (int i=0; i < work->used; i++) {
            for (struct tree *node = step_first(step, work->nodes[i]);
                 node != NULL;
                 node = step_next(step, work->nodes[i], node))
                ns_add(next, node, state);
        }
        // FIXME: Need to uniquify NS
        if (step->predicates != NULL) {
            for (int p=0; p < step->predicates->nexpr; p++) {
                state->ctx_len = next->used;
                state->ctx_pos = 1;
                for (int i=0; i < next->used; state->ctx_pos++) {
                    state->ctx = next->nodes[i];
                    if (eval_pred(step->predicates->exprs[p], state)) {
                        i+=1;
                    } else {
                        memmove(next->nodes + i, next->nodes + i+1,
                                sizeof(next->nodes[0]) * (next->used - (i+1)));
                        next->used -= 1;
                    }
                }
            }
        }
        cur_ns += 1;
    }

    state->ctx = old_ctx;
    state->ctx_pos = old_ctx_pos;
    state->ctx_len = old_ctx_len;
    return;
 error:
    if (*ns != NULL) {
        for (int i=0; i <= *maxns; i++)
            free_nodeset((*ns)[i]);
        FREE(*ns);
    }
    return;
}

static void eval_locpath(struct locpath *lp, struct state *state) {
    struct nodeset **ns = NULL;
    uint maxns;

    ns_from_locpath(lp, &maxns, &ns, state);
    CHECK_ERROR;

    value_ind_t vind = make_value(T_NODESET, state);
    CHECK_ERROR;
    state->value_pool[vind].nodeset = ns[maxns];
    push_value(vind, state);

    for (int i=0; i < maxns; i++)
        free_nodeset(ns[i]);
    FREE(ns);
}

static void eval_expr(struct expr *expr, struct state *state) {
    CHECK_ERROR;
    switch (expr->tag) {
    case E_LOCPATH:
        eval_locpath(expr->locpath, state);
        break;
    case E_BINARY:
        eval_binary(expr, state);
        break;
    case E_VALUE:
        push_value(expr->value_ind, state);
        break;
    case E_APP:
        eval_app(expr, state);
        break;
    default:
        assert(0);
    }
}

/*************************************************************************
 * Typechecker
 *************************************************************************/

static void check_expr(struct expr *expr, struct state *state);

/* Typecheck a list of predicates. A predicate is a function of
 * one of the following types:
 *
 * T_NODESET -> T_BOOLEAN
 * T_NUMBER  -> T_BOOLEAN  (position test)
 * T_BOOLEAN -> T_BOOLEAN
 */
static void check_preds(struct pred *pred, struct state *state) {
    for (int i=0; i < pred->nexpr; i++) {
        struct expr *e = pred->exprs[i];
        check_expr(e, state);
        CHECK_ERROR;
        if (e->type != T_NODESET && e->type != T_NUMBER &&
            e->type != T_BOOLEAN) {
            STATE_ERROR(state, PATHX_ETYPE);
            return;
        }
    }
}

static void check_locpath(struct expr *expr, struct state *state) {
    assert(expr->tag == E_LOCPATH);

    struct locpath *locpath = expr->locpath;
    list_for_each(s, locpath->steps) {
        if (s->predicates != NULL) {
            check_preds(s->predicates, state);
            CHECK_ERROR;
        }
    }
    expr->type = T_NODESET;
}

static void check_app(struct expr *expr, struct state *state) {
    assert(expr->tag == E_APP);
    for (int i=0; i < expr->func->arity; i++) {
        check_expr(expr->args[i], state);
        CHECK_ERROR;
        if (expr->args[i]->type != expr->func->arg_types[i]) {
            STATE_ERROR(state, PATHX_ETYPE);
            return;
        }
    }
    expr->type = expr->func->type;
}

/* Check the binary operators. Type rules:
 *
 * '=', '!='  : T_NODESET -> T_NODESET -> T_BOOLEAN
 *              T_STRING  -> T_NODESET -> T_BOOLEAN
 *              T_NODESET -> T_STRING  -> T_BOOLEAN
 *              T_NUMBER  -> T_NUMBER  -> T_BOOLEAN
 *
 * '+', '-', '*': T_NUMBER -> T_NUMBER -> T_NUMBER
 *
 */
static void check_binary(struct expr *expr, struct state *state) {
    check_expr(expr->left, state);
    check_expr(expr->right, state);
    CHECK_ERROR;

    enum type l = expr->left->type;
    enum type r = expr->right->type;
    int  ok = 1;
    enum type res;

    switch(expr->op) {
    case OP_EQ:
    case OP_NEQ:
        ok = ((l == T_NODESET || l == T_STRING)
              && (r == T_NODESET || r == T_STRING))
            || (l == T_NUMBER && r == T_NUMBER);;
        res = T_BOOLEAN;
        break;
    case OP_PLUS:
    case OP_MINUS:
    case OP_STAR:
        ok =  (l == T_NUMBER && r == T_NUMBER);
        res = T_NUMBER;
        break;
    default:
        assert(0);
    }
    if (! ok) {
        STATE_ERROR(state, PATHX_ETYPE);
    } else {
        expr->type = res;
    }
}

/* Typecheck an expression */
static void check_expr(struct expr *expr, struct state *state) {
    CHECK_ERROR;
    switch(expr->tag) {
    case E_LOCPATH:
        check_locpath(expr, state);
        break;
    case E_BINARY:
        check_binary(expr, state);
        break;
    case E_VALUE:
        expr->type = expr_value(expr, state)->tag;
        break;
    case E_APP:
        check_app(expr, state);
        break;
    default:
        assert(0);
    }
}

/*
 * Utility functions for the parser
 */

static void skipws(struct state *state) {
    while (isspace(*state->pos)) state->pos += 1;
}

static int match(struct state *state, char m) {
    skipws(state);

    if (*state->pos == '\0')
        return 0;
    if (*state->pos == m) {
        state->pos += 1;
        return 1;
    }
    return 0;
}

static int peek(struct state *state, const char *chars) {
    return strchr(chars, *state->pos) != NULL;
}

/* Return 1 if STATE->POS starts with TOKEN, followed by optional
 * whitespace, followed by FOLLOW. In that case, STATE->POS is set to the
 * first character after FOLLOW. Return 0 otherwise and leave STATE->POS
 * unchanged.
 */
static int looking_at(struct state *state, const char *token,
                      const char *follow) {
    if (STREQLEN(state->pos, token, strlen(token))) {
        const char *p = state->pos + strlen(token);
        while (isspace(*p)) p++;
        if (STREQLEN(p, follow, strlen(follow))) {
            state->pos = p + strlen(follow);
            return 1;
        }
    }
    return 0;
}

/*************************************************************************
 * The parser
 *************************************************************************/

static void parse_expr(struct state *state);

static struct expr* pop_expr(struct state *state) {
    if (state->exprs_used > 0) {
        state->exprs_used -= 1;
        return state->exprs[state->exprs_used];
    } else {
        STATE_ERROR(state, PATHX_EINTERNAL);
        assert(0);
        return NULL;
    }
}

static void push_expr(struct expr *expr, struct state *state) {
    if (state->exprs_used >= state->exprs_size) {
        size_t new_size = 2*state->exprs_size;
        if (new_size == 0) new_size = 8;
        if (REALLOC_N(state->exprs, new_size) < 0) {
            STATE_ENOMEM;
            return;
        }
        state->exprs_size = new_size;
    }
    state->exprs[state->exprs_used++] = expr;
}

static void push_new_binary_op(enum binary_op op, struct state *state) {
    struct expr *expr = NULL;
    if (ALLOC(expr) < 0) {
        STATE_ENOMEM;
        return;
    }

    expr->tag = E_BINARY;
    expr->op  = op;
    expr->right = pop_expr(state);
    expr->left = pop_expr(state);
    push_expr(expr, state);
}

/*
 * Name ::= /[^/\[ \t\n]+/
 */
static char *parse_name(struct state *state) {
    const char *s = state->pos;
    char *result;

    while (*state->pos != '\0' &&
           *state->pos != L_BRACK && *state->pos != SEP &&
           *state->pos != R_BRACK && *state->pos != '=' &&
           !isspace(*state->pos)) {
        if (*state->pos == '\\') {
            state->pos += 1;
            if (*state->pos == '\0') {
                STATE_ERROR(state, PATHX_ENAME);
                return NULL;
            }
        }
        state->pos += 1;
    }

    if (state->pos == s) {
        STATE_ERROR(state, PATHX_ENAME);
        return NULL;
    }

    result = strndup(s, state->pos - s);
    if (result == NULL) {
        STATE_ENOMEM;
        return NULL;
    }

    char *p = result;
    for (char *t = result; *t != '\0'; t++, p++) {
        if (*t == '\\')
            t += 1;
        *p = *t;
    }
    *p = '\0';

    return result;
}

/*
 * Predicate    ::= "[" Expr "]" *
 */
static struct pred *parse_predicates(struct state *state) {
    struct pred *pred = NULL;
    int nexpr = 0;

    while (match(state, L_BRACK)) {
        parse_expr(state);
        nexpr += 1;
        CHECK_ERROR_RET0;

        if (! match(state, R_BRACK)) {
            STATE_ERROR(state, PATHX_EPRED);
            return NULL;
        }
        skipws(state);
    }

    if (nexpr == 0)
        return NULL;

    if (ALLOC(pred) < 0) {
        STATE_ENOMEM;
        return NULL;
    }
    pred->nexpr = nexpr;

    if (ALLOC_N(pred->exprs, nexpr) < 0) {
        free_pred(pred);
        STATE_ENOMEM;
        return NULL;
    }

    for (int i = nexpr - 1; i >= 0; i--)
        pred->exprs[i] = pop_expr(state);

    return pred;
}

/*
 * Step ::= AxisSpecifier NameTest Predicate* | '.' | '..'
 * AxisSpecifier ::= AxisName '::' | <epsilon>
 * AxisName ::= 'ancestor'
 *            | 'ancestor-or-self'
 *            | 'child'
 *            | 'descendant'
 *            | 'descendant-or-self'
 *            | 'parent'
 *            | 'self'
 *            | 'root'
 */
static struct step *parse_step(struct state *state) {
    struct step *step;

    if (ALLOC(step) < 0) {
        STATE_ENOMEM;
        return NULL;
    }

    if (*state->pos == '.' && state->pos[1] == '.') {
        state->pos += 2;
        step->axis = PARENT;
    } else if (match(state, '.')) {
        step->axis = SELF;
    } else {
        step->axis = CHILD;
        for (int i = 0; i < ARRAY_CARDINALITY(axis_names); i++) {
            if (looking_at(state, axis_names[i], "::")) {
                step->axis = i;
                break;
            }
        }

        if (! match(state, '*')) {
            step->name = parse_name(state);
            if (HAS_ERROR(state))
                goto error;
        }

        step->predicates = parse_predicates(state);
        if (HAS_ERROR(state))
            goto error;
    }
    return step;

 error:
    free_step(step);
    return NULL;
}

static struct step *make_step(enum axis axis, struct state *state) {
    struct step *result = NULL;

    if (ALLOC(result) < 0) {
        STATE_ENOMEM;
        return NULL;
    }
    result->axis = axis;
    return result;
}

/*
 * RelativeLocationPath ::= Step
 *                        | RelativeLocationPath '/' Step
 *                        | AbbreviatedRelativeLocationPath
 * AbbreviatedRelativeLocationPath ::= RelativeLocationPath '//' Step
 *
 * The above is the same as
 * RelativeLocationPath ::= Step ('/' Step | '//' Step)*
 */
static struct locpath *
parse_relative_location_path(struct state *state) {
    struct step *step = NULL;
    struct locpath *locpath = NULL;

    step = parse_step(state);
    CHECK_ERROR_RET0;

    if (ALLOC(locpath) < 0) {
        STATE_ENOMEM;
        goto error;
    }
    list_append(locpath->steps, step);
    step = NULL;

    while (match(state, '/')) {
        if (*state->pos == '/') {
            state->pos += 1;
            step = make_step(DESCENDANT_OR_SELF, state);
            ENOMEM_ON_NULL(state, step);
            list_append(locpath->steps, step);
        }
        step = parse_step(state);
        if (HAS_ERROR(state))
            goto error;
        list_append(locpath->steps, step);
        step = NULL;
    }
    return locpath;

 error:
    free_step(step);
    free_locpath(locpath);
    return NULL;
}

/*
 * LocationPath ::= RelativeLocationPath | AbsoluteLocationPath
 * AbsoluteLocationPath ::= '/' RelativeLocationPath?
 *                        | AbbreviatedAbsoluteLocationPath
 * AbbreviatedAbsoluteLocationPath ::= '//' RelativeLocationPath
 *
 */
static void parse_location_path(struct state *state) {
    struct expr *expr = NULL;
    struct locpath *locpath = NULL;

    if (match(state, '/')) {
        if (*state->pos == '/') {
            state->pos += 1;
            locpath = parse_relative_location_path(state);
            if (HAS_ERROR(state))
                return;
            struct step *step = make_step(DESCENDANT_OR_SELF, state);
            if (HAS_ERROR(state))
                goto error;
            list_cons(locpath->steps, step);
        } else {
            if (*state->pos != '\0') {
                locpath = parse_relative_location_path(state);
            } else {
                if (ALLOC(locpath) < 0)
                    goto err_nomem;
            }
            struct step *step = make_step(ROOT, state);
            if (HAS_ERROR(state))
                goto error;
            list_cons(locpath->steps, step);
        }
    } else {
        locpath = parse_relative_location_path(state);
    }

    if (ALLOC(expr) < 0)
        goto err_nomem;
    expr->tag = E_LOCPATH;
    expr->locpath = locpath;
    push_expr(expr, state);
    return;

 err_nomem:
    STATE_ENOMEM;
 error:
    free_expr(expr);
    free_locpath(locpath);
    return;
}

/*
 * Number       ::= /[0-9]+/
 */
static void parse_number(struct state *state) {
    struct expr *expr = NULL;
    unsigned long val;
    char *end;

    errno = 0;
    val = strtoul(state->pos, &end, 10);
    if (errno || end == state->pos || (int) val != val) {
        STATE_ERROR(state, PATHX_ENUMBER);
        return;
    }

    state->pos = end;

    if (ALLOC(expr) < 0)
        goto err_nomem;
    expr->tag = E_VALUE;
    expr->value_ind = make_value(T_NUMBER, state);
    if (HAS_ERROR(state))
        goto error;
    expr_value(expr, state)->number = val;

    push_expr(expr, state);
    return;

 err_nomem:
    STATE_ENOMEM;
 error:
    free_expr(expr);
    return;
}

/*
 * Literal ::= '"' /[^"]* / '"' | "'" /[^']* / "'"
 */
static void parse_literal(struct state *state) {
    char delim;
    const char *s;
    struct expr *expr = NULL;

    if (*state->pos == '"')
        delim = '"';
    else if (*state->pos == '\'')
        delim = '\'';
    else {
        STATE_ERROR(state, PATHX_ESTRING);
        return;
    }
    state->pos += 1;

    s = state->pos;
    while (*state->pos != '\0' && *state->pos != delim) state->pos += 1;

    if (*state->pos != delim) {
        STATE_ERROR(state, PATHX_EDELIM);
        return;
    }
    state->pos += 1;

    if (ALLOC(expr) < 0)
        goto err_nomem;
    expr->tag = E_VALUE;
    expr->value_ind = make_value(T_STRING, state);
    if (HAS_ERROR(state))
        goto error;
    expr_value(expr, state)->string = strndup(s, state->pos - s - 1);
    if (expr_value(expr, state)->string == NULL)
        goto err_nomem;

    push_expr(expr, state);
    return;

 err_nomem:
    STATE_ENOMEM;
 error:
    free_expr(expr);
    return;
}

/*
 * FunctionCall ::= Name '(' ( Expr ( ',' Expr )* )? ')'
 */
static void parse_function_call(struct state *state) {
    const struct func *func = NULL;
    struct expr *expr = NULL;
    int nargs = 0;

    for (int i=0; i < ARRAY_CARDINALITY(builtin_funcs); i++) {
        if (looking_at(state, builtin_funcs[i].name, "("))
            func = builtin_funcs + i;
    }
    if (func == NULL) {
        STATE_ERROR(state, PATHX_ENAME);
        return;
    }

    if (! match(state, ')')) {
        do {
            nargs += 1;
            parse_expr(state);
            CHECK_ERROR;
        } while (match(state, ','));

        if (! match(state, ')')) {
            STATE_ERROR(state, PATHX_EDELIM);
            return;
        }
    }

    if (nargs != func->arity) {
        STATE_ERROR(state, PATHX_EDELIM);
        return;
    }

    if (ALLOC(expr) < 0) {
        STATE_ENOMEM;
        return;
    }
    expr->tag = E_APP;
    if (ALLOC_N(expr->args, nargs) < 0) {
        free_expr(expr);
        STATE_ENOMEM;
        return;
    }
    expr->func = func;
    for (int i = nargs - 1; i >= 0; i--)
        expr->args[i] = pop_expr(state);

    push_expr(expr, state);
}

/*
 * PrimaryExpr ::= Literal
 *               | Number
 *               | FunctionCall
 *
 */
static void parse_primary_expr(struct state *state) {
    if (peek(state, "'\"")) {
        parse_literal(state);
    } else if (peek(state, "0123456789")) {
        parse_number(state);
    } else {
        parse_function_call(state);
    }
}

static int looking_at_primary_expr(struct state *state) {
    const char *s = state->pos;
    /* Is it a Number or Literal ? */
    if (peek(state, "'\"0123456789"))
        return 1;

    /* Or maybe a function call, i.e. a word followed by a '(' ?
     * Note that our function names are only [a-zA-Z]+
     */
    while (*s != '\0' && isalpha(*s)) s++;
    while (*s != '\0' && isspace(*s)) s++;
    return *s == '(';
}

/*
 * PathExpr ::= LocationPath | PrimaryExpr
 *
 * The grammar is ambiguous here: the expression '42' can either be the
 * number 42 (a PrimaryExpr) or the RelativeLocationPath 'child::42'. The
 * reason for this ambiguity is that we allow node names like '42' in the
 * tree; rather than forbid them, we resolve the ambiguity by always
 * parsing '42' as a number, and requiring that the user write the
 * RelativeLocationPath in a different form, e.g. 'child::42' or './42'.
 */
static void parse_path_expr(struct state *state) {
    if (looking_at_primary_expr(state)) {
        parse_primary_expr(state);
    } else {
        parse_location_path(state);
    }
}

/*
 * MultiplicativeExpr ::= PathExpr ('*' PathExpr)*
 */
static void parse_multiplicative_expr(struct state *state) {
    parse_path_expr(state);
    CHECK_ERROR;
    while (match(state, '*')) {
        parse_path_expr(state);
        CHECK_ERROR;
        push_new_binary_op(OP_STAR, state);
    }
}

/*
 * AdditiveExpr ::= MultiplicativeExpr (AdditiveOp MultiplicativeExpr)*
 * AdditiveOp   ::= '+' | '-'
 */
static void parse_additive_expr(struct state *state) {
    parse_multiplicative_expr(state);
    CHECK_ERROR;
    while (*state->pos == '+' || *state->pos == '-') {
        enum binary_op op = (*state->pos == '+') ? OP_PLUS : OP_MINUS;
        state->pos += 1;
        skipws(state);
        parse_multiplicative_expr(state);
        CHECK_ERROR;
        push_new_binary_op(op, state);
    }
}

/*
 * EqualityExpr ::= AdditiveExpr (EqualityOp AdditiveExpr)?
 * EqualityOp ::= "=" | "!="
 */
static void parse_equality_expr(struct state *state) {
    parse_additive_expr(state);
    CHECK_ERROR;
    if (*state->pos == '=' ||
        (*state->pos == '!' && state->pos[1] == '=')) {
        enum binary_op op = (*state->pos == '=') ? OP_EQ : OP_NEQ;
        state->pos += (op == OP_EQ) ? 1 : 2;
        skipws(state);
        parse_additive_expr(state);
        CHECK_ERROR;
        push_new_binary_op(op, state);
    }
}

/*
 * Expr ::= EqualityExpr
 */
static void parse_expr(struct state *state) {
    skipws(state);
    parse_equality_expr(state);
}

int pathx_parse(const struct tree *tree, const char *txt,
                struct pathx **pathx) {
    struct state *state = NULL;

    *pathx = NULL;

    if (ALLOC(*pathx) < 0)
        return PATHX_ENOMEM;

    (*pathx)->origin = (struct tree *) tree;

    /* Set up state */
    if (ALLOC((*pathx)->state) < 0) {
        free_pathx(*pathx);
        *pathx = NULL;
        return PATHX_ENOMEM;
    }
    state = (*pathx)->state;

    state->errcode = PATHX_NOERROR;
    state->txt = txt;
    state->pos = txt;

    if (ALLOC_N(state->value_pool, 8) < 0) {
        STATE_ENOMEM;
        goto done;
    }
    state->value_pool_size = 8;
    state->value_pool[0].tag = T_BOOLEAN;
    state->value_pool[0].boolval = 0;
    state->value_pool[1].tag = T_BOOLEAN;
    state->value_pool[1].boolval = 1;
    state->value_pool_used = 2;

    /* Parse */
    parse_expr(state);
    if (HAS_ERROR(state))
        goto done;

    if (state->exprs_used != 1) {
        STATE_ERROR(state, PATHX_EINTERNAL);
        goto done;
    }

    /* Typecheck */
    check_expr(state->exprs[0], state);
    if (HAS_ERROR(state))
        goto done;

    if (state->exprs[0]->type != T_NODESET
        || state->exprs[0]->tag != E_LOCPATH) {
        STATE_ERROR(state, PATHX_ETYPE);
        goto done;
    }

    (*pathx)->locpath = state->exprs[0]->locpath;

 done:
    return state->errcode;
}

/*************************************************************************
 * Searching in the tree
 *************************************************************************/

static bool step_matches(struct step *step, struct tree *tree) {
    return (step->name == NULL || streqx(step->name, tree->label));
}

static struct tree *step_first(struct step *step, struct tree *ctx) {
    struct tree *node = NULL;
    switch (step->axis) {
    case SELF:
    case DESCENDANT_OR_SELF:
        node = ctx;
        break;
    case CHILD:
    case DESCENDANT:
        node = ctx->children;
        break;
    case PARENT:
    case ANCESTOR:
        node = ctx->parent;
        break;
    case ROOT:
        while (ctx->parent != ctx)
            ctx = ctx->parent;
        node = ctx;
        break;
    default:
        assert(0);
    }
    if (node == NULL)
        return NULL;
    if (step_matches(step, node))
        return node;
    return step_next(step, ctx, node);
}

static struct tree *step_next(struct step *step, struct tree *ctx,
                              struct tree *node) {
    while (node != NULL) {
        switch (step->axis) {
        case SELF:
            node = NULL;
            break;
        case CHILD:
            node = node->next;
            break;
        case DESCENDANT:
        case DESCENDANT_OR_SELF:
            if (node->children != NULL) {
                node = node->children;
            } else {
                while (node->next == NULL && node != ctx)
                    node = node->parent;
                if (node == ctx)
                    node = NULL;
                else
                    node = node->next;
            }
            break;
        case PARENT:
        case ROOT:
            node = NULL;
            break;
        case ANCESTOR:
            if (node->parent == node)
                node = NULL;
            else
                node = node->parent;
            break;
        default:
            assert(0);
        }
        if (node != NULL && step_matches(step, node))
            break;
    }
    return node;
}

struct tree *pathx_next(struct pathx *pathx) {
    if (pathx->node + 1 < pathx->nodeset->used)
        return pathx->nodeset->nodes[++pathx->node];
    return NULL;
}

/* Find the first node in TREE matching PATH. */
struct tree *pathx_first(struct pathx *pathx) {
    if (pathx->nodeset == NULL) {
        /* Evaluate */
        struct state *state = pathx->state;
        state->ctx = pathx->origin;
        state->ctx_pos = 1;
        state->ctx_len = 1;
        eval_expr(state->exprs[0], state);
        if (HAS_ERROR(state))
            return NULL;

        if (state->values_used != 1) {
            STATE_ERROR(state, PATHX_EINTERNAL);
            return NULL;
        }
        pathx->nodeset = pop_value(state)->nodeset;
    }
    pathx->node = 0;
    if (pathx->nodeset->used == 0)
        return NULL;
    else
        return pathx->nodeset->nodes[0];
}

/* Find a node in the tree that matches the longest prefix of PATH.
 *
 * Return 1 if a node was found that exactly matches PATH, 0 if an incomplete
 * prefix matches, and -1 if more than one node in the tree match.
 *
 * TMATCH is set to the tree node that matches, and SMATCH to the next step
 * after the one where TMATCH matched. If no node matches or multiple nodes
 * at the same depth match, TMATCH and SMATCH will be NULL. When exactly
 * one node matches, TMATCH will be that node, and SMATCH will be NULL.
 */
static int locpath_search(struct locpath *lp, struct state *state,
                          struct tree **tmatch, struct step **smatch) {
    struct nodeset **ns = NULL;
    uint maxns;
    int last;
    int result = -1;

    state->ctx = *tmatch;
    *tmatch = NULL;
    *smatch = NULL;

    ns_from_locpath(lp, &maxns, &ns, state);
    if (HAS_ERROR(state))
        goto done;

    for (last=maxns; last >= 0 && ns[last]->used == 0; last--);
    if (last < 0) {
        *smatch = lp->steps;
        result = 1;
        goto done;
    }
    if (ns[last]->used > 1) {
        result = -1;
        goto done;
    }
    result = 0;
    *tmatch = ns[last]->nodes[0];
    *smatch = lp->steps;
    for (int i=0; i < last; i++)
        *smatch = (*smatch)->next;
 done:
    for (int i=0; i <= maxns; i++)
        free_nodeset(ns[i]);
    FREE(ns);
    return result;
}

/* Expand the tree ROOT so that it contains all components of PATH. PATH
 * must have been initialized against ROOT by a call to PATH_FIND_ONE.
 *
 * Return the first segment that was created by this operation, or NULL on
 * error.
 */
int pathx_expand_tree(struct pathx *path, struct tree **tree) {
    int r;
    struct step *step;

    *tree = path->origin;
    r = locpath_search(path->locpath, path->state, tree, &step);
    if (r == -1)
        return -1;

    if (step == NULL)
        return 0;

    struct tree *first_child = NULL;
    struct tree *parent = *tree;

    if (parent == NULL)
        parent = path->origin;

    list_for_each(s, step) {
        if (s->name == NULL || s->axis != CHILD)
            goto error;
        struct tree *t = make_tree(strdup(s->name), NULL, parent, NULL);
        if (first_child == NULL)
            first_child = t;
        if (t == NULL || t->label == NULL)
            goto error;
        list_append(parent->children, t);
        parent = t;
    }

    while (first_child->children != NULL)
        first_child = first_child->children;

    *tree = first_child;
    return 0;

 error:
    if (first_child != NULL) {
        list_remove(first_child, first_child->parent->children);
        free_tree(first_child);
    }
    *tree = NULL;
    return -1;
}

int pathx_find_one(struct pathx *path, struct tree **tree) {
    *tree = pathx_first(path);
    if (*tree == NULL)
        return 0;

    if (pathx_next(path) != NULL) {
        *tree = NULL;
        return -1;
    }
    return 1;
}

const char *pathx_error(struct pathx *path, const char **txt, int *pos) {
    int errcode = PATHX_ENOMEM;

    if (path != NULL) {
        if (path->state->errcode < ARRAY_CARDINALITY(errcodes))
            errcode = path->state->errcode;
        else
            errcode = PATHX_EINTERNAL;
    }

    if (txt)
        *txt = path->state->txt;

    if (pos)
        *pos = path->state->pos - path->state->txt;

    return errcodes[errcode];
}


/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
