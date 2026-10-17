// Folio for Android - Gradle wiring.
//
// Gradle's only jobs here are to drive our existing CMake build through
// externalNativeBuild, stage the fonts as assets, and package the result.
// FolioActivity is a tiny Java bridge for SAF, IME and clipboard; the UI
// remains the same C++/Skia surface as the desktop.
plugins { id("com.android.application") }

android {
    namespace = "ai.folio.reader"
    compileSdk = 34
    ndkVersion = "27.2.12479018"   // r27c

    defaultConfig {
        applicationId = "ai.folio.reader"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        externalNativeBuild {
            cmake {
                // Mirrors tools/dev-setup.sh. FOLIO_BUILD_TESTS is off
                // because the test binaries are host executables.
                arguments += listOf(
                    "-DFOLIO_BUILD_UI=ON",
                    "-DFOLIO_BUILD_TESTS=OFF",
                    // Full -g on the large ReaderScreen TU can exhaust a 2 GiB
                    // builder. Keep line tables for native debug builds.
                    "-DCMAKE_CXX_FLAGS_DEBUG=-O0 -g1",
                    "-DCMAKE_C_FLAGS_DEBUG=-O0 -g1",
                    "-DANDROID_STL=c++_shared"
                )
                targets += "folio-android"
            }
        }

        // skia-pack ships arm64 and x64 only - there is no armeabi-v7a
        // slice, so asking for one would fail at link time rather than
        // produce a 32-bit build. x86_64 is here for the emulator.
        //
        // Each ABI needs its prebuilts fetched first, into its own cache
        // directory, or CMake will not find Skia and will configure a
        // build with no renderer:
        //   FOLIO_TARGET=android FOLIO_ABI=arm64 tools/bootstrap-deps.sh
        //   FOLIO_TARGET=android FOLIO_ABI=x64   tools/bootstrap-deps.sh
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.22.1+"
        }
    }

    sourceSets {
        getByName("main") {
            // The fonts the text system needs. main_android.cpp unpacks
            // these out of the APK on first run, because Skia wants paths
            // and APK assets are zip entries.
            assets.srcDirs("../../assets")
        }
    }

    packaging {
        jniLibs {
            // PDFium ships as a shared library and must be packaged
            // uncompressed so the loader can map it directly.
            useLegacyPackaging = false
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false      // nothing to minify: no Java
            isJniDebuggable = false
        }
        debug {
            isJniDebuggable = true
        }
    }
}
