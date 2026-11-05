#pragma once
struct Oops {
    int code;
};
int risky(int x);
int pass_through(int x);
int guarded(int x);
int callback(int x); // in main.cpp: throws Oops{x} for negative x
