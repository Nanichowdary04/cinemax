juce_add_console_app(cinemax_tests PRODUCT_NAME "cinemax_tests")

target_sources(cinemax_tests PRIVATE DspTests.cpp)

target_compile_definitions(cinemax_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(cinemax_tests
    PRIVATE
        cinemax_engine
        juce::juce_dsp
        juce::juce_audio_formats
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags)

# Lets the tests find the shipped real-ear sets in data/hrtf.
target_compile_definitions(cinemax_tests PRIVATE CINEMAX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
