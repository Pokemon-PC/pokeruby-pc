# Builds pokeruby's host tools natively (Windows/Linux/macOS) without
# pkg-config or a system libpng. Each executable is written next to its
# sources (tools/<name>/<name>[.exe]) so the original GBA Makefile and its
# asset rules (graphics_file_rules.mk, map_data_rules.mk...) find them as-is.
#
#   cmake -S src/platform_pc/tools -B build-tools -G Ninja
#   cmake --build build-tools
cmake_minimum_required(VERSION 3.20)
project(pokeruby_tools C CXX)

set(PRS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
get_filename_component(PRS_ROOT "${PRS_ROOT}" ABSOLUTE)
set(T "${PRS_ROOT}/tools")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Static runtime on MinGW so the tools run from any shell.
if(MINGW)
    add_link_options(-static)
endif()

# ---- zlib + libpng (system first, else downloaded and built statically) ----
find_package(PNG QUIET)
if(PNG_FOUND)
    add_library(prs_png INTERFACE)
    target_link_libraries(prs_png INTERFACE PNG::PNG)
else()
    include(FetchContent)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(zlib)
    if(TARGET zlibstatic)
        set(PRS_ZLIB zlibstatic)
        target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    else()
        set(PRS_ZLIB zlib)
    endif()
    if(NOT TARGET ZLIB::ZLIB)   # newer zlib CMake files define it themselves
        add_library(ZLIB::ZLIB ALIAS ${PRS_ZLIB})
    endif()
    set(ZLIB_FOUND TRUE)
    set(ZLIB_LIBRARY ${PRS_ZLIB})
    set(ZLIB_LIBRARIES ${PRS_ZLIB})
    set(ZLIB_INCLUDE_DIR ${zlib_SOURCE_DIR};${zlib_BINARY_DIR})
    set(ZLIB_INCLUDE_DIRS ${ZLIB_INCLUDE_DIR})

    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    set(PNG_STATIC ON CACHE BOOL "" FORCE)
    set(PNG_TESTS OFF CACHE BOOL "" FORCE)
    set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
    set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
    set(PNG_BUILD_ZLIB ON CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(libpng
        URL https://download.sourceforge.net/libpng/libpng-1.6.43.tar.gz
        URL_HASH SHA256=e804e465d4b109b5ad285a8fb71f0dd3f74f0068f91ce3cdfde618180c174925
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(libpng)
    add_library(prs_png INTERFACE)
    target_link_libraries(prs_png INTERFACE png_static ${PRS_ZLIB})
    target_include_directories(prs_png INTERFACE ${libpng_SOURCE_DIR} ${libpng_BINARY_DIR})
endif()

# ---- helper ----
function(prs_tool name)
    cmake_parse_arguments(A "" "STD" "SOURCES;LIBS;DEFS" ${ARGN})
    set(srcs)
    foreach(s ${A_SOURCES})
        list(APPEND srcs "${T}/${name}/${s}")
    endforeach()
    add_executable(${name} ${srcs})
    target_include_directories(${name} PRIVATE "${T}/${name}")
    if(A_DEFS)
        target_compile_definitions(${name} PRIVATE ${A_DEFS})
    endif()
    if(A_LIBS)
        target_link_libraries(${name} PRIVATE ${A_LIBS})
    endif()
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${T}/${name}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${T}/${name}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${T}/${name}")
endfunction()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

prs_tool(bin2c     SOURCES bin2c.c)
prs_tool(gbafix    SOURCES gbafix.c)
prs_tool(gbagfx    SOURCES main.c convert_png.c gfx.c jasc_pal.c lz.c rl.c util.c font.c huff.c
                   LIBS prs_png DEFS PNG_SKIP_SETJMP_CHECK)
prs_tool(rsfont    SOURCES main.c convert_png.c util.c font.c
                   LIBS prs_png DEFS PNG_SKIP_SETJMP_CHECK)
prs_tool(jsonproc  SOURCES jsonproc.cpp)
prs_tool(mapjson   SOURCES json11.cpp mapjson.cpp)
prs_tool(mid2agb   SOURCES agb.cpp error.cpp main.cpp midi.cpp tables.cpp)
file(GLOB _preproc RELATIVE "${T}/preproc" "${T}/preproc/*.cpp")
prs_tool(preproc   SOURCES ${_preproc})
# preproc has its own io.h: keep tools/preproc off the include path so that
# <io.h> (needed for _setmode on Windows) is the system header.
set_property(TARGET preproc PROPERTY INCLUDE_DIRECTORIES "")
prs_tool(ramscrgen SOURCES main.cpp sym_file.cpp elf.cpp)
prs_tool(scaninc   SOURCES scaninc.cpp c_file.cpp asm_file.cpp source_file.cpp)
file(GLOB _wav2agb RELATIVE "${T}/wav2agb" "${T}/wav2agb/*.cpp")
prs_tool(wav2agb   SOURCES ${_wav2agb})
