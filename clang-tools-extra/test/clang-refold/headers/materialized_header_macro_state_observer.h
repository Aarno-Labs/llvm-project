// Header for materialized_header_macro_state_rejects_carry_across_observer.c.
//
// `VAL` is defined first and observed by `crosser` before the invocation the
// edit lands on.  Repairing that invocation requires carrying `#define VAL`
// past `crosser`, which observes it -- the conflict under test.
#define VAL 99
int crosser(void) { return VAL; }
#define ID(x) (x)
int patched(void) { return ID(4242); }
