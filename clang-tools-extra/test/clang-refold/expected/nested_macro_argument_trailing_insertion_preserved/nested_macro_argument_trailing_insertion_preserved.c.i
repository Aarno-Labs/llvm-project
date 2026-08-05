short f(short *x, short *cdbk, int i, int j)
{
   short tmp;
   tmp = ((x[j])-(((short)*cdbk++)));
   return tmp;
}
