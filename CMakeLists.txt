cmake_minimum_required(VERSION 3.20)
project(ITCHEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Target optimizations specifically tuned for AMD Zen 3 (Ryzen 5 5625U) on Linux
add_compile_options(-O3 -march=znver3 -flto -Wall -Wextra -ffast-math)

include_directories(include)

add_executable(ITCH-PARSER src/main.cpp)