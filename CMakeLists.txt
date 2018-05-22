cmake_minimum_required(VERSION 2.8)
project(sba)

set(SOURCES src/sba_levmar.c src/sba_levmar_wrap.c
    src/sba_lapack.c src/sba_crsm.c src/sba_chkjac.c)

add_library(sba STATIC ${SOURCES})

install(TARGETS sba ARCHIVE DESTINATION lib)
install(FILES sba.h DESTINATION include)
