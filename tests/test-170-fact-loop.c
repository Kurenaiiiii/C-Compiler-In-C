int printf(char *fmt, ...);
int main() { int f = 1; for (int i = 1; i <= 5; i++) { f *= i; printf("%d\n", f); } return 0; }
