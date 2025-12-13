cmake_minimum_required(VERSION 3.10)

# Nazwa projektu (musi być PO cmake_minimum_required)
project(SO2 VERSION 1.0)

# Standard C++
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Znajdowanie biblioteki Ncurses
find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIRS})

# Definicja pliku wykonywalnego o nazwie "SO2"
add_executable(SO2 main.cpp)

# Linkowanie bibliotek do celu "SO2"
target_link_libraries(SO2 ${CURSES_LIBRARIES})
target_link_libraries(SO2 pthread)