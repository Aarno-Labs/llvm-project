void hello(const char *str);
void world(int x, float y);
int xxx(int x, int y);
int injected(float f);
int no_yyy(int x, int y);
int zzz(int x, int y);
int first(int x);
int last(int x);

int main() {
  return injected(2.0f);
}
