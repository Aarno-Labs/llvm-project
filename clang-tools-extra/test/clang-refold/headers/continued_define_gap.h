int before = 1;
#define TIMES_SUM(a, b)					\
	(((a) + (b)) *					\
	 ((a) + (b)))
int after = 2;
int product = TIMES_SUM(2, 3);
