cmake_minimum_required(VERSION 3.24)
project(hdr10pg LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build with MSVC x64 (the vendor SDKs are Windows x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(nvapi
  GIT_REPOSITORY https://github.com/NVIDIA/nvapi.git
  GIT_TAG 51e5aae4bc40c3ccd3fb7d4dfd94256e7bfc496e) # R620
FetchContent_Declare(igcl
  GIT_REPOSITORY https://github.com/intel/drivers.gpu.control-library.git
  GIT_TAG 1db55c364b15649427a71a05d950ab9c1eaf9a07) # v306
FetchContent_MakeAvailable(nvapi igcl)

# cApiWrapper.cpp loads the driver's ControlLib.dll at run time, so no Intel import library is needed.
add_executable(hdr10pg src/main.cpp src/displays.cpp src/test_window.cpp ${igcl_SOURCE_DIR}/Source/cApiWrapper.cpp)
target_include_directories(hdr10pg SYSTEM PRIVATE ${nvapi_SOURCE_DIR} ${igcl_SOURCE_DIR}/include)
target_link_libraries(hdr10pg PRIVATE ${nvapi_SOURCE_DIR}/amd64/nvapi64.lib d3d12 dxgi user32)
target_compile_definitions(hdr10pg PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN VC_EXTRALEAN)
set_source_files_properties(src/main.cpp src/displays.cpp src/test_window.cpp PROPERTIES COMPILE_OPTIONS "/W4")

enable_testing()
# Smoke test: detection must run cleanly on any machine, with or without a supported display.
add_test(NAME detect COMMAND hdr10pg detect)
add_test(NAME selftest COMMAND hdr10pg selftest)
add_test(NAME sdr-image COMMAND hdr10pg sdr-image)
add_test(NAME sdr-image-green COMMAND hdr10pg sdr-image --green)
