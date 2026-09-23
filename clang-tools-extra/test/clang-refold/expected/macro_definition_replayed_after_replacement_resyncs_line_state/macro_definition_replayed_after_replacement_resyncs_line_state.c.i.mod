static int lead(int *ctx, int x);
const char *get_grammar(int *ctx)
{
 return "g";
}
struct fields { int alpha; int beta; int gamma; };
int line_after(void) { return 34; }
static int lead(int x) { return x; }
