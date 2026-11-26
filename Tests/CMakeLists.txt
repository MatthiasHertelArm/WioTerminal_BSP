cmake_minimum_required(VERSION 3.20)
project(WioSDKHostTests LANGUAGES C)

include(CTest)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Assertions are the test oracle and must remain enabled even in Release.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-UNDEBUG)
endif()

option(WIO_TEST_SANITIZERS "Enable address/undefined-behavior sanitizers" OFF)
if(WIO_TEST_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# The SD transport tests compile against the genuine CMSIS Driver API, not a
# second hand-written ABI. CI supplies the installed pack root; local builds
# may select an explicit include directory without downloading dependencies.
set(CMSIS_DRIVER_INCLUDE_DIR "" CACHE PATH "CMSIS/Driver/Include from ARM::CMSIS")
if(NOT CMSIS_DRIVER_INCLUDE_DIR)
  file(GLOB cmsis_driver_candidates LIST_DIRECTORIES TRUE
       "$ENV{CMSIS_PACK_ROOT}/ARM/CMSIS/*/CMSIS/Driver/Include"
       "$ENV{HOME}/.cache/arm/packs/ARM/CMSIS/*/CMSIS/Driver/Include")
  list(SORT cmsis_driver_candidates COMPARE NATURAL ORDER DESCENDING)
  foreach(candidate IN LISTS cmsis_driver_candidates)
    if(EXISTS "${candidate}/Driver_SPI.h")
      set(CMSIS_DRIVER_INCLUDE_DIR "${candidate}")
      break()
    endif()
  endforeach()
endif()
if(NOT EXISTS "${CMSIS_DRIVER_INCLUDE_DIR}/Driver_SPI.h")
  message(FATAL_ERROR "Set CMSIS_DRIVER_INCLUDE_DIR to an installed CMSIS/Driver/Include")
endif()

add_subdirectory(sercom)
add_subdirectory(storage)
add_subdirectory(network)
