juce_add_console_app(cinemax_cli PRODUCT_NAME "cinemax-cli")

target_sources(cinemax_cli PRIVATE cli/CliMain.cpp ${CMAKE_SOURCE_DIR}/app/src/HeadTracker.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/cinemax/head/FaceAnalyser.cpp)
target_include_directories(cinemax_cli PRIVATE ${CMAKE_SOURCE_DIR}/app/src ${CMAKE_SOURCE_DIR}/third_party/onnxruntime/include)

target_compile_definitions(cinemax_cli PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(cinemax_cli
    PRIVATE
        cinemax_engine
        juce::juce_dsp
        juce::juce_audio_formats
        juce::juce_events
        mfplat
        mf
        mfreadwrite
        mfuuid
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
