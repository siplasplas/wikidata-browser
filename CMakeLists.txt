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

# wdsort: sorts the triple files by (bit-reversed property, target, item)
add_executable(wdsort tools/wdsort.cpp tools/triples.h)
# wdmerge: merges the sorted triple files into 256 index files by property % 256
add_executable(wdmerge tools/wdmerge.cpp tools/triples.h)

# wddense: dense indexes q.idx, p.idx (with English names), l.idx from index/ of wdindex
add_executable(wddense tools/wddense.cpp)
