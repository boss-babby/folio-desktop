// folio-render/skia/PageThemeShader.h
//
// The page-theme fragment shader, in SkSL.
//
// WHY SkSL AND NOT GLSL/MSL/HLSL
// ------------------------------
// This one source string is compiled by Skia into GLSL ES for Android and
// WebGL, Metal for iOS and macOS, HLSL/D3D for Windows, SPIR-V for Vulkan,
// and WGSL for WebGPU. It is the concrete reason Appendix C puts "shaders" in
// the 85% of the codebase that is platform-agnostic rather than in the 15%
// that is not. A hand-written shader per backend would be five files that
// drift apart, and dark mode looking subtly different on Android would be a
// bug nobody files and everybody notices.

#pragma once

#include "folio/render/PageTheme.h"

#include "include/core/SkColorFilter.h"
#include "include/core/SkRefCnt.h"

namespace folio::render::skia {

/// The SkSL source. Exposed so a test can compile it and assert the error
/// string is empty - a shader that fails to compile at run time on one
/// platform is otherwise discovered by a user, in the dark, on a train.
[[nodiscard]] const char* pageThemeSkSL();

/// Build a colour filter for `theme`. Returns nullptr for the identity
/// (light) theme so the caller can skip the pass entirely.
///
/// Results are cached: there are exactly three themes and building a runtime
/// effect involves parsing and compiling SkSL, which is not something to do
/// on a 16 ms budget.
[[nodiscard]] sk_sp<SkColorFilter> makePageThemeFilter(const PageTheme& theme);

/// Compile the effect once, eagerly, and report whether it worked. Call at
/// start-up so a broken shader is a launch-time failure with a message
/// rather than a blank page later.
[[nodiscard]] bool validatePageThemeShader(const char** errorOut);

}  // namespace folio::render::skia
