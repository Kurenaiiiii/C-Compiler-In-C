// exit: 83
void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
int main() { int x = 3, y = 8; swap(&x, &y); return x * 10 + y; }
