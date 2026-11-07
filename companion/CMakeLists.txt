cmake_minimum_required(VERSION 3.20)
project(PopTabCompanion LANGUAGES CXX RC)
if(NOT WIN32)
  message(FATAL_ERROR "PopTab Companion requires Windows.")
endif()
set(EXT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(RESOURCES
  "1101 RCDATA \"${EXT_DIR}/manifest.json\"\n"
  "1102 RCDATA \"${EXT_DIR}/background.js\"\n"
  "1103 RCDATA \"${EXT_DIR}/native-companion.js\"\n"
  "1104 RCDATA \"${EXT_DIR}/video-float.js\"\n"
  "1116 RCDATA \"${EXT_DIR}/assets/icons/icon-16.png\"\n"
  "1132 RCDATA \"${EXT_DIR}/assets/icons/icon-32.png\"\n"
  "1148 RCDATA \"${EXT_DIR}/assets/icons/icon-48.png\"\n"
  "1199 RCDATA \"${EXT_DIR}/assets/icons/icon-128.png\"\n"
)
string(JOIN "" PAYLOAD_RC ${RESOURCES})
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/extension_payload.rc" CONTENT "${PAYLOAD_RC}")
add_executable(PopTabCompanion WIN32 main.cpp setup.cpp tray.cpp "${CMAKE_CURRENT_BINARY_DIR}/extension_payload.rc")
target_compile_features(PopTabCompanion PRIVATE cxx_std_17)
target_compile_definitions(PopTabCompanion PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(PopTabCompanion PRIVATE user32 gdi32 gdiplus dwmapi shell32 advapi32 comctl32)
if(MSVC)
  target_compile_options(PopTabCompanion PRIVATE /W4 /EHsc /utf-8)
  # A portable EXE must not depend on a separately installed VC++ runtime.
  set_property(TARGET PopTabCompanion PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
