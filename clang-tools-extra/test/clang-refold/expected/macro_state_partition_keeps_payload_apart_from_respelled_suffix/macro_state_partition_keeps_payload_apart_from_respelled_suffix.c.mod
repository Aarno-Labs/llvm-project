// RUN: %clang-refold-tester-verify-off macro_state_partition_keeps_payload_apart_from_respelled_suffix
// Regression: B replaces `PTR(s)`, which expands to `s *`, with the painted
// `M`.  The whole cover's text `M` meets the preserved `t` with no white
// space, and the partition that brackets the line with `#undef M` and its
// restore re-spells the rest of the line beside the payload, so the join sat
// inside the widened edit and read `int f(Mt);`.  A repair that re-spells
// source beside an edit's text now keeps that join a token boundary too.
typedef struct s { int v; } s;
typedef long M;
#define M ((M)0)
#define PTR(T) T *
long m0 = M;
#undef M
int f(M t);
#define M ((M)0)
long m1 = M;
