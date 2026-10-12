// Header for materialized_header_macro_state_undef_instead_of_carry_across_indirect_read.
//
// `crosser` lies between `#define VAL` and the edited invocation and reads
// `VAL` through `IND` without spelling it.
#define IND VAL
#define ID(x) (x)
#define VAL 99
int crosser(void) { return IND; }
int patched(void) { return ID(4242); }
