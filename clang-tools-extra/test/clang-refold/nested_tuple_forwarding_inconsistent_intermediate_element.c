// RUN: %clang-refold-tester nested_tuple_forwarding_inconsistent_intermediate_element
// Fail-closed regression: `x` is one root tuple element that `ADD`'s own
// replacement list substitutes twice, so the two occurrences cannot be edited
// independently.  The edited stream changes only the first occurrence, which no
// tuple-element edit can express.  The recursive tuple theorem must refuse and
// leave the sound expansion fallback in place rather than realize one
// occurrence and silently claim the whole root envelope.
#define EXPR(f,t) f t
#define ADD(g,x,t) ((x)+(x)+(g t))
#define SUB(x,y) ((x)-(y))

int x = 5;
int y = 2;
int z = 4;
int res = EXPR(ADD, (SUB, x, (y, z)));
