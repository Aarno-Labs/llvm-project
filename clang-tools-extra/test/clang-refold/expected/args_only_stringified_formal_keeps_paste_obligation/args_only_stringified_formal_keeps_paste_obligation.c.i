static int before = 1;
static int after = 2;
static const char *f_before = "before"; static int v_before = before;
int main(void) { return v_before != 2; }
