cmake_minimum_required(VERSION 3.0)
project(sba)

# Use FindMKL.cmake module to find Intel MKL
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

set(SOURCES src/sba_levmar.c src/sba_levmar_wrap.c
    src/sba_lapack.c src/sba_crsm.c src/sba_chkjac.c)

add_library(sba STATIC ${SOURCES})
target_include_directories(sba PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

install(TARGETS sba ARCHIVE DESTINATION lib)
install(FILES sba.h DESTINATION include)

add_subdirectory(demo)