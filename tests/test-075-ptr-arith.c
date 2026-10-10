// exit: 30
int main() { int a[5]; for (int i = 0; i < 5; i++) a[i] = i * 10; int *p = a; return *(p + 3); }
