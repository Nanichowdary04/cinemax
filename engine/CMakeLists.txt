# The engine is an INTERFACE library: its sources are compiled into each consumer
# together with that consumer's single copy of the JUCE modules (the same model JUCE
# uses for its own modules). This avoids duplicate JUCE symbols across static libs.
add_library(cinemax_engine INTERFACE)

set(_src ${CMAKE_CURRENT_SOURCE_DIR}/src/cinemax)

target_sources(cinemax_engine INTERFACE
    ${_src}/core/Params.cpp
    ${_src}/core/Settings.cpp
    ${_src}/core/Metering.cpp
    ${_src}/core/DeviceMonitor.cpp
    ${_src}/core/Engine.cpp
    ${_src}/dsp/ContentAnalyzer.cpp
    ${_src}/dsp/Equalizer.cpp
    ${_src}/dsp/BassEnhancer.cpp
    ${_src}/dsp/DialogueEnhancer.cpp
    ${_src}/dsp/Hrtf.cpp
    ${_src}/dsp/IntelligentEq.cpp
    ${_src}/dsp/TvTuning.cpp
    ${_src}/dsp/Spatializer.cpp
    ${_src}/dsp/Upmixer.cpp
    ${_src}/dsp/Dynamics.cpp
    ${_src}/dsp/Resampler.cpp
    ${_src}/dsp/DspChain.cpp
    ${_src}/audio/WasapiPipeline.cpp
    ${_src}/audio/VolumeMirror.cpp
    ${_src}/vision/Picture.cpp
    ${_src}/vision/VisionEngine.cpp)

target_include_directories(cinemax_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(cinemax_engine INTERFACE
    cinemax_win
    juce::juce_dsp
    juce::juce_audio_formats
    juce::juce_events
    avrt)
