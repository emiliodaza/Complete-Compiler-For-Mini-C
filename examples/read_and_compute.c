extern void print(int);
extern int read();

int calculate() {
    int a;
    int b;
    a = read();
    b = read();
    print(a + b);
    print(a - b);
    print(a * b);
    print(a / b);
    return a + b;
}
