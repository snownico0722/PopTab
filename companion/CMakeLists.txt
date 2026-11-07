cmake_minimum_required(VERSION 3.20)
project(PopTabCompanion LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "PopTab Companion requires Windows.")
endif()

add_executable(PopTabCompanion WIN32 main.cpp)
target_compile_features(PopTabCompanion PRIVATE cxx_std_17)
target_compile_definitions(PopTabCompanion PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(PopTabCompanion PRIVATE user32 gdi32 gdiplus dwmapi)

if(MSVC)
  target_compile_options(PopTabCompanion PRIVATE /W4 /EHsc /utf-8)
endif()
