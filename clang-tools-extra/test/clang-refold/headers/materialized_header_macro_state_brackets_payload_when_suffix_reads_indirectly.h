// Header for materialized_header_macro_state_brackets_payload_when_suffix_reads_indirectly.
//
// The edit lands inside `ID(4242)`, and `+ IND` after it on the same line
// reads `VAL` through `IND` without spelling it.
#define VAL 99
#define IND VAL
#define ID(x) (x)
int patched(void) { return ID(4242) + IND; }
