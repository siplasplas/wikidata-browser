cmake_minimum_required(VERSION 3.24)
project(wikidata-browser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# wdlist: lists the parts of Wikidata dumps of a directory (<date>.lst)
add_executable(wdlist tools/wdlist.cpp)
