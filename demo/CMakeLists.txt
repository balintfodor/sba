# CMake file for sba's demo program

set(DEMO_SOURCES eucsbademo eucsbademo.c imgproj.c readparams.c eucsbademo.h readparams.h)
add_executable(eucsbademo ${DEMO_SOURCES})

set(USE_MKL 0 CACHE BOOL "Set true to use Intel MKL libs")

if(USE_MKL)
  find_package(MKL REQUIRED)
  include_directories(${MKL_INCLUDE_DIR})
  target_link_libraries(eucsbademo sba ${MKL_LIBRARIES})
else()
  find_package(BLAS REQUIRED)
  find_package(LAPACK REQUIRED)
  target_link_libraries(eucsbademo sba -lm ${BLAS_LIBRARIES} ${LAPACK_LIBRARIES})
endif()