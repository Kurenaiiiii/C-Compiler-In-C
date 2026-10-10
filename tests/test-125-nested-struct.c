// exit: 12
struct I { int x; };
struct O { struct I in; int y; };
int main() { struct O o; o.in.x = 5; o.y = 7; return o.in.x + o.y; }
