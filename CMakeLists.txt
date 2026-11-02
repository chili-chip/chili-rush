# Basic parameters; check that these match your project / environment
cmake_minimum_required(VERSION 3.15...3.31)

# Replace "game" with a name for your project (this is used the name of the output)
project(chili-rush LANGUAGES C CXX ASM)

# Add your sources here (adding headers is optional, but helps some CMake generators)
set(PROJECT_SOURCE game.cpp game.hpp audio.cpp audio.hpp sprites.hpp)

# ... and any other files you want in the release here
set(PROJECT_DISTRIBS LICENSE README.md)

# Build configuration; approach this with caution!
if(MSVC)
  add_compile_options("/W4" "/wd4244" "/wd4324" "/wd4458" "/wd4100")
else()
  add_compile_options("-Wall" "-Wextra" "-Wdouble-promotion" "-Wno-unused-parameter")
endif()

find_package (32BLIT CONFIG REQUIRED PATHS ../32blit-sdk $ENV{PATH_32BLIT_SDK})

blit_executable (${PROJECT_NAME} ${PROJECT_SOURCE})

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".html")
else()
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".elf")
endif()

blit_assets_yaml (${PROJECT_NAME} assets.yml)
blit_metadata (${PROJECT_NAME} metadata.yml)
add_custom_target (flash DEPENDS ${PROJECT_NAME}.flash)

# Headless self-test: a planning bot plays thousands of seconds and checks
# that every obstacle pattern can be survived (see README). Desktop only.
option(CHILI_SELFTEST "Build chili-rush-selftest, the headless fairness and edge-case test" OFF)
if(CHILI_SELFTEST AND NOT CMAKE_CROSSCOMPILING AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  add_executable(chili-rush-selftest selftest_main.cpp game.cpp ${CMAKE_CURRENT_BINARY_DIR}/assets.cpp)
  target_compile_definitions(chili-rush-selftest PRIVATE CHILI_SELFTEST)
  if(NOT MSVC)
    target_compile_options(chili-rush-selftest PRIVATE -Wno-double-promotion)  # the test's printf reports
  endif()
  target_include_directories(chili-rush-selftest PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(chili-rush-selftest BlitEngine)
  add_dependencies(chili-rush-selftest ${PROJECT_NAME})  # packs the assets
endif()

# setup release packages
install (FILES ${PROJECT_DISTRIBS} DESTINATION .)
set (CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set (CPACK_GENERATOR "ZIP" "TGZ")
include (CPack)
