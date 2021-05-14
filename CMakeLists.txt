cmake_minimum_required(VERSION 3.16)

project(paramount VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost
    REQUIRED COMPONENTS filesystem program_options system thread)
find_package(fmt)

add_compile_options(-Wall -Werror)

set(programs
    paramount
)

add_executable(paramount
    tempdir.hpp
    paramount.cpp
)

target_link_libraries(paramount
    Boost::filesystem
    Boost::program_options
    Boost::system
    Boost::thread
    fmt::fmt
)
