// Header for the include-owned macro-state repairs.
//
// `VAL` is defined first and observed by `crosser` before the invocation the
// edit lands on, so `#define VAL` cannot be carried past the replacement.  The
// repair undefines `VAL` around the replacement's line and restores it after.
#define VAL 99
int crosser(void) { return VAL; }
#define ID(x) (x)
int patched(void) { return ID(4242); }
