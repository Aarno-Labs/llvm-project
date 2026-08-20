// Header for the macro-state repair that has no single answer.
//
// `VAL` is observed by `crosser` before the invocation the edit lands on,
// so the definition cannot be carried past the replacement -- and the
// patched line itself also reads `VAL`, so it cannot be undefined across
// either.
#define VAL 99
int crosser(void) { return VAL; }
#define ID(x) (x)
int patched(void) { return ID(4242) + VAL; }
