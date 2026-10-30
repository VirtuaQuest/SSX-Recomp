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
        uint32_t antiAliasing = 0;   // Not implemented yet.
        uint32_t renderScale = 1;    // Internal resolution: 1 is the game's own (720p), 2 is 1440p, 3 is 2160p.
        bool shadows = true;         // Off: the shadow maps get no shadow casters, so nothing is shadowed.
    };

    extern VideoSettings g_videoSettings;
}
