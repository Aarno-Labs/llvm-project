
static const int hdr82_val = 820;
int INS82_BOUNDARY = hdr82_val + 1;
int TU82_START = 82;
int INS82_LATE = TU82_START + 100;
int TU82_NEXT = 182;
int main(){ return hdr82_val + TU82_START + TU82_NEXT; }
