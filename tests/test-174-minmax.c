// exit: 128
int main() { int a[6] = {5, -3, 12, 0, 7, -8}; int mn = a[0], mx = a[0]; for (int i = 1; i < 6; i++) { if (a[i] < mn) mn = a[i]; if (a[i] > mx) mx = a[i]; } return mx * 10 - mn; }
