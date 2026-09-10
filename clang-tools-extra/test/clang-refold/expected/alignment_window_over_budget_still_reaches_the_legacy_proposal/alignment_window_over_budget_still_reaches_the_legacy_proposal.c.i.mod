struct XjGlobals;

void push(struct XjGlobals *xjg, char c);
void pop(struct XjGlobals *xjg);
char top(struct XjGlobals *xjg);
char empty(struct XjGlobals *xjg);
int size(struct XjGlobals *xjg);

void *first(void)
{
  return ((void*)0);
}
void *first_w(void *x, void *s) { return first_impl(x, s); }
void *first_v(void *x, void *s) { return first_impl(x, s); }
void *first_u(void *x, void *s) { return first_impl(x, s); }
void *first_t(void *x, void *s) { return first_impl(x, s); }
void *first_s(void *x, void *s) { return first_impl(x, s); }

static int second(int a, int b);
int tail_0 = 6000;
int tail_1 = 6001;
int tail_2 = 6002;
int tail_3 = 6003;
