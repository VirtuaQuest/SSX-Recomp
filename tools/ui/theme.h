#pragma once

// The look shared by the launcher and setup: a winter night over the mountains, icy cyan lines and hot magenta accents. Everything
// is drawn here, from code and the fonts Windows has, so these programs carry no images or fonts of anyone's (none of the game's art
// either: the sky, ridges, snow and emblem are made up in code).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace theme
{
    constexpr COLORREF BACKGROUND = RGB(10, 10, 30);
    constexpr COLORREF FIELD = RGB(14, 18, 44);
    constexpr COLORREF ICE = RGB(96, 232, 255);     // The main accent: lines, highlights, the primary button.
    constexpr COLORREF ICE_MID = RGB(46, 140, 196);
    constexpr COLORREF ICE_DARK = RGB(30, 52, 98);
    constexpr COLORREF MAGENTA = RGB(255, 64, 170);  // Hover and the second accent.
    constexpr COLORREF SUNSET = RGB(255, 150, 70);
    constexpr COLORREF TEXT = RGB(236, 244, 255);
    constexpr COLORREF DIM = RGB(150, 170, 210);
    constexpr COLORREF BAD = RGB(255, 98, 110);
    constexpr COLORREF WARNING = RGB(255, 205, 70);

    enum class Font
    {
        Title,   // The big heading.
        Section, // Headings of sections and buttons.
        Body,
        Small,
    };

    // A small icon made from code, for programs with none of their own.
    HICON MakeIcon(int size);

    // One window's drawing and its controls. The window passes its messages to Handle first.
    class Form
    {
    public:
        void Attach(HWND window, int width, int height, const wchar_t* title, const wchar_t* subtitle);
        bool Handle(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

        // Text drawn on the window. Returns what SetLabel takes.
        int AddLabel(int x, int y, int width, int height, const std::wstring& text, COLORREF color = TEXT, Font font = Font::Body, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        void SetLabel(int label, const std::wstring& text, COLORREF color);
        void AddSection(int x, int y, int width, int height, const wchar_t* title);

        HWND AddCombo(int id, int x, int y, int width, const wchar_t* const* items, size_t count, int selected);
        HWND AddEdit(int id, int x, int y, int width, const std::wstring& text);
        HWND AddButton(int id, int x, int y, int width, int height, const wchar_t* text, bool primary = false);
        HWND AddProgress(int id, int x, int y, int width, int height);
        HWND AddLog(int id, int x, int y, int width, int height);

    private:
        struct Label
        {
            RECT rect;
            std::wstring text;
            COLORREF color;
            Font font;
            UINT format;
        };

        struct Section
        {
            RECT rect;
            std::wstring title;
        };

        void RenderBackground();
        void Render();
        void Invalidate();

        HWND m_window{};
        int m_width{}, m_height{};
        std::wstring m_title, m_subtitle;
        std::vector<Label> m_labels;
        std::vector<Section> m_sections;
        std::vector<RECT> m_fields;
        HDC m_backgroundDc{}, m_canvasDc{};
        HBITMAP m_backgroundBitmap{}, m_canvasBitmap{};
        uint32_t* m_canvasPixels{};
        bool m_stale{ true };
    };

    // The progress control: 0 to 1000.
    void SetProgress(HWND progress, int value);
}
