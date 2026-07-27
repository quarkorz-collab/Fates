cmake_minimum_required(VERSION 3.20)
project(Fates
  VERSION 1.0
  DESCRIPTION "Finding Algebraic Targets via Expression Search"
  LANGUAGES CXX)

option(FATES_USE_STD_CONTAINERS "Use std::unordered containers instead of ankerl::unordered_dense" OFF)
option(FATES_ENABLE_AVX2 "Require AVX2 and enable AVX2 code generation" OFF)

add_executable(fates src/fates.cpp)
target_compile_features(fates PRIVATE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(fates PRIVATE Threads::Threads)

if(FATES_USE_STD_CONTAINERS)
  target_compile_definitions(fates PRIVATE FATES_USE_STD_CONTAINERS=1)
endif()

if(MSVC)
  foreach(flag_var CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO)
    string(REPLACE "/Ob2" "/Ob3" ${flag_var} "${${flag_var}}")
  endforeach()
  target_compile_options(fates PRIVATE /O2 /GL /Gw /Gy /fp:precise /W4 /utf-8)
  if(FATES_ENABLE_AVX2)
    target_compile_options(fates PRIVATE /arch:AVX2)
  endif()
  target_link_options(fates PRIVATE /LTCG /OPT:REF /OPT:ICF)
  set_property(TARGET fates PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(fates PRIVATE -O3 -Wall -Wextra -Wpedantic)
  if(FATES_ENABLE_AVX2)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
      target_compile_options(fates PRIVATE -mavx2)
    else()
      message(FATAL_ERROR "FATES_ENABLE_AVX2 requires an x86 or x86-64 target")
    endif()
  endif()
endif()

# Keep the historical executable name for existing scripts while Fates is the
# canonical product and build target.
add_custom_command(TARGET fates POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:fates>"
          "$<TARGET_FILE_DIR:fates>/pries${CMAKE_EXECUTABLE_SUFFIX}")

include(CTest)
if(BUILD_TESTING)
  add_test(NAME fates_self_test COMMAND fates --self-test)
endif()
