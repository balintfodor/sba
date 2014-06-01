cmake_minimum_required(VERSION 2.8)
project(sba)

set(SOURCES sba_levmar.c sba_levmar_wrap.c
    sba_lapack.c sba_crsm.c sba_chkjac.c)

add_library(sba STATIC ${SOURCES})

install(TARGETS sba ARCHIVE DESTINATION lib)
install(FILES sba.h DESTINATION include)
