static int before(int x) { return x; }
static int after(int x) { return x + 1; }
static int f_before(int x) { return before(x); }
int main(void) { return f_before(1) != 2; }
