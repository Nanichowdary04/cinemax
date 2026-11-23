cmake_minimum_required(VERSION 3.25)

project(CINEMAX VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR
        "CINEMAX is a Windows application and must be built with MSVC "
        "(Visual Studio 2022/2026 or Build Tools with the C++ workload). "
        "JUCE 9 does not support MinGW. Use build.ps1, which sets up the MSVC environment.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: the executables need no Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_compile_definitions(NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
add_compile_options(/utf-8 /permissive-)

# ---------------------------------------------------------------------------
# JUCE 9 (framework for DSP utilities, JSON/files, GUI window, tray and WebView2)
set(CINEMAX_JUCE_DIR "" CACHE PATH "Existing JUCE 9 checkout (optional; fetched from GitHub otherwise)")
if(CINEMAX_JUCE_DIR)
    add_subdirectory("${CINEMAX_JUCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/juce-build" EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.6
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(juce)
endif()

# Microsoft WebView2 SDK (NuGet) - headers + static loader used by JUCE's WebBrowserComponent
include(cmake/WebView2.cmake)

# ---------------------------------------------------------------------------
add_subdirectory(win)      # JUCE-free Win32 layer (endpoints, routing, recovery state)
add_subdirectory(engine)   # audio engine: DSP + WASAPI pipeline + device policy
add_subdirectory(guard)    # CinemaxGuard.exe crash/logon recovery watchdog
add_subdirectory(app)      # CINEMAX.exe desktop application (WebView2 UI + tray)
add_subdirectory(tools)    # cinemax-cli diagnostics
add_subdirectory(tests)    # offline DSP test-suite

enable_testing()
add_test(NAME cinemax_dsp_tests COMMAND cinemax_tests)
