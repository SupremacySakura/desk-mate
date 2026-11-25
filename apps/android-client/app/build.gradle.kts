plugins {
    id("com.android.application")
}

val coreDir = rootProject.file(
    providers.gradleProperty("deskmate.coreDir").getOrElse("../../core")
)

android {
    namespace = "dev.deskmate.client"
    compileSdk = 35

    defaultConfig {
        applicationId = "dev.deskmate.client"
        minSdk = 26
        targetSdk = 35
        versionCode = 3
        versionName = "1.2.0"
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DDESKMATE_CORE_DIR=${coreDir.absolutePath}",
                    "-DDESKMATE_COMMON_DIR=${rootProject.projectDir.parentFile}/common"
                )
                cppFlags += listOf("-std=c++20", "-Wall", "-Wextra")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        prefab = false
        viewBinding = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    // The client only uses framework APIs (Activity, SurfaceView, UsbManager,
    // MediaCodec via NDK), so there are no library dependencies to download.
}
