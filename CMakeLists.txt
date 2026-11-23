cmake_minimum_required(VERSION 3.21)
project(VivitarCapture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C++ runtime so the .exe runs without installing the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC UNICODE _UNICODE)

add_executable(VivitarCapture WIN32
  src/main.cpp
  src/capture.cpp
  src/audio_out.cpp
  src/recorder.cpp
  src/renderer.cpp
  src/settings.cpp
  src/util.cpp
  src/amd_fx.cpp
  res/app.rc
  res/app.manifest)

target_include_directories(VivitarCapture PRIVATE third_party/amf/include)
target_compile_definitions(VivitarCapture PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(VivitarCapture PRIVATE /W3 /utf-8 /MP /permissive-)
  target_compile_options(imgui PRIVATE /utf-8 /MP)
endif()

target_link_libraries(VivitarCapture PRIVATE
  imgui d3d11 dxgi d3dcompiler dwmapi
  mfplat mfreadwrite mf mfuuid
  ole32 oleaut32 uuid windowscodecs shell32 shlwapi)
