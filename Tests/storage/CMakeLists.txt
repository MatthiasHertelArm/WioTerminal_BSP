cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(wio_storage_host_tests C)
  enable_testing()
endif()

set(CMSIS_DRIVER_INCLUDE_DIR "" CACHE PATH "CMSIS/Driver/Include containing genuine Driver_SPI.h")
if(NOT EXISTS "${CMSIS_DRIVER_INCLUDE_DIR}/Driver_SPI.h")
  message(FATAL_ERROR "Set CMSIS_DRIVER_INCLUDE_DIR to an installed ARM::CMSIS Driver/Include directory")
endif()
get_filename_component(WIO_STORAGE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(storage_sd test_sd.c "${WIO_STORAGE_ROOT}/Board/SDK/wio_sd.c")
add_executable(storage_fs test_fs.c
  "${WIO_STORAGE_ROOT}/Board/SDK/wio_fs.c"
  "${WIO_STORAGE_ROOT}/Middleware/FatFs/ff.c")

option(WIO_STORAGE_SANITIZE "Enable host address/undefined-behavior sanitizers" OFF)
foreach(test_target storage_sd storage_fs)
  target_compile_features(${test_target} PRIVATE c_std_11)
  target_include_directories(${test_target} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/stubs"
    "${WIO_STORAGE_ROOT}/Board/Include"
    "${WIO_STORAGE_ROOT}/Middleware/FatFs"
    "${CMSIS_DRIVER_INCLUDE_DIR}")
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${test_target} PRIVATE -Wall -Wextra -Werror)
    if(WIO_STORAGE_SANITIZE)
      target_compile_options(${test_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${test_target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
  add_test(NAME ${test_target} COMMAND ${test_target})
  set_tests_properties(${test_target} PROPERTIES TIMEOUT 30)
endforeach()
