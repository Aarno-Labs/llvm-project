
static void sink_ptr(const int *p) { (void)p; }
  { sink_ptr((int[]){1}); }
int t32_after_marker = 42;
int main(void) {
  return 0;
}
