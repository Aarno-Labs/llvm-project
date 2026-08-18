#define VAL 99
int crosser(void) { return VAL; }
int before_line = __LINE__;
#define ID(x) (x)
int patched(void) { return ID(4242); }
int after_line = __LINE__;
