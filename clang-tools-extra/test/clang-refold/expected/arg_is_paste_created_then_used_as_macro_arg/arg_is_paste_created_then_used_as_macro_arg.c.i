int pre_int(void) { return 31; } int call_XCAT(pre_,int)(void) { return pre_int(); }
int main(void) {
  return call_pre_int() == 31 ? 0 : 1;
}
