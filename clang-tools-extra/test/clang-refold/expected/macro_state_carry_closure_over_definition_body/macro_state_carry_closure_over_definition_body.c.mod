// RUN: %clang-refold-tester macro_state_carry_closure_over_definition_body

// A definition carried past a materialized include must bring the definitions
// its own replacement list names.  OUTER is carried because the surviving
// `OUTER(3)` call names it, but nothing names INNER directly: every INNER
// invocation is generated while expanding OUTER.  Carrying OUTER alone emits a
// definition that no longer expands, and `INNER` survives into the output as a
// bare identifier.
int untouched = 1;

#define INNER(v) ((v) + 1)
#define OUTER(v) INNER(v)
int before = 10, after = 20;


int use = OUTER(3);
