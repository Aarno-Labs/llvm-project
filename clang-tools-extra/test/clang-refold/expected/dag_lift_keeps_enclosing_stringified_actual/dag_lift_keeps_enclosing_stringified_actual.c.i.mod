static int before(int x) { return x; }
static int after(int x) { return x + 1; }
int main(void) { int v = after(1); const char *s = "ENTRY(before)"; (void)s; return v; }
