#pragma once

// Section, key and option names in MGS4Enabler.settings. Shared by the ASI, which reads them,
// and the settings tool, which writes them. Change a string here and both sides follow.
namespace mgs4e::keys
{
    constexpr const char* Graphics = "Graphics";
    constexpr const char* Debugging = "Debugging";

    // [Graphics]
    constexpr const char* InternalResolutionScale = "Internal Resolution Scale (%)";

    constexpr const char* WindowAspectRatio = "Window Aspect Ratio";
    constexpr const char* WindowAspectRatio_Off = "Use Game Setting";
    constexpr const char* WindowAspectRatio_16_9 = "16:9";
    constexpr const char* WindowAspectRatio_16_10 = "16:10";
    constexpr const char* WindowAspectRatio_21_9 = "21:9";
    constexpr const char* WindowAspectRatio_32_9 = "32:9";
    constexpr const char* WindowAspectRatio_4_3 = "4:3";

    constexpr const char* WindowResolution16x9 = "Window Resolution (16:9)";
    constexpr const char* WindowResolution16x10 = "Window Resolution (16:10)";
    constexpr const char* WindowResolution21x9 = "Window Resolution (21:9)";
    constexpr const char* WindowResolution32x9 = "Window Resolution (32:9)";
    constexpr const char* WindowResolution4x3 = "Window Resolution (4:3)";

    constexpr const char* UltrawideHud = "Ultrawide HUD";
    constexpr const char* UltrawideHud_Stretched = "Stretched HUD";
    constexpr const char* UltrawideHud_Centered = "Centered HUD";
    constexpr const char* UltrawideHud_Expanded = "Expanded HUD";
    // The setting's first shape, a plain on/off, read only when the choice above is absent so
    // a settings file from that build keeps its meaning (on = Expanded HUD).
    constexpr const char* UltrawideHudFix = "Ultrawide HUD Fix";

    constexpr const char* SolidEyeOverlayFix = "Solid Eye Overlay Fix";
    constexpr const char* MenuMasking = "Menu Masking";
    constexpr const char* CutsceneMasking = "Cutscene Masking";
    constexpr const char* CutsceneFovCompensation = "Cutscene FOV Compensation";
    constexpr const char* FovAdjustment = "FOV Adjustment (%)";

    constexpr const char* ShadowResolutionScale = "Shadow Resolution Scale (%)";
    constexpr const char* ShadowSampleCount = "Shadow Softness (Samples)";
    constexpr const char* AnisotropicFiltering = "Anisotropic Filtering";

    constexpr const char* Fxaa = "FXAA";
    constexpr const char* FxaaQuality = "FXAA Quality";
    constexpr const char* FxaaQuality_Slow = "Slow (best quality)";
    constexpr const char* FxaaQuality_Medium = "Medium";
    constexpr const char* FxaaQuality_Fast = "Fast";

    // Undocumented, read but never written by the tool.
    constexpr const char* FixReticleTruncation = "Fix Reticle Truncation";
    constexpr const char* MaxInternalBufferWidth = "Max Internal Buffer Width";

    // [Debugging]
    constexpr const char* DebugLogging = "Debug Logging";
}
