#include "shapes.h"
Shape::~Shape() = default;
int Square::area() const { return side * side; }
Shape* make_square(int side) { return new Square(side); }
int is_square(const Shape* s) { return dynamic_cast<const Square*>(s) ? 1 : 0; }
