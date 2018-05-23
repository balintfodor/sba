# CMake file for sba's demo program

set(DEMO_SOURCES eucsbademo eucsbademo.c imgproj.c readparams.c eucsbademo.h readparams.h)
add_executable(eucsbademo ${DEMO_SOURCES})
target_link_libraries(eucsbademo sba)