// RUN: %clang-refold-tester-with-lines owner_state_graph_censused_once_per_run
// RUN: FileCheck --input-file=%t/outputs/owner_state_graph_censused_once_per_run.out %s
//
// Regression: the owner-state graph must be censused once per run, not once
// per pass.
//
// An insertion ahead of a line observer moves that observer, so the run must
// repair line state and ask which owners observe it.  That query consumes the
// owner-state graph, and the repair ladder plans the unit more than once, so
// the census is reached from several engines in one run.
//
// The census reads the producer model and the A token stream and nothing
// else.  Both are constants of the run: an attempt narrows which owners must
// expand and which anchors it plans from, and a candidate simulation is handed
// a different alignment, but neither rewrites the producer facts or the A
// stream.  So every later engine presents the identical facts and must replay
// the recorded census instead of re-deriving it.
//
// Rebuilding it is not a small waste.  The census attaches a canonical state
// summary to every recorded owner, so its cost scales with the owner count
// rather than with the edit.  On a corpus unit whose ambiguous windows
// enumerate many candidate maps it was the single largest cost in a pass, paid
// once per enumerated map: one 43k-token unit spent 42% of a thirteen-minute
// run re-deriving one identical graph 89 times.
//
// One engine builds the census, a later one replays it, and no engine builds
// it a second time.
//
// CHECK: owner-state graph: nodes=
// CHECK: owner-state graph: replaying
// CHECK-NOT: owner-state graph: nodes=
#line 80 "virtual_unit.c"
int base = 1;
int observed = __LINE__;
const char *unit = __FILE__;
