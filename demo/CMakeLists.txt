# CMake file for sba's demo program

set(DEMO_SOURCES eucsbademo eucsbademo.c imgproj.c readparams.c eucsbademo.h readparams.h)
add_executable(eucsbademo ${DEMO_SOURCES})

find_package(MKL)
if (MKL_FOUND)
    include_directories(${MKL_INCLUDE_DIR})
    target_link_libraries(eucsbademo sba ${MKL_LIBRARIES})
else()
  if(HAVE_F2C)
    target_link_libraries(eucsbademo sba ${LAPACK_LIB} ${BLAS_LIB} ${F2C_LIB})
  else(HAVE_F2C)
    target_link_libraries(eucsbademo sba ${LAPACK_LIB} ${BLAS_LIB} ${F77_LIB} ${I77_LIB})
  endif(HAVE_F2C)
endif()