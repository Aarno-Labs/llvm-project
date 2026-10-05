static int before(int x) { return x; }
static int after(int x) { return x + 1; }
int main(void) { int v = before(1); const char *s = "before(1)"; (void)s; return v; }
