/*
 * Options Visitor
 *
 * Copyright Red Hat, Inc. 2012-2016
 *
 * Author: Laszlo Ersek <lersek@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qapi/qmp/qerror.h"
#include "qapi/opts-visitor.h"
#include "qemu/queue.h"
#include "qemu/option_int.h"
#include "qapi/visitor-impl.h"


enum ListMode
{
    LM_NONE,             /* not traversing a list of repeated options */

    LM_IN_PROGRESS,      /* opts_next_list() ready to be called.
                          *
                          * Generating the next list link will consume the most
                          * recently parsed QemuOpt instance of the repeated
                          * option.
                          *
                          * Parsing a value into the list link will examine the
                          * next QemuOpt instance of the repeated option, and
                          * possibly enter LM_SIGNED_INTERVAL or
                          * LM_UNSIGNED_INTERVAL.
                          */

    LM_SIGNED_INTERVAL,  /* opts_next_list() has been called.
                          *
                          * Generating the next list link will consume the most
                          * recently stored element from the signed interval,
                          * parsed from the most recent QemuOpt instance of the
                          * repeated option. This may consume QemuOpt itself
                          * and return to LM_IN_PROGRESS.
                          *
                          * Parsing a value into the list link will store the
                          * next element of the signed interval.
                          */

    LM_UNSIGNED_INTERVAL /* Same as above, only for an unsigned interval. */
};

typedef enum ListMode ListMode;

struct OptsVisitor
{
    Visitor visitor;

    /* Ownership remains with opts_visitor_new()'s caller. */
    const QemuOpts *opts_root;

    unsigned depth;

    /* Non-null iff depth is positive. Each key is a QemuOpt name. Each value
     * is a non-empty GQueue, enumerating all QemuOpt occurrences with that
     * name. */
    GHashTable *unprocessed_opts;

    /* The list currently being traversed with opts_start_list() /
     * opts_next_list(). The list must have a struct element type in the
     * schema, with a single mandatory scalar member. */
    ListMode list_mode;
    GQueue *repeated_opts;
    char *repeated_name;

    /* When parsing a list of repeating options as integers, values of the form
     * "a-b", representing a closed interval, are allowed. Elements in the
     * range are generated individually.
     */
    union {
        int64_t s;
        uint64_t u;
    } range_next, range_limit;

    /* If "opts_root->id" is set, reinstantiate it as a fake QemuOpt for
     * uniformity. Only its "name" and "str" fields are set. "fake_id_opt" does
     * not survive or escape the OptsVisitor object.
     */
    QemuOpt *fake_id_opt;

    /* List of field names leading to the current structure. */
    GQueue *nested_names;
};


static OptsVisitor *to_ov(Visitor *v)
{
    return container_of(v, OptsVisitor, visitor);
}


static void
destroy_list(gpointer list)
{
  g_queue_free(list);
}


static void
opts_visitor_insert(GHashTable *unprocessed_opts, const QemuOpt *opt)
{
    GQueue *list;
    assert(opt);

    list = g_hash_table_lookup(unprocessed_opts, opt->name);
    if (list == NULL) {
        list = g_queue_new();

        /* GHashTable will never try to free the keys -- we supply NULL as
         * "key_destroy_func" in opts_start_struct(). Thus cast away key
         * const-ness in order to suppress gcc's warning.
         */
        g_hash_table_insert(unprocessed_opts, (gpointer)opt->name, list);
    }

    /* Similarly, destroy_list() doesn't call g_queue_free_full(). */
    g_queue_push_tail(list, (gpointer)opt);
}


static void
opts_start_struct(Visitor *v, const char *name, void **obj,
                  size_t size, Error **errp)
{
    OptsVisitor *ov = to_ov(v);
    const QemuOpt *opt;

    if (obj) {
        *obj = g_malloc0(size);
    }

    if (ov->nested_names != NULL) {
        g_queue_push_tail(ov->nested_names, (gpointer) name);
    }

    if (ov->depth++ > 0) {
        return;
    }

    ov->unprocessed_opts = g_hash_table_new_full(&g_str_hash, &g_str_equal,
                                                 NULL, &destroy_list);
    QTAILQ_FOREACH(opt, &ov->opts_root->head, next) {
        /* ensured by qemu-option.c::opts_do_parse() */
        assert(strcmp(opt->name, "id") != 0);

        opts_visitor_insert(ov->unprocessed_opts, opt);
    }

    if (ov->opts_root->id != NULL) {
        ov->fake_id_opt = g_malloc0(sizeof *ov->fake_id_opt);

        ov->fake_id_opt->name = g_strdup("id");
        ov->fake_id_opt->str = g_strdup(ov->opts_root->id);
        opts_visitor_insert(ov->unprocessed_opts, ov->fake_id_opt);
    }
}

static void
opts_start_alternate(Visitor *v, const char *name, GenericAlternate **obj,
                     size_t size, Error **errp)
{
    opts_start_struct(v, name, (void **)obj, size, errp);
}


static void
opts_check_struct(Visitor *v, Error **errp)
{
    OptsVisitor *ov = to_ov(v);
    GHashTableIter iter;
    GQueue *any;

    if (ov->nested_names != NULL) {
        g_queue_pop_tail(ov->nested_names);
    }

    if (ov->depth > 1) {
        return;
    }

    /* we should have processed all (distinct) QemuOpt instances */
    g_hash_table_iter_init(&iter, ov->unprocessed_opts);
    if (g_hash_table_iter_next(&iter, NULL, (void **)&any)) {
        const QemuOpt *first;

        first = g_queue_peek_head(any);
        error_setg(errp, QERR_INVALID_PARAMETER, first->name);
    }
}


static void
opts_end_struct(Visitor *v, void **obj)
{
    OptsVisitor *ov = to_ov(v);

    if (--ov->depth > 0) {
        return;
    }

    g_hash_table_destroy(ov->unprocessed_opts);
    ov->unprocessed_opts = NULL;
    if (ov->fake_id_opt) {
        g_free(ov->fake_id_opt->name);
        g_free(ov->fake_id_opt->str);
        g_free(ov->fake_id_opt);
    }
    ov->fake_id_opt = NULL;
}

static void
opts_end_alternate(Visitor *v, void **obj)
{
    opts_end_struct(v, obj);
}


static void
sum_strlen(gpointer data, gpointer user_data)
{
    const char *str = data;
    size_t *sum_len = user_data;

    if (str) { /* skip NULLs */
        *sum_len += strlen(str) + 1;
    }
}

static void
append_str(gpointer data, gpointer user_data)
{
    const char *str = data;
    char *concat_str = user_data;

    if (str) {
        strcat(concat_str, str);
        strcat(concat_str, ".");
    }
}

/* lookup a name, using a fully qualified version */
static GQueue *
lookup_distinct(const OptsVisitor *ov, const char *name, char **out_key,
                Error **errp)
{
    GQueue *list = NULL;
    char *key;

    if (ov->nested_names != NULL) {
        size_t sum_len = strlen(name);
        g_queue_foreach(ov->nested_names, sum_strlen, &sum_len);
        key = g_malloc(sum_len + 1);
        key[0] = 0;
        g_queue_foreach(ov->nested_names, append_str, key);
        strcat(key, name);
    } else {
        key = g_strdup(name);
    }

    list = g_hash_table_lookup(ov->unprocessed_opts, key);
    if (list && out_key) {
        *out_key = key;
        key = NULL;
    }

    if (!list) {
        error_setg(errp, QERR_MISSING_PARAMETER, name);
    }

    g_free(key);
    return list;
}


static void
opts_start_list(Visitor *v, const char *name, GenericList **list, size_t size,
                Error **errp)
{
    OptsVisitor *ov = to_ov(v);

    /* we can't traverse a list in a list */
    assert(ov->list_mode == LM_NONE);
    /* we don't support visits without a list */
    assert(list);
    assert(ov->repeated_opts == NULL && ov->repeated_name == NULL);
    ov->repeated_opts = lookup_distinct(ov, name, &ov->repeated_name, errp);
    if (ov->repeated_opts) {
        ov->list_mode = LM_IN_PROGRESS;
        *list = g_malloc0(size);
    } else {
        *list = NULL;
    }
}


static GenericList *
opts_next_list(Visitor *v, GenericList *tail, size_t size)
{
    OptsVisitor *ov = to_ov(v);

    switch (ov->list_mode) {
    case LM_SIGNED_INTERVAL:
    case LM_UNSIGNED_INTERVAL:
        if (ov->list_mode == LM_SIGNED_INTERVAL) {
            if (ov->range_next.s < ov->range_limit.s) {
                ++ov->range_next.s;
                break;
            }
        } else if (ov->range_next.u < ov->range_limit.u) {
            ++ov->range_next.u;
            break;
        }
        ov->list_mode = LM_IN_PROGRESS;
        /* range has been completed, fall through in order to pop option */

    case LM_IN_PROGRESS: {
        g_queue_pop_head(ov->repeated_opts);
        if (g_queue_is_empty(ov->repeated_opts)) {
            g_hash_table_remove(ov->unprocessed_opts, ov->repeated_name);
            return NULL;
        }
        break;
    }

    default:
        abort();
    }

    tail->next = g_malloc0(size);
    return tail->next;
}


static void
opts_check_list(Visitor *v, Error **errp)
{
    /*
     * Unvisited list elements will be reported later when checking
     * whether unvisited struct members remain.
     */
}


static void
opts_end_list(Visitor *v, void **obj)
{
    OptsVisitor *ov = to_ov(v);

    assert(ov->list_mode == LM_IN_PROGRESS ||
           ov->list_mode == LM_SIGNED_INTERVAL ||
           ov->list_mode == LM_UNSIGNED_INTERVAL);
    ov->repeated_opts = NULL;

    g_free(ov->repeated_name);
    ov->repeated_name = NULL;

    ov->list_mode = LM_NONE;
}


static const QemuOpt *
lookup_scalar(const OptsVisitor *ov, const char *name, char** out_key,
              Error **errp)
{
    if (ov->list_mode == LM_NONE) {
        GQueue *list;

        /* the last occurrence of any QemuOpt takes effect when queried by name
         */
        list = lookup_distinct(ov, name, out_key, errp);
        return list ? g_queue_peek_tail(list) : NULL;
    }
    assert(ov->list_mode == LM_IN_PROGRESS);
    assert(out_key == NULL || *out_key == NULL);
    return g_queue_peek_head(ov->repeated_opts);
}


static void
processed(OptsVisitor *ov, const char *name)
{
    if (ov->list_mode == LM_NONE) {
        g_hash_table_remove(ov->unprocessed_opts, name);
        return;
    }
    assert(ov->list_mode == LM_IN_PROGRESS);
    /* do nothing */
}


static void
opts_type_str(Visitor *v, const char *name, char **obj, Error **errp)
{
    OptsVisitor *ov = to_ov(v);
    const QemuOpt *opt;
    char *key = NULL;

    opt = lookup_scalar(ov, name, &key, errp);
    if (!opt) {
        *obj = NULL;
        return;
    }
    *obj = g_strdup(opt->str ? opt->str : "");
    /* Note that we consume a string even if this is called as part of
     * an enum visit that later fails because the string is not a
     * valid enum value; this is harmless because tracking what gets
     * consumed only matters to visit_end_struct() as the final error
     * check if there were no other failures during the visit.  */
    processed(ov, key);
    g_free(key);
}


/* mimics qemu-option.c::parse_option_bool() */
static void
opts_type_bool(Visitor *v, const char *name, bool *obj, Error **errp)
{
    OptsVisitor *ov = to_ov(v);
    const QemuOpt *opt;
    char *key = NULL;

    opt = lookup_scalar(ov, name, &key, errp);
    if (!opt) {
        return;
    }

    if (opt->str) {
        if (strcmp(opt->str, "on") == 0 ||
            strcmp(opt->str, "yes") == 0 ||
            strcmp(opt->str, "y") == 0) {
            *obj = true;
        } else if (strcmp(opt->str, "off") == 0 ||
            strcmp(opt->str, "no") == 0 ||
            strcmp(opt->str, "n") == 0) {
            *obj = false;
        } else {
            error_setg(errp, QERR_INVALID_PARAMETER_VALUE, opt->name,
                       "on|yes|y|off|no|n");
            g_free(key);
            return;
        }
    } else {
        *obj = true;
    }

    processed(ov, key);
    g_free(key);
}


static void
opts_type_int64(Visitor *v, const char *name, int64_t *obj, Error **errp)
{
    OptsVisitor *ov = to_ov(v);
    const QemuOpt *opt;
    const char *str;
    long long val;
    char *endptr;
    char *key = NULL;

    if (ov->list_mode == LM_SIGNED_INTERVAL) {
        *obj = ov->range_next.s;
        return;
    }

    opt = lookup_scalar(ov, name, &key, errp);
    if (!opt) {
        return;
    }
    str = opt->str ? opt->str : "";

    /* we've gotten past lookup_scalar() */
    assert(ov->list_mode == LM_NONE || ov->list_mode == LM_IN_PROGRESS);

    errno = 0;
    val = strtoll(str, &endptr, 0);
    if (errno == 0 && endptr > str && INT64_MIN <= val && val <= INT64_MAX) {
        if (*endptr == '\0') {
            *obj = val;
            processed(ov, key);
            g_free(key);
            return;
        }
        if (*endptr == '-' && ov->list_mode == LM_IN_PROGRESS) {
            long long val2;
            assert(key == NULL);

            str = endptr + 1;
            val2 = strtoll(str, &endptr, 0);
            if (errno == 0 && endptr > str && *endptr == '\0' &&
                INT64_MIN <= val2 && val2 <= INT64_MAX && val <= val2 &&
                (val > INT64_MAX - OPTS_VISITOR_RANGE_MAX ||
                 val2 < val + OPTS_VISITOR_RANGE_MAX)) {
                ov->range_next.s = val;
                ov->range_limit.s = val2;
                ov->list_mode = LM_SIGNED_INTERVAL;

                /* as if entering on the top */
                *obj = ov->range_next.s;
                return;
            }
        }
    }
    error_setg(errp, QERR_INVALID_PARAMETER_VALUE, opt->name,
               (ov->list_mode == LM_NONE) ? "an int64 value" :
                                            "an int64 value or range");
    g_free(key);
}


static void
opts_type_uint64(Visitor *v, const char *name, uint64_t *obj, Error **errp)
{
    OptsVisitor *ov = to_ov(v);
    const QemuOpt *opt;
    const char *str;
    unsigned long long val;
    char *endptr;
    char *key = NULL;

    if (ov->list_mode == LM_UNSIGNED_INTERVAL) {
        *obj = ov->range_next.u;
        return;
    }

    opt = lookup_scalar(ov, name, &key, errp);
    if (!opt) {
        return;
    }
    str = opt->str;

    /* we've gotten past lookup_scalar() */
    assert(ov->list_mode == LM_NONE || ov->list_mode == LM_IN_PROGRESS);

    if (parse_uint(str, &val, &endptr, 0) == 0 && val <= UINT64_MAX) {
        if (*endptr == '\0') {
            *obj = val;
            processed(ov, key);
            g_free(key);
            return;
        }
        if (*endptr == '-' && ov->list_mode == LM_IN_PROGRESS) {
            unsigned long long val2;
            assert(key == NULL);

            str = endptr + 1;
            if (parse_uint_full(str, &val2, 0) == 0 &&
                val2 <= UINT64_MAX && val <= val2 &&
                val2 - val < OPTS_VISITOR_RANGE_MAX) {
                ov->range_next.u = val;
                ov->range_limit.u = val2;
                ov->list_mode = LM_UNSIGNED_INTERVAL;

                /* as if entering on the top */
                *obj = ov->range_next.u;
                return;
            }
        }
    }
    error_setg(errp, QERR_INVALID_PARAMETER_VALUE, opt->name,
               (ov->list_mode == LM_NONE) ? "a uint64 value" :
                                            "a uint64 value or range");
    g_free(key);
}


static void
opts_type_size(Visitor *v, const char *name, uint64_t *obj, Error **errp)
{
    OptsVisitor *ov = to_ov(v);
    const QemuOpt *opt;
    int err;
    char *key = NULL;

    opt = lookup_scalar(ov, name, &key, errp);
    if (!opt) {
        return;
    }

    err = qemu_strtosz(opt->str ? opt->str : "", NULL, obj);
    if (err < 0) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, opt->name,
                   "a size value");
        g_free(key);
        return;
    }

    processed(ov, key);
    g_free(key);
}


static void
opts_optional(Visitor *v, const char *name, bool *present)
{
    OptsVisitor *ov = to_ov(v);

    /* we only support a single mandatory scalar field in a list node */
    assert(ov->list_mode == LM_NONE);
    *present = (lookup_distinct(ov, name, NULL, NULL) != NULL);
}


static void
opts_free(Visitor *v)
{
    OptsVisitor *ov = to_ov(v);

    if (ov->unprocessed_opts != NULL) {
        g_hash_table_destroy(ov->unprocessed_opts);
    }
    g_free(ov->fake_id_opt);
    g_free(ov);
}


Visitor *
opts_visitor_new(const QemuOpts *opts, bool nested)
{
    OptsVisitor *ov;

    assert(opts);
    ov = g_malloc0(sizeof *ov);

    ov->visitor.type = VISITOR_INPUT;

    if (nested) {
        ov->nested_names = g_queue_new();
    } else {
        ov->nested_names = NULL;
    }

    ov->visitor.start_struct = &opts_start_struct;
    ov->visitor.check_struct = &opts_check_struct;
    ov->visitor.end_struct   = &opts_end_struct;

    ov->visitor.start_alternate = &opts_start_alternate;
    ov->visitor.end_alternate = &opts_end_alternate;

    ov->visitor.start_list = &opts_start_list;
    ov->visitor.next_list  = &opts_next_list;
    ov->visitor.check_list = &opts_check_list;
    ov->visitor.end_list   = &opts_end_list;

    ov->visitor.type_int64  = &opts_type_int64;
    ov->visitor.type_uint64 = &opts_type_uint64;
    ov->visitor.type_size   = &opts_type_size;
    ov->visitor.type_bool   = &opts_type_bool;
    ov->visitor.type_str    = &opts_type_str;

    /* type_number() is not filled in, but this is not the first visitor to
     * skip some mandatory methods... */

    ov->visitor.optional = &opts_optional;
    ov->visitor.free = opts_free;

    ov->opts_root = opts;

    return &ov->visitor;
}
