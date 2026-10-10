// exit: 6
int main() { int a[3]; a[0] = 1; a[1] = 2; a[2] = 3; int *p = a; int *q = a + 3; int n = 0; while (p < q) { n += *p; p++; } return n; }
