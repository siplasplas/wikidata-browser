cmake_minimum_required(VERSION 3.24)
project(wikidata-browser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# wdlist: lists the parts of Wikidata dumps of a directory (<date>.lst)
add_executable(wdlist tools/wdlist.cpp)

find_package(BZip2 REQUIRED)
find_package(nlohmann_json REQUIRED)

# wdtriples: (property, entity, target) triples of entity links of a dump part
add_executable(wdtriples tools/wdtriples.cpp)
target_link_libraries(wdtriples BZip2::BZip2 nlohmann_json::nlohmann_json)

# wdindex: triples and block indexes of all parts of a dump, in threads
add_executable(wdindex tools/wdindex.cpp)
target_link_libraries(wdindex BZip2::BZip2 nlohmann_json::nlohmann_json)
