add_library(cinemax_win STATIC
    src/Win32Common.cpp
    src/Endpoints.cpp
    src/DolbyProbe.cpp
    src/RoutingState.cpp
    src/AudioSessions.cpp
    src/Displays.cpp
    src/IntelPipe.cpp
    src/VisionState.cpp
    src/ForegroundApp.cpp)

target_include_directories(cinemax_win PUBLIC include)
# Intel Graphics Control Library header (runtime-loaded ControlLib.dll; Intel Software License)
target_include_directories(cinemax_win PRIVATE ${CMAKE_SOURCE_DIR}/third_party/igcl)
target_link_libraries(cinemax_win PUBLIC ole32 oleaut32 uuid shell32 advapi32 propsys dxgi dxva2 wbemuuid gdi32 user32)
set_target_properties(cinemax_win PROPERTIES FOLDER "CINEMAX")
