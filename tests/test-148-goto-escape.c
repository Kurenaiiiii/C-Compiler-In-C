// exit: 99
int main() { for (int i = 0; i < 10; i++) { for (int j = 0; j < 10; j++) { if (i * j > 20) goto out; } } out: return 99; }
