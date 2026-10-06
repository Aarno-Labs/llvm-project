// RUN: %clang-refold-tester materialized_header_commented_child_include_keeps_definition

// The declaration edit materializes the outer header, and its child include
// survives in the payload as a directive.  That directive carries a trailing
// comment, so the producer's synthesized `#include <...>` text is not the line
// the payload contains.  Matching on it read the child as inlined, treated the
// FLAG definition it supplies as consumed by the materialization, and tried to
// carry that definition in front of a payload whose `#ifndef FLAG` observes
// it, which refused the whole refold.

#include <commented_child_include_inner.h>	/* defines FLAG */
#ifndef FLAG
#define FLAG 0
#endif
int outer_fn(int *ctx, int a);

int use = FLAG;
