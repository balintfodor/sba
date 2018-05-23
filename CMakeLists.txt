cmake_minimum_required(VERSION 3.0)
project(sba)

option(BUILD_SHARED_LIBS "Build shared libs" OFF)
option(USE_MKL "Use Intel MKL for BLAS/LAPACK" OFF)
option(BUILD_DEMO "Compile provided demo" ON)

# Use FindMKL.cmake module to find Intel MKL
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

# Select source files to build
set(SOURCES src/sba_levmar.c src/sba_levmar_wrap.c
    src/sba_lapack.c src/sba_crsm.c src/sba_chkjac.c)
set(PUBLIC_HEADERS src/sba.h)

# Add targets
add_library(sba ${SOURCES})
target_include_directories(sba PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link BLAS/LAPACK
if(USE_MKL)
    find_package(MKL REQUIRED)
    include_directories(${MKL_INCLUDE_DIR})
    target_link_libraries(sba ${MKL_LIBRARIES})
else()
    find_package(BLAS REQUIRED)
    find_package(LAPACK REQUIRED)
    target_link_libraries(sba -lm ${BLAS_LIBRARIES} ${LAPACK_LIBRARIES})
endif()

# Define install command
install(TARGETS sba DESTINATION lib)
install(FILES ${PUBLIC_HEADERS} DESTINATION include)

# Compile demo
if(BUILD_DEMO)
    add_subdirectory(demo)
endif()