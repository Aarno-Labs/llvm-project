static int before = 1;
static int after = 2;
static const char *f_before = "after"; static int v_before = after;
int main(void) { return v_before != 2; }
