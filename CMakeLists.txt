cmake_minimum_required(VERSION 3.16)
project(lens LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sources include each other as "core/log.h", "runtime/runtime.h" etc., rooted
# at src/. Without this the cmake build fails to find its own headers.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

add_compile_definitions(LENS_VERSION="1.0.0")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W3 /permissive- /utf-8)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

add_library(lens_core STATIC
  src/core/pattern.cpp
  src/core/mem.cpp
  src/core/mem_win.cpp
  src/core/mem_sim.cpp
  src/core/matrix.cpp
  src/core/yaml.cpp
  src/descriptor/descriptor.cpp
  src/descriptor/resolve.cpp
  src/runtime/runtime.cpp
  src/runtime/snapshot.cpp
  src/overlay/drawlist.cpp
  src/overlay/font.cpp
  src/overlay/font_data.cpp
  src/overlay/backend_offline.cpp
  src/overlay/backend_win32.cpp
  src/features/esp.cpp
  src/features/panel.cpp
  src/features/editor.cpp
  src/script/expr.cpp
  src/config/settings.cpp
  src/selftest.cpp
)

add_executable(lens src/main.cpp)
target_link_libraries(lens PRIVATE lens_core)

# Synthetic live target used by `lens selftest`. POSIX transport only.
if(NOT WIN32)
  add_executable(lens_sim src/sim/main.cpp)
  target_link_libraries(lens_sim PRIVATE lens_core rt)
endif()

install(TARGETS lens RUNTIME DESTINATION bin)
install(DIRECTORY adapters DESTINATION share/lens)
