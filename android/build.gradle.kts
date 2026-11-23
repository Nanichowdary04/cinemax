// Build output lives outside OneDrive (thousands of generated files).
layout.buildDirectory.set(file("${System.getProperty("user.home")}/CINEMAX-dev/android-build/root"))

plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.compose) apply false
}
