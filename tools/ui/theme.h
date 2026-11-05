#pragma once

// The look shared by the launcher and setup: near black with lime lines and text. Everything is drawn here, from code and the fonts Windows has, so these
// programs carry no images or fonts of anyone's.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

namespace theme
{
    constexpr COLORREF BACKGROUND = RGB(5, 10, 6);
    constexpr COLORREF FIELD = RGB(9, 22, 11);
    constexpr COLORREF LIME = RGB(170, 240, 40);
    constexpr COLORREF GREEN = RGB(58, 150, 58);
    constexpr COLORREF DARK_GREEN = RGB(24, 66, 28);
    constexpr COLORREF TEXT = RGB(220, 240, 208);
    constexpr COLORREF DIM = RGB(120, 160, 110);
    constexpr COLORREF BAD = RGB(255, 112, 84);
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
        bool m_stale{ true };
    };

    // The progress control: 0 to 1000.
    void SetProgress(HWND progress, int value);
}
