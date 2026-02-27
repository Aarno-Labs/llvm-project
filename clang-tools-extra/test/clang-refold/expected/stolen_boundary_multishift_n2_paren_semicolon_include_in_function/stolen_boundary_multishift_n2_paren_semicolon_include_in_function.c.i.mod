static inline void ms_call_unique(int x) { (void)x; }
static inline void other_call_unique(int x) { (void)x; }
static inline void ms_outer(void) {
  int ms_unique_arg_123 = 7;
  ms_call_unique(ms_unique_arg_123);
  other_call_unique((0)); /* pure insertion at before boundary for ms_child_begin_marker */
int ms_child_begin_marker = 42;

  other_call_unique(ms_unique_arg_123);
}

int main(void) {
  ms_outer();
  return 0;
}
