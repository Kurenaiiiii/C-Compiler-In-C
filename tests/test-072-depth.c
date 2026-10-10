// exit: 50
int down(int n) { if (n == 0) return 0; return 1 + down(n - 1); }
int main() { return down(50); }
