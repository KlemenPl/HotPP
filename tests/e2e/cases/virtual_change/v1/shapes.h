#pragma once
struct Shape {
    virtual ~Shape();
    virtual int area() const = 0;
};
struct Square : Shape {
    explicit Square(int s) : side(s) {}
    int area() const override;
    int side;
};
Shape* make_square(int side);
int is_square(const Shape* s);
