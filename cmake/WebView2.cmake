# Downloads the Microsoft.Web.WebView2 NuGet package into the build tree and points
# JUCE's FindWebView2 at it (no NuGet / PackageManagement setup required).
set(CINEMAX_WEBVIEW2_VERSION "1.0.3485.44" CACHE STRING "Microsoft.Web.WebView2 NuGet version")

set(_wv2_root "${CMAKE_BINARY_DIR}/_deps/webview2")
set(_wv2_dir  "${_wv2_root}/Microsoft.Web.WebView2.${CINEMAX_WEBVIEW2_VERSION}")

if(NOT EXISTS "${_wv2_dir}/build/native/include/WebView2.h")
    message(STATUS "Downloading Microsoft.Web.WebView2 ${CINEMAX_WEBVIEW2_VERSION} from nuget.org")
    file(DOWNLOAD
        "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${CINEMAX_WEBVIEW2_VERSION}"
        "${_wv2_root}/webview2.nupkg"
        STATUS _wv2_status TLS_VERIFY ON)
    list(GET _wv2_status 0 _wv2_code)
    if(NOT _wv2_code EQUAL 0)
        message(FATAL_ERROR "Could not download the WebView2 SDK: ${_wv2_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_wv2_root}/webview2.nupkg" DESTINATION "${_wv2_dir}")
endif()

set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${_wv2_root}" CACHE PATH "Folder containing the Microsoft.Web.WebView2 package" FORCE)
