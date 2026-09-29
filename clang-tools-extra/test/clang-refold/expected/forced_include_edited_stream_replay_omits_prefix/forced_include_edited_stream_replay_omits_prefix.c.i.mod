extern int prelude_reset(void);
extern int prelude_wreset(int);
int counter = 2;
int main(void) { return prelude_wreset(0) + counter; }
