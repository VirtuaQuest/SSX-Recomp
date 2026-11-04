#pragma once

// Display options chosen at launch (command line, usually from the launcher).
namespace gpu
{
    enum class DisplayMode
    {
        Windowed,
        Borderless,  // A borderless window covering the monitor; the image is rendered at the chosen size and stretched.
        Fullscreen,  // Exclusive fullscreen at the chosen resolution.
    };

    struct VideoSettings
    {
        uint32_t width = 1280;
        uint32_t height = 720;
        DisplayMode mode = DisplayMode::Windowed;
        uint32_t frameRate = 30;     // Target frame rate; the game runs at 30 on the console.
        uint32_t antiAliasing = 0;   // 0 off, 1 FXAA (on the finished picture). Rendering at a higher scale is the other way.
        uint32_t anisotropy = 0;     // 0 what the title asks for; 2, 4, 8, 16 for all filtered textures with mipmaps.
        float textureLodBias = 0.0f; // Below 0 sharper textures, at a distance too (some shimmer at -1).
        uint32_t sharpen = 0;        // 0 off, 1-3 contrast adaptive sharpening.
        bool ultrawide = false;      // A wider view for a resolution wider than 16:9, rather than bars at the sides.
        bool vsync = false;          // Waits for the display's refresh: no tearing, some latency.
        uint32_t renderScale = 1;    // Internal resolution: 1 is the game's own (720p), 2 is 1440p, 3 is 2160p.
        bool shadows = true;         // Off: the shadow maps get no shadow casters, so nothing is shadowed.
    };

    extern VideoSettings g_videoSettings;

    // How much wider than 16:9 the setting makes the view (1: not).
    inline float UltrawideSetting()
    {
        if (!g_videoSettings.ultrawide || g_videoSettings.height == 0)
            return 1.0f;
        float factor = (float(g_videoSettings.width) / float(g_videoSettings.height)) / (16.0f / 9.0f);
        return factor > 1.01f ? factor : 1.0f;
    }

    // How much wider than 16:9 the picture is shown now: only while the title is building a 3D camera's view, or movies and 2D screens would be stretched
    // (they stay 16:9 with bars). See sub_82703DF8 in draw_hooks.cpp.
    float UltrawideFactor();
}
