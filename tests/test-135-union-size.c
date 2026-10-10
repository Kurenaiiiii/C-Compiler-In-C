// exit: 8
union U { char c; int i; long l; };
int main() { return (int)sizeof(union U); }
