cmake_minimum_required(VERSION 3.20)

add_library(sircore_hosted_zabi
  hosted_zabi.c
  zcl1.c
  sem_host.c
  guest_mem.c
  handles.c
  hosted_file_fs.c
)

target_include_directories(sircore_hosted_zabi PUBLIC ${CMAKE_CURRENT_LIST_DIR})

target_compile_options(sircore_hosted_zabi PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)
