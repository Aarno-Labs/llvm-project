double acos(double);
static double wrap_error_r0(double x) { return x; }
static double f_acos(double x) { return wrap_error_r0(acos(x)); }
struct entry { double (*fptr)(double); const char *name; };
static const struct entry table[] = {
{f_acos, "acos"},

};
