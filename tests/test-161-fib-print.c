int printf(char *fmt, ...);
int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
int main() { for (int i = 0; i < 8; i++) printf("%d ", fib(i)); printf("\n"); return 0; }
