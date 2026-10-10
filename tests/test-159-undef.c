// exit: 2
#define X 1
#undef X
int main() {
#ifdef X
  return 1;
#else
  return 2;
#endif
}
