
struct T { int* pa; int b; int* px; int y; };
int ig;
struct T g = { .pa = &g.b, .y = 4, .b = 3, .px = &g.b };
int first(int x);
int last(int x);
int main() {
  *(g.pa) = 32;
  *(g.px) += 10;
  printf("%d\n", *(g.pa));
  return 0;
}
