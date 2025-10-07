void hello(const char *str);
void world(int x, float y);
unsigned long xxx(int x, int y);
unsigned long injected(float f);
unsigned long no_yyy(int x, int y);
unsigned long zzz(int x, int y);
int first(int x);
int last(int x);

int main() {
  return injected(2.0f);
}
