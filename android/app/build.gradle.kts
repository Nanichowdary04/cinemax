plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

layout.buildDirectory.set(file("${System.getProperty("user.home")}/CINEMAX-dev/android-build/app"))

android {
    namespace = "app.cinemax.mobile"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "app.cinemax.mobile"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "1.0.0"
    }

    buildTypes {
        release {
            optimization {
                enable = false
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }
    // libcinemax.so (laptop DSP engine) is built by ../build-native.ps1 with the NDK.
    sourceSets["main"].jniLibs.directories.add("${System.getProperty("user.home")}/CINEMAX-dev/android-build/jniLibs")
    // Real-ear recordings + face model, staged by build-native.ps1 from data/.
    sourceSets["main"].assets.directories.add("${System.getProperty("user.home")}/CINEMAX-dev/android-build/assets")
    // Keep native libraries compressed in the APK (ONNX Runtime is 36 MB uncompressed, ~14 MB packed).
    packaging {
        jniLibs.useLegacyPackaging = true
    }
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
}
