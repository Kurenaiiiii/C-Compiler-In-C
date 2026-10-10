// exit: 3
#define A 1
#define B 1
int main() {
#ifdef A
#ifdef B
  return 3;
#else
  return 2;
#endif
#else
  return 1;
#endif
}
