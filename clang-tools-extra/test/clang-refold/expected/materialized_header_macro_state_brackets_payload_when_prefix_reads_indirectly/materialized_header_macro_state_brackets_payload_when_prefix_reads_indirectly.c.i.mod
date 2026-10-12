enum { VAL = 3 };
int patched(void) { return 99 + (VAL); }
int tu(void) { return 0; }
