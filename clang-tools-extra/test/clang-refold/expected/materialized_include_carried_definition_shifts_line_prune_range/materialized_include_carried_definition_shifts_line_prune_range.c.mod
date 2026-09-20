// RUN: %clang-refold-tester-with-lines materialized_include_carried_definition_shifts_line_prune_range

// Folding the initializer consumes the `#define LINE_PRUNE_EOF` that sits
// between its tokens, so the surviving use below forces that definition to be
// carried out in front of the materialized include body.  The body already
// carries line-control pruning candidates at its own byte offsets; prepending
// the definition moves every one of those bytes right, and a candidate that is
// not moved with them names the bytes just before its directive.  The pruner
// deletes exactly what a candidate names, so the miss splices the guard's
// `#define` into the tail of the entry directive's filename -- damage the
// closing token check cannot see, because a malformed macro that is never
// expanded contributes no token to either stream.
#define LINE_PRUNE_EOF (-1)
#ifndef CARRIED_DEFINITION_LINE_PRUNE_OUTER_H
#define CARRIED_DEFINITION_LINE_PRUNE_OUTER_H
int value = 3;
#endif
int result = LINE_PRUNE_EOF;
