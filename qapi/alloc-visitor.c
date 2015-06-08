#include "qapi/alloc-visitor.h"
#include "qemu-common.h"
#include "qapi/visitor-impl.h"

struct AllocVisitor {
    Visitor visitor;
};

static void alloc_start_struct(Visitor *v, void **obj, const char* kind,
                               const char *name, size_t size, Error **errp)
{
    if (obj) {
        *obj = g_malloc0(size);
    }
}

static void alloc_end_struct(Visitor *v, Error **errp)
{
}

static void alloc_start_implicit_struct(Visitor *v, void **obj, size_t size,
                                        Error **errp)
{
    if (obj) {
        *obj = g_malloc0(size);
    }
}

static void alloc_end_implicit_struct(Visitor *v, Error **errp)
{
}

static void alloc_type_enum(Visitor *v, int *obj, const char *strings[],
                            const char *kind, const char *name, Error **errp)
{
    assert(*strings); /* there is at least one valid enum value... */
    *obj = 0;
}

AllocVisitor *alloc_visitor_new(void)
{
    AllocVisitor *v = g_malloc0(sizeof(AllocVisitor));

    v->visitor.start_struct = alloc_start_struct;
    v->visitor.end_struct = alloc_end_struct;
    v->visitor.start_implicit_struct = alloc_start_implicit_struct;
    v->visitor.end_implicit_struct = alloc_end_implicit_struct;

    v->visitor.type_enum = alloc_type_enum;

    return v;
}

void alloc_visitor_cleanup(AllocVisitor *v)
{
    g_free(v);
}

Visitor *alloc_visitor_get_visitor(AllocVisitor *v)
{
    return &v->visitor;
}
