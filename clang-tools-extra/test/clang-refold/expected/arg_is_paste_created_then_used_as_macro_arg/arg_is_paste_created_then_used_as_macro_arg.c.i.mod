int pre_long(void) { return 31; } int call_XCAT(pre_,long)(void) { return pre_long(); }
int main(void) {
  return call_pre_long() == 31 ? 0 : 1;
}
