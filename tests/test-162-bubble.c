// exit: 55
int main() { int a[10] = {9, 3, 7, 1, 8, 2, 6, 4, 5, 0}; for (int i = 0; i < 10; i++) for (int j = 0; j < 9 - i; j++) if (a[j] > a[j + 1]) { int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t; } int ok = 1; for (int i = 0; i < 10; i++) if (a[i] != i) ok = 0; return ok ? 55 : 0; }
