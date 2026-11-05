# A project that uses an installed HotPP through find_package().
cmake_minimum_required(VERSION 3.25)
project(hotpp_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(HotPP 0.1 REQUIRED)
add_executable(consumer main.cpp game.cpp)
hotpp_enable(consumer)
