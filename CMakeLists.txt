# Compilación del port (Linux y Windows con MSYS2/MinGW-w64).
#   python3 tools/setup.py --iso juego.iso      (genera build/gen y compila con make en Linux)
# o, a mano tras generar build/gen:
#   cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/cmake
cmake_minimum_required(VERSION 3.20)
project(cars C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CARS_GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "C generado por tools/recomp/recomp.py")

if(NOT EXISTS "${CARS_GEN_DIR}/funcs.h")
  message(FATAL_ERROR "No hay código generado en ${CARS_GEN_DIR}: ejecuta antes tools/setup.py o tools/recomp/recomp.py")
endif()
if(MSVC)
  message(FATAL_ERROR "MSVC no está soportado: usa GCC o Clang (en Windows, MSYS2 MinGW-w64)")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)

# Código recompilado: enorme y plano; -O1 compila rápido y rinde bien.
file(GLOB CARS_GEN_SRC "${CARS_GEN_DIR}/*.c")
add_library(cars_gen OBJECT ${CARS_GEN_SRC})
target_include_directories(cars_gen PRIVATE runtime/include "${CARS_GEN_DIR}")
target_compile_options(cars_gen PRIVATE -O1 -fno-strict-aliasing -fwrapv -w)

file(GLOB_RECURSE CARS_RT_SRC CONFIGURE_DEPENDS runtime/src/*.cpp)
add_executable(cars ${CARS_RT_SRC} $<TARGET_OBJECTS:cars_gen>)
target_include_directories(cars PRIVATE runtime/include runtime/src runtime/third_party)
target_compile_options(cars PRIVATE -Wall -Wextra -fno-strict-aliasing)
target_link_libraries(cars PRIVATE PkgConfig::SDL2 PkgConfig::FFMPEG)
if(WIN32)
  target_link_options(cars PRIVATE -static-libgcc -static-libstdc++)
endif()
set_target_properties(cars PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build")
