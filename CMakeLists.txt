cmake_minimum_required(VERSION 2.8)
project(sba)

add_library(sba STATIC
  sba_levmar.c sba_levmar_wrap.c sba_lapack.c sba_crsm.c sba_chkjac.c
  sba.h sba_chkjac.h compiler.h
)

