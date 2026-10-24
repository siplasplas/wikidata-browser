cmake_minimum_required(VERSION 3.24)
project(wikidata-browser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# wdlist: lists the parts of Wikidata dumps of a directory (<date>.lst)
add_executable(wdlist tools/wdlist.cpp)

find_package(BZip2 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

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

# wdsample: sizes of a slimmer Wikidata on a random sample of whole bz2 streams
add_executable(wdsample tools/wdsample.cpp)
target_link_libraries(wdsample BZip2::BZip2 nlohmann_json::nlohmann_json PkgConfig::ZSTD)

# wdconvert: the dump slimmed, in zstd frames with dictionaries and frame indexes
add_executable(wdconvert tools/wdconvert.cpp tools/dump.h tools/claims.h)
target_link_libraries(wdconvert BZip2::BZip2 nlohmann_json::nlohmann_json PkgConfig::ZSTD)

# wdfilter: triple index without scholarly articles in no Wikipedia; mask of the items left out
add_executable(wdfilter tools/wdfilter.cpp tools/triples.h)

# wddense: dense indexes q.idx, p.idx (with English names), l.idx from index/ of wdindex
add_executable(wddense tools/wddense.cpp)

find_package(ICU REQUIRED COMPONENTS uc)
# wdnames: names, names-by-q, q-by-name (search by name) from index/ of wdindex
add_executable(wdnames tools/wdnames.cpp tools/fold.h)
target_link_libraries(wdnames ICU::uc)

# wdbrowse: the browser (Qt6). The tools above need no Qt; -DWDB_BROWSER=OFF builds only them.
option(WDB_BROWSER "Build the browser wdbrowse (Qt6, qcodeedit, qt-extra)" ON)
if(WDB_BROWSER)
    set(CMAKE_AUTOMOC ON)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    # qcodeedit (LGPL-3.0): the editor widget with Kate syntax highlighting and
    # folding. The sibling checkout ../qcodeedit when present, otherwise fetched.
    set(QCE_BUILD_DEMO OFF CACHE BOOL "" FORCE)
    set(QCE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(QCE_BUILD_KATE ON CACHE BOOL "" FORCE)
    foreach(option QCE_BUILD_KATE_DOWNLOADER QCE_BUILD_ENCODING QCE_BUILD_GRAMMAR QCE_BUILD_SPELL
            QCE_BUILD_SPELL_DOWNLOADER QCE_BUILD_SPELLTUNE QCE_BUILD_DIFF)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    include(FetchContent)
    set(QCODEEDIT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../qcodeedit CACHE PATH "qcodeedit sources")
    if(EXISTS ${QCODEEDIT_DIR}/CMakeLists.txt)
        FetchContent_Declare(qcodeedit SOURCE_DIR ${QCODEEDIT_DIR})
    else()
        FetchContent_Declare(qcodeedit GIT_REPOSITORY https://gitlab.com/siplasplas/qcodeedit.git GIT_TAG master)
    endif()
    FetchContent_MakeAvailable(qcodeedit)
    # qt-extra: the file dialog. The sibling checkout ../qt-extra when present, otherwise fetched.
    set(QT_EXTRA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../qt-extra CACHE PATH "qt-extra sources")
    if(EXISTS ${QT_EXTRA_DIR}/CMakeLists.txt)
        FetchContent_Declare(qt-extra SOURCE_DIR ${QT_EXTRA_DIR})
    else()
        FetchContent_Declare(qt-extra GIT_REPOSITORY https://github.com/siplasplas/qt-extra.git GIT_TAG master)
    endif()
    FetchContent_MakeAvailable(qt-extra)

    add_executable(wdbrowse browser/main.cpp browser/Entities.cpp browser/Entities.h browser/FindBar.cpp
            browser/FindBar.h browser/TreeFindBar.cpp browser/TreeFindBar.h)
    target_include_directories(wdbrowse PRIVATE tools) # fold.h: the same search key as wdnames
    target_link_libraries(wdbrowse Qt6::Widgets qcodeedit::qcodeedit qcodeedit::kate qt-extra
            nlohmann_json::nlohmann_json BZip2::BZip2 ICU::uc)
endif()
