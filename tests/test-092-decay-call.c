// exit: 26
int sum(int *p, int n) { int s = 0; for (int i = 0; i < n; i++) s += p[i]; return s; }
int main() { int a[4] = {5, 6, 7, 8}; return sum(a, 4); }
