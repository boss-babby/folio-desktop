// folio-ui/shell/main_android.cpp
//
// The Android entry point. NativeActivity loads this as a shared library
// and calls android_main on a thread of its own, so unlike main() it is
// free to block - which is exactly what Folio's idle-until-an-event loop
// wants.
//
// Everything of substance is in NativeShell.cpp, shared with the desktop
// binary. The only Android-specific work here is handing the activity to
// the platform layer and unpacking the fonts, because APK assets are not
// files and Skia's font manager needs files.

#include "NativeShell.h"
#include "folio/platform/Platform.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "folio", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "folio", __VA_ARGS__)

namespace {

/// Copy one asset out of the APK into a real file.
///
/// Assets inside an APK are entries in a zip, reachable only through
/// AAssetManager. Skia's font manager, PDFium and PoDoFo all want paths,
/// so the handful of files they need are unpacked once into the app's
/// private directory on first run. This is a few hundred kilobytes of
/// fonts, not the documents - those are opened through the file layer.
bool unpackAsset(AAssetManager* mgr, const char* name, const std::string& dest) {
    struct stat st {};
    if (stat(dest.c_str(), &st) == 0 && st.st_size > 0) return true;  // cached

    AAsset* asset = AAssetManager_open(mgr, name, AASSET_MODE_STREAMING);
    if (!asset) {
        LOGE("asset missing: %s", name);
        return false;
    }
    FILE* out = std::fopen(dest.c_str(), "wb");
    if (!out) {
        LOGE("cannot write %s: %s", dest.c_str(), std::strerror(errno));
        AAsset_close(asset);
        return false;
    }
    char buf[64 * 1024];
    int n = 0;
    bool ok = true;
    while ((n = AAsset_read(asset, buf, sizeof(buf))) > 0) {
        if (std::fwrite(buf, 1, static_cast<std::size_t>(n), out) !=
            static_cast<std::size_t>(n)) {
            ok = false;
            break;
        }
    }
    if (n < 0) ok = false;
    std::fclose(out);
    AAsset_close(asset);
    if (!ok) LOGE("short write unpacking %s", name);
    return ok;
}

/// Recreate, on disk, the layout the desktop build ships beside the binary.
///
/// Note the path arithmetic, which is easy to get wrong by one level.
/// Gradle packages the *contents* of assets.srcDirs at the asset root, so
/// the repository's `assets/fonts/DMSans-Variable.ttf` is addressed inside
/// the APK as `fonts/DMSans-Variable.ttf`. The shell then appends
/// "/fonts" to whatever directory it is handed, so what we return has to
/// be the parent of the unpacked `fonts/`, not `fonts/` itself.
std::string unpackAssets(android_app* app) {
    const char* dir = app->activity->internalDataPath;
    if (!dir) return {};
    const std::string root = std::string(dir) + "/assets";
    ::mkdir(root.c_str(), 0700);
    const std::string fonts = root + "/fonts";
    ::mkdir(fonts.c_str(), 0700);

    AAssetManager* mgr = app->activity->assetManager;
    if (!mgr) return {};

    // Enumerate rather than hard-code: the font set is chosen by
    // tools/gen-tokens.py and CMake, and a list duplicated here would rot.
    AAssetDir* d = AAssetManager_openDir(mgr, "fonts");
    if (!d) {
        LOGE("no fonts/ in the APK - check assets.srcDirs in build.gradle.kts");
        return root;
    }
    int count = 0;
    while (const char* name = AAssetDir_getNextFileName(d)) {
        const std::string rel = std::string("fonts/") + name;
        if (unpackAsset(mgr, rel.c_str(), fonts + "/" + name)) ++count;
    }
    AAssetDir_close(d);
    LOGI("unpacked %d font file(s) to %s", count, fonts.c_str());
    return root;
}

}  // namespace

extern "C" void android_main(android_app* app) {
    // Must happen before anything asks the platform layer for a window.
    folio::platform::setAndroidApp(app);

    folio::ui::NativeShellOptions opts;
    opts.assetDir = unpackAssets(app);
    // Size and software-rendering flags are meaningless here: the activity
    // dictates the surface, and a phone without working GLES is not a
    // configuration worth a fallback path.

    const int rc = folio::ui::runNativeShell(opts);
    LOGI("shell exited with %d", rc);

    // Returning from android_main ends the activity's thread. The glue
    // expects the process to go away, so do not try to keep it alive.
    ANativeActivity_finish(app->activity);
}
