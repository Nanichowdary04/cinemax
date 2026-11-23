set(CINEMAX_UI_HTML "" CACHE FILEPATH "Built single-file UI (index.html produced by ui/build-ui.ps1)")

if(NOT CINEMAX_UI_HTML OR NOT EXISTS "${CINEMAX_UI_HTML}")
    message(FATAL_ERROR
        "CINEMAX_UI_HTML is not set or the file does not exist.\n"
        "Build the UI first (ui/build-ui.ps1) and pass -DCINEMAX_UI_HTML=<path to index.html>. "
        "build.ps1 does both automatically.")
endif()

juce_add_binary_data(CinemaxBinaryData
    HEADER_NAME CinemaxBinaryData.h
    NAMESPACE CinemaxBinary
    SOURCES
        "${CINEMAX_UI_HTML}"
        resources/cinemax_icon.png)

juce_add_gui_app(Cinemax
    PRODUCT_NAME "CINEMAX"
    COMPANY_NAME "CINEMAX"
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/cinemax_icon.png"
    NEEDS_WEBVIEW2 TRUE)

target_sources(Cinemax PRIVATE
    src/Main.cpp
    src/MainWindow.cpp
    src/WebBridge.cpp
    src/HeadTracker.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/cinemax/head/FaceAnalyser.cpp)

# Webcam head tracking: ONNX Runtime C API headers (the DLL is loaded at run time).
target_include_directories(Cinemax PRIVATE ${CMAKE_SOURCE_DIR}/third_party/onnxruntime/include)

target_compile_definitions(Cinemax PRIVATE
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Cinemax,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Cinemax,JUCE_VERSION>")

target_link_libraries(Cinemax
    PRIVATE
        CinemaxBinaryData
        cinemax_engine
        juce::juce_gui_extra
        juce::juce_dsp
        juce::juce_audio_formats
        dwmapi
        mfplat
        mf
        mfreadwrite
        mfuuid
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# The recovery guard must live next to CINEMAX.exe.
add_dependencies(Cinemax CinemaxGuard)
add_custom_command(TARGET Cinemax POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:CinemaxGuard> $<TARGET_FILE_DIR:Cinemax>
    COMMENT "Copying CinemaxGuard.exe next to CINEMAX.exe")
