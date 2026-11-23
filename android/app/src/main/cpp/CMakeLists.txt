# Native part of CINEMAX Mobile: the laptop's DSP engine (engine/src/cinemax, compiled
# unchanged) plus the Android capture -> process -> render pipeline and JNI bridge.
# Built by android/build-native.ps1 with the NDK toolchain.
cmake_minimum_required (VERSION 3.22)
project (cinemax_android LANGUAGES CXX)

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component (ENGINE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../engine/src" ABSOLUTE)

add_library (cinemax SHARED
    jni.cpp
    Pipeline.cpp
    meterbus_android.cpp
    ${ENGINE}/cinemax/core/Params.cpp
    ${ENGINE}/cinemax/dsp/BassEnhancer.cpp
    ${ENGINE}/cinemax/dsp/ContentAnalyzer.cpp
    ${ENGINE}/cinemax/dsp/DialogueEnhancer.cpp
    ${ENGINE}/cinemax/dsp/DspChain.cpp
    ${ENGINE}/cinemax/dsp/Dynamics.cpp
    ${ENGINE}/cinemax/dsp/Equalizer.cpp
    ${ENGINE}/cinemax/dsp/Resampler.cpp
    ${ENGINE}/cinemax/dsp/Hrtf.cpp
    ${ENGINE}/cinemax/dsp/IntelligentEq.cpp
    ${ENGINE}/cinemax/dsp/TvTuning.cpp
    ${ENGINE}/cinemax/dsp/Spatializer.cpp
    ${ENGINE}/cinemax/dsp/Upmixer.cpp
    ${ENGINE}/cinemax/head/FaceAnalyser.cpp)

# ONNX Runtime C API headers (same 1.32 release as the laptop). libonnxruntime.so itself is
# packaged next to libcinemax.so and opened with dlopen, so the sound engine runs without it.
target_include_directories (cinemax PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shim" "${ENGINE}"
    "${ENGINE}/../../third_party/onnxruntime/include")
target_compile_options (cinemax PRIVATE -O2 -Wall -Wno-unused-parameter)
target_link_options (cinemax PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384)
target_link_libraries (cinemax PRIVATE log dl)
