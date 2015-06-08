/*
 * Alloc Visitor.
 * Recursively allocates structs, leaving all optional fields unset. In case of
 * a non-optional field it fails.
 */

#ifndef ALLOC_VISITOR_H
#define ALLOC_VISITOR_H

#include "qapi/visitor.h"

typedef struct AllocVisitor AllocVisitor;

AllocVisitor *alloc_visitor_new(void);
void alloc_visitor_cleanup(AllocVisitor *v);
Visitor *alloc_visitor_get_visitor(AllocVisitor *v);

#endif
