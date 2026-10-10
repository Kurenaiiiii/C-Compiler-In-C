// exit: 17
int main() { int a[4]; a[0] = 1; a[1] = 2; a[2] = 17; a[3] = 4; int *p = a + 3; return p[-1]; }
