// RUN: python3 -c "import os,time; ts='Mon Jan  1 22:04:05 2001'; e=time.mktime(time.strptime(ts, '%%a %%b %%d %%H:%%M:%%S %%Y')); [os.utime(p, (e, e)) for p in [r'%s', r'%S/headers/parent7.h']]"
// RUN: %clang-refold-tester-clang-flags materialized_header_timestamp_observer -- -I %S
int p = 3;
const char *v = "Thu Jan 01 00:00:00 1970";
int q = 2;
int main(void) { return p + q; }
