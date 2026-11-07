#include "theme.h"
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

namespace theme
{
    namespace
    {
        constexpr int ITEM_HEIGHT = 24;
        constexpr int FIELD_HEIGHT = 28;
        constexpr int HEADER_HEIGHT = 84;
        constexpr double PI = 3.14159265358979;

        HFONT g_fonts[4];
        HBRUSH g_fieldBrush, g_backgroundBrush;

        int CALLBACK FontExists(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found)
        {
            *reinterpret_cast<bool*>(found) = true;
            return 0;
        }

        bool HasFont(const wchar_t* name)
        {
            LOGFONTW wanted{};
            wanted.lfCharSet = DEFAULT_CHARSET;
            wcsncpy_s(wanted.lfFaceName, name, _TRUNCATE);
            bool found = false;
            HDC dc = GetDC(nullptr);
            EnumFontFamiliesExW(dc, &wanted, FontExists, reinterpret_cast<LPARAM>(&found), 0);
            ReleaseDC(nullptr, dc);
            return found;
        }

        HFONT MakeFont(const wchar_t* name, int height, int weight, bool italic = false)
        {
            return CreateFontW(-height, 0, 0, 0, weight, italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, name);
        }

        void InitFonts()
        {
            if (g_fonts[0] != nullptr)
                return;

            // Bahnschrift has been part of Windows since Windows 10, Impact and Segoe UI for longer. The title leans forward, like speed.
            bool condensed = HasFont(L"Bahnschrift Condensed");
            g_fonts[int(Font::Title)] = condensed ? MakeFont(L"Bahnschrift Condensed", 54, FW_BOLD, true) : MakeFont(L"Impact", 48, FW_NORMAL, true);
            g_fonts[int(Font::Section)] = condensed ? MakeFont(L"Bahnschrift SemiBold Condensed", 18, FW_NORMAL) : MakeFont(L"Segoe UI Semibold", 15, FW_NORMAL);
            g_fonts[int(Font::Body)] = MakeFont(L"Segoe UI", 14, FW_NORMAL);
            g_fonts[int(Font::Small)] = MakeFont(L"Segoe UI", 12, FW_NORMAL);
            g_fieldBrush = CreateSolidBrush(FIELD);
            g_backgroundBrush = CreateSolidBrush(BACKGROUND);
        }

        HFONT GetFont(Font font)
        {
            return g_fonts[int(font)];
        }

        void FillSolid(HDC dc, const RECT& rect, COLORREF color)
        {
            SetDCBrushColor(dc, color);
            FillRect(dc, &rect, HBRUSH(GetStockObject(DC_BRUSH)));
        }

        void Line(HDC dc, int x0, int y0, int x1, int y1, COLORREF color)
        {
            SelectObject(dc, GetStockObject(DC_PEN));
            SetDCPenColor(dc, color);
            MoveToEx(dc, x0, y0, nullptr);
            LineTo(dc, x1, y1);
        }

        void Outline(HDC dc, const RECT& rect, COLORREF color)
        {
            FillSolid(dc, { rect.left, rect.top, rect.right, rect.top + 1 }, color);
            FillSolid(dc, { rect.left, rect.bottom - 1, rect.right, rect.bottom }, color);
            FillSolid(dc, { rect.left, rect.top, rect.left + 1, rect.bottom }, color);
            FillSolid(dc, { rect.right - 1, rect.top, rect.right, rect.bottom }, color);
        }

        void DrawTextIn(HDC dc, const std::wstring& text, RECT rect, COLORREF color, Font font, UINT format)
        {
            SelectObject(dc, GetFont(font));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, color);
            DrawTextW(dc, text.c_str(), int(text.size()), &rect, format);
        }

        // Upper case with a little air between the letters, as DrawText can't.
        int DrawSpaced(HDC dc, const std::wstring& text, int x, int y, COLORREF color, int spacing)
        {
            SetTextColor(dc, color);
            for (wchar_t c : text)
            {
                TextOutW(dc, x, y, &c, 1);
                SIZE size;
                GetTextExtentPoint32W(dc, &c, 1, &size);
                x += size.cx + spacing;
            }
            return x;
        }

        // ---- Pixels ----

        uint32_t Pack(double r, double g, double b)
        {
            auto channel = [](double v) { return uint32_t(std::clamp(v, 0.0, 255.0)); };
            return channel(r) << 16 | channel(g) << 8 | channel(b);
        }

        void Blend(uint32_t& pixel, double r, double g, double b, double alpha)
        {
            double pr = (pixel >> 16) & 255, pg = (pixel >> 8) & 255, pb = pixel & 255;
            pixel = Pack(pr + (r - pr) * alpha, pg + (g - pg) * alpha, pb + (b - pb) * alpha);
        }

        // A repeatable pseudo random number in [0, 1).
        double Hash(uint32_t n)
        {
            n = (n ^ 61) ^ (n >> 16);
            n *= 9;
            n ^= n >> 4;
            n *= 0x27D4EB2D;
            n ^= n >> 15;
            return (n & 0xFFFFFF) / double(0x1000000);
        }

        // A ridge line: a few octaves of smooth noise, sharpened into peaks.
        double Ridge(double x, uint32_t seed)
        {
            double height = 0.0, amplitude = 1.0, frequency = 1.0 / 260.0, total = 0.0;
            for (int octave = 0; octave < 5; octave++)
            {
                double position = x * frequency;
                int cell = int(std::floor(position));
                double t = position - cell;
                double a = Hash(uint32_t(cell) * 7919u + seed * 104729u + octave * 31u);
                double b = Hash(uint32_t(cell + 1) * 7919u + seed * 104729u + octave * 31u);
                double smooth = t * t * (3.0 - 2.0 * t);
                double value = a + (b - a) * smooth;
                value = 1.0 - std::abs(value * 2.0 - 1.0); // Folded: sharp crests.
                height += value * amplitude;
                total += amplitude;
                amplitude *= 0.5;
                frequency *= 2.1;
            }
            return height / total;
        }

        // ---- The button ----

        LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
        {
            if (message == WM_MOUSEMOVE && GetPropW(window, L"hover") == nullptr)
            {
                SetPropW(window, L"hover", HANDLE(1));
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window };
                TrackMouseEvent(&track);
                InvalidateRect(window, nullptr, FALSE);
            }
            else if (message == WM_MOUSELEAVE)
            {
                RemovePropW(window, L"hover");
                InvalidateRect(window, nullptr, FALSE);
            }
            else if (message == WM_NCDESTROY)
                RemovePropW(window, L"hover");
            return DefSubclassProc(window, message, wParam, lParam);
        }

        void DrawButton(const DRAWITEMSTRUCT& item)
        {
            HDC dc = item.hDC;
            RECT rect = item.rcItem;
            bool primary = GetPropW(item.hwndItem, L"primary") != nullptr;
            bool hover = GetPropW(item.hwndItem, L"hover") != nullptr;
            bool disabled = (item.itemState & ODS_DISABLED) != 0;
            bool pressed = (item.itemState & ODS_SELECTED) != 0;
            bool focused = (item.itemState & ODS_FOCUS) != 0;

            // Primary buttons are ice filled, the others outlined; hovering turns either magenta.
            COLORREF fill = disabled ? FIELD : pressed ? ICE_MID : hover ? MAGENTA : primary ? ICE : FIELD;
            COLORREF ink = disabled ? ICE_DARK : (hover || pressed || primary) ? BACKGROUND : ICE;
            if (hover && !pressed && !disabled)
                ink = TEXT;
            FillSolid(dc, rect, fill);
            Outline(dc, rect, disabled ? ICE_DARK : hover ? MAGENTA : ICE);
            // A slanted notch at the right, the one bit of decoration.
            if (!disabled)
            {
                for (int i = 0; i < 6; i++)
                    Line(dc, rect.right - 12 + i, rect.bottom - 1, rect.right - 6 + i, rect.top, hover ? TEXT : primary ? BACKGROUND : ICE_MID);
            }
            if (focused)
            {
                RECT inner{ rect.left + 2, rect.top + 2, rect.right - 2, rect.bottom - 2 };
                Outline(dc, inner, (hover || primary) ? BACKGROUND : ICE_MID);
            }

            wchar_t text[64];
            GetWindowTextW(item.hwndItem, text, int(std::size(text)));
            std::wstring upper = text;
            CharUpperW(upper.data());
            DrawTextIn(dc, upper, rect, ink, Font::Section, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // ---- The list of choices ----

        LRESULT CALLBACK ComboProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
        {
            switch (message)
            {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT paint;
                HDC dc = BeginPaint(window, &paint);
                RECT rect;
                GetClientRect(window, &rect);
                bool focused = GetFocus() == window || SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) != 0;
                bool hover = GetPropW(window, L"hover") != nullptr;

                FillSolid(dc, rect, FIELD);
                Outline(dc, rect, hover ? MAGENTA : focused ? ICE : ICE_MID);

                wchar_t text[128] = {};
                LRESULT selected = SendMessageW(window, CB_GETCURSEL, 0, 0);
                if (selected != CB_ERR && SendMessageW(window, CB_GETLBTEXTLEN, selected, 0) < LRESULT(std::size(text)))
                    SendMessageW(window, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(text));
                RECT textRect{ rect.left + 9, rect.top, rect.right - 24, rect.bottom };
                DrawTextIn(dc, text, textRect, TEXT, Font::Body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

                // The chevron.
                int cx = rect.right - 13, cy = (rect.top + rect.bottom) / 2;
                for (int i = 0; i < 2; i++)
                {
                    Line(dc, cx - 5, cy - 2 + i, cx, cy + 3 + i, hover ? MAGENTA : ICE);
                    Line(dc, cx, cy + 3 + i, cx + 6, cy - 3 + i, hover ? MAGENTA : ICE);
                }
                EndPaint(window, &paint);
                return 0;
            }
            case WM_MOUSEMOVE:
                if (GetPropW(window, L"hover") == nullptr)
                {
                    SetPropW(window, L"hover", HANDLE(1));
                    TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window };
                    TrackMouseEvent(&track);
                    InvalidateRect(window, nullptr, FALSE);
                }
                break;
            case WM_MOUSELEAVE:
                RemovePropW(window, L"hover");
                InvalidateRect(window, nullptr, FALSE);
                break;
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
            case CB_SETCURSEL:
                InvalidateRect(window, nullptr, FALSE);
                break;
            case WM_NCDESTROY:
                RemovePropW(window, L"hover");
                break;
            }
            return DefSubclassProc(window, message, wParam, lParam);
        }

        void DrawComboItem(const DRAWITEMSTRUCT& item)
        {
            if (item.itemID == UINT(-1))
                return;
            bool selected = (item.itemState & ODS_SELECTED) != 0;
            FillSolid(item.hDC, item.rcItem, selected ? ICE : FIELD);
            wchar_t text[128] = {};
            if (SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0) < LRESULT(std::size(text)))
                SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text));
            RECT rect = item.rcItem;
            rect.left += 9;
            DrawTextIn(item.hDC, text, rect, selected ? BACKGROUND : TEXT, Font::Body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        // ---- The progress bar ----

        LRESULT CALLBACK ProgressProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT paint;
                HDC dc = BeginPaint(window, &paint);
                RECT rect;
                GetClientRect(window, &rect);
                int width = rect.right, height = rect.bottom;
                HDC memory = CreateCompatibleDC(dc);
                HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
                HGDIOBJ old = SelectObject(memory, bitmap);

                FillSolid(memory, rect, FIELD);
                Outline(memory, rect, ICE_MID);
                int value = int(GetWindowLongPtrW(window, GWLP_USERDATA));
                constexpr int SEGMENT = 11, GAP = 3;
                int inner = width - 8;
                int count = (inner + GAP) / (SEGMENT + GAP);
                int lit = value * count / 1000;
                for (int i = 0; i < count; i++)
                {
                    // Lit segments run from ice to magenta, slanted like the buttons' notch.
                    double t = count > 1 ? double(i) / (count - 1) : 0.0;
                    COLORREF color = i < lit ? RGB(int(96 + (255 - 96) * t), int(232 + (64 - 232) * t), int(255 + (170 - 255) * t)) : ICE_DARK;
                    int x = 4 + i * (SEGMENT + GAP);
                    for (int row = 4; row < height - 4; row++)
                    {
                        int slant = (height - 4 - row) / 3;
                        FillSolid(memory, { x + slant, row, x + SEGMENT + slant - 2, row + 1 }, color);
                    }
                }
                BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
                SelectObject(memory, old);
                DeleteObject(bitmap);
                DeleteDC(memory);
                EndPaint(window, &paint);
                return 0;
            }
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }

        const wchar_t* ProgressClass()
        {
            static const wchar_t* name = []
                {
                    WNDCLASSW windowClass{};
                    windowClass.lpfnWndProc = ProgressProc;
                    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
                    windowClass.lpszClassName = L"ARThemeProgress";
                    RegisterClassW(&windowClass);
                    return L"ARThemeProgress";
                }();
            return name;
        }
    }

    void SetProgress(HWND progress, int value)
    {
        SetWindowLongPtrW(progress, GWLP_USERDATA, std::clamp(value, 0, 1000));
        InvalidateRect(progress, nullptr, FALSE);
    }

    HICON MakeIcon(int size)
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = -size;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits;
        HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        auto* pixels = static_cast<uint32_t*>(bits);

        // A round badge: dusk sky, a white peak with an ice edge, and a magenta rim. Drawn 4x4 supersampled.
        const double s = size;
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                double r = 0, g = 0, b = 0, a = 0;
                for (int sy = 0; sy < 4; sy++)
                {
                    for (int sx = 0; sx < 4; sx++)
                    {
                        double px = (x + (sx + 0.5) / 4) / s * 2 - 1, py = (y + (sy + 0.5) / 4) / s * 2 - 1;
                        double distance = std::sqrt(px * px + py * py);
                        if (distance > 0.96)
                            continue;
                        double cr, cg, cb;
                        if (distance > 0.82)
                        {
                            cr = 255; cg = 64; cb = 170; // Rim.
                        }
                        else
                        {
                            double t = (py + 1) / 2; // Sky: indigo to pink.
                            cr = 30 + 190 * t * t; cg = 22 + 60 * t; cb = 80 + 70 * t;
                            // The peak: a triangle from the bottom with its top at (0.05, -0.45).
                            double ridge = -0.45 + std::abs(px - 0.05) * 1.25;
                            if (py > ridge)
                            {
                                bool snow = py < ridge + 0.28 || px < 0.05 - (py + 0.45) * 0.2;
                                cr = snow ? 240 : 40; cg = snow ? 248 : 50; cb = snow ? 255 : 110;
                                if (py < ridge + 0.06)
                                {
                                    cr = 96; cg = 232; cb = 255; // Ice edge.
                                }
                            }
                        }
                        r += cr; g += cg; b += cb; a += 1;
                    }
                }
                if (a == 0)
                {
                    pixels[y * size + x] = 0;
                    continue;
                }
                double alpha = a / 16.0;
                // Premultiplied for the icon.
                pixels[y * size + x] = uint32_t(alpha * 255) << 24 | Pack(r / 16, g / 16, b / 16);
            }
        }

        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        ICONINFO iconInfo{ TRUE, 0, 0, mask, color };
        HICON icon = CreateIconIndirect(&iconInfo);
        DeleteObject(mask);
        DeleteObject(color);
        return icon;
    }

    void Form::Attach(HWND window, int width, int height, const wchar_t* title, const wchar_t* subtitle)
    {
        InitFonts();
        m_window = window;
        m_width = width;
        m_height = height;
        m_title = title;
        m_subtitle = subtitle;

        HDC dc = GetDC(window);
        m_backgroundDc = CreateCompatibleDC(dc);
        m_canvasDc = CreateCompatibleDC(dc);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits;
        m_canvasBitmap = CreateDIBSection(m_canvasDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        m_canvasPixels = static_cast<uint32_t*>(bits);
        SelectObject(m_canvasDc, m_canvasBitmap);
        ReleaseDC(window, dc);
        RenderBackground();
    }

    void Form::RenderBackground()
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = m_width;
        info.bmiHeader.biHeight = -m_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits;
        m_backgroundBitmap = CreateDIBSection(m_backgroundDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        SelectObject(m_backgroundDc, m_backgroundBitmap);
        auto* pixels = static_cast<uint32_t*>(bits);
        const int w = m_width, h = m_height;

        // The sky: deep indigo at the top, violet, then a pink and orange glow along the horizon at two thirds down.
        const double horizon = h * 0.68;
        for (int y = 0; y < h; y++)
        {
            double t = std::min(1.0, y / horizon);
            double r = 12 + 70 * t * t + 150 * std::pow(t, 6.0);
            double g = 10 + 22 * t + 60 * std::pow(t, 8.0);
            double b = 34 + 70 * t - 20 * std::pow(t, 6.0);
            for (int x = 0; x < w; x++)
            {
                // Warmer to the right, where the sun went down.
                double warm = std::exp(-std::pow((x - w * 0.78) / (w * 0.45), 2.0)) * std::pow(t, 4.0);
                pixels[y * w + x] = Pack(r + 60 * warm, g + 30 * warm, b - 10 * warm);
            }
        }

        // Stars, thinning towards the horizon.
        for (int i = 0; i < w * h / 900; i++)
        {
            int x = int(Hash(i * 3u + 1) * w), y = int(Hash(i * 3u + 2) * horizon * 0.8);
            double brightness = Hash(i * 3u + 3) * (1.0 - y / horizon);
            Blend(pixels[y * w + x], 255, 255, 255, brightness * 0.9);
        }

        // An aurora: two soft ribbons waving across the upper sky.
        for (int x = 0; x < w; x++)
        {
            for (int band = 0; band < 2; band++)
            {
                double center = h * (0.20 + band * 0.11) + std::sin(x / double(w) * PI * (2.2 + band) + band * 1.7) * h * 0.06;
                double thickness = h * (0.05 + 0.03 * std::sin(x * 0.013 + band));
                for (int y = std::max(0, int(center - thickness * 3)); y < std::min(h, int(center + thickness)); y++)
                {
                    double d = (y - center) / thickness;
                    double alpha = (d < 0 ? std::exp(d * 0.9) : std::exp(-d * d * 4)) * 0.16 * (0.6 + 0.4 * std::sin(x * 0.05 + band * 2.0));
                    if (band == 0)
                        Blend(pixels[y * w + x], 60, 255, 200, alpha);
                    else
                        Blend(pixels[y * w + x], 200, 90, 255, alpha * 0.8);
                }
            }
        }

        // Three ranges of mountains, far to near, the far ones lit pink from the horizon, the nearest dark with snow on top.
        struct Range { double base, height; uint32_t seed; double r, g, b; double snow; };
        const Range ranges[] = {
            { horizon + h * 0.02, h * 0.30, 11, 120, 80, 150, 0.35 },
            { horizon + h * 0.12, h * 0.30, 23, 62, 50, 112, 0.5 },
            { horizon + h * 0.26, h * 0.26, 37, 22, 24, 58, 0.7 },
        };
        for (const Range& range : ranges)
        {
            for (int x = 0; x < w; x++)
            {
                double ridge = Ridge(x + range.seed * 1000.0, range.seed);
                double top = range.base - range.height * ridge;
                // The slope over a stretch of the ridge, so faces are broad: positive where the ridge climbs to the right.
                double slope = (Ridge(x + 9 + range.seed * 1000.0, range.seed) - Ridge(x - 9 + range.seed * 1000.0, range.seed)) * 12.0;
                double lit = std::clamp(0.5 - slope, 0.15, 1.0); // Light from the left, where the sky is darker: faces turned left are lit.
                // The snow line follows the ridge's broad shape, not its finest wiggles, so it doesn't streak.
                double broad = 0.0;
                for (int k = -3; k <= 3; k++)
                    broad += Ridge(x + k * 8 + range.seed * 1000.0, range.seed);
                double capDepth = range.snow * (broad / 7.0) * 0.55;
                for (int y = std::max(0, int(top)); y < h; y++)
                {
                    double depth = (y - top) / range.height;
                    // Snow near the crests, thinning out downwards, brighter on the lit faces.
                    double snow = std::clamp((capDepth - depth) / 0.04, 0.0, 1.0) * (0.45 + 0.55 * lit);
                    double shade = (1.0 - std::min(depth, 1.0) * 0.35) * (0.8 + 0.2 * lit);
                    double r = range.r * shade, g = range.g * shade, b = range.b * shade;
                    r += (225 - r) * snow * 0.8;
                    g += (232 - g) * snow * 0.8;
                    b += (255 - b) * snow * 0.8;
                    pixels[y * w + x] = Pack(r, g, b);
                }
                // A thin icy rim along the crest.
                if (top >= 0 && top < h)
                    Blend(pixels[int(top) * w + x], 150, 240, 255, 0.5);
            }
        }

        // Falling snow over everything, larger flakes nearer.
        for (int i = 0; i < w * h / 700; i++)
        {
            int x = int(Hash(i * 5u + 101) * w), y = int(Hash(i * 5u + 102) * h);
            double size = Hash(i * 5u + 103);
            double alpha = 0.25 + 0.5 * Hash(i * 5u + 104);
            Blend(pixels[y * w + x], 255, 255, 255, alpha);
            if (size > 0.75)
            {
                if (x + 1 < w) Blend(pixels[y * w + x + 1], 255, 255, 255, alpha * 0.6);
                if (y + 1 < h) Blend(pixels[(y + 1) * w + x], 255, 255, 255, alpha * 0.6);
            }
        }

        // The header band: darker, so the heading stands out, with a slanted magenta stripe behind the title.
        for (int y = 0; y < HEADER_HEIGHT; y++)
            for (int x = 0; x < w; x++)
                Blend(pixels[y * w + x], 8, 8, 26, 0.45);

        SelectObject(m_backgroundDc, GetFont(Font::Title));
        SetBkMode(m_backgroundDc, TRANSPARENT);
        std::wstring upper = m_title;
        CharUpperW(upper.data());
        SIZE titleSize{};
        {
            int end = 0;
            HDC measure = m_backgroundDc;
            for (wchar_t c : upper)
            {
                SIZE size;
                GetTextExtentPoint32W(measure, &c, 1, &size);
                end += size.cx + 3;
            }
            titleSize.cx = end;
        }
        // The stripe: a parallelogram from behind the first letters past the end of the title, fading out.
        for (int y = 14; y < 66; y++)
        {
            int shift = (66 - y) / 2;
            for (int x = 16 + shift; x < std::min(w, int(40 + titleSize.cx + shift + 60)); x++)
            {
                double fade = 1.0 - std::clamp((x - (24 + titleSize.cx)) / 80.0, 0.0, 1.0);
                Blend(pixels[y * w + x], 255, 64, 170, 0.22 * fade);
            }
        }
        // Speed lines to the right of the title.
        for (int i = 0; i < 4; i++)
        {
            int y = 24 + i * 9;
            int start = 40 + titleSize.cx + i * 14;
            for (int x = start; x < std::min(w - 24, start + 120 - i * 20); x++)
                Blend(pixels[y * w + x], i % 2 ? 255 : 96, i % 2 ? 150 : 232, i % 2 ? 70 : 255, 0.7 * (1.0 - double(x - start) / (120 - i * 20)));
        }
        // The title: a magenta shadow, then white with an ice glow.
        DrawSpaced(m_backgroundDc, upper, 27, 11, MAGENTA, 3);
        for (int i = 2; i >= 1; i--)
        {
            COLORREF glow = RGB(30, 70 + i * 20, 120 + i * 20);
            DrawSpaced(m_backgroundDc, upper, 24 - i, 8, glow, 3);
            DrawSpaced(m_backgroundDc, upper, 24 + i, 8, glow, 3);
        }
        DrawSpaced(m_backgroundDc, upper, 24, 8, TEXT, 3);

        RECT subtitle{ 26, 62, m_width - 24, 80 };
        DrawTextIn(m_backgroundDc, m_subtitle, subtitle, DIM, Font::Small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // A line under it: ice, with a magenta and an orange tick.
        FillSolid(m_backgroundDc, { 24, HEADER_HEIGHT - 2, m_width - 24, HEADER_HEIGHT - 1 }, ICE_MID);
        FillSolid(m_backgroundDc, { 24, HEADER_HEIGHT - 3, 150, HEADER_HEIGHT }, ICE);
        FillSolid(m_backgroundDc, { m_width - 64, HEADER_HEIGHT - 3, m_width - 44, HEADER_HEIGHT }, MAGENTA);
        FillSolid(m_backgroundDc, { m_width - 44, HEADER_HEIGHT - 3, m_width - 24, HEADER_HEIGHT }, SUNSET);
    }

    void Form::Render()
    {
        BitBlt(m_canvasDc, 0, 0, m_width, m_height, m_backgroundDc, 0, 0, SRCCOPY);
        GdiFlush();

        for (const Section& section : m_sections)
        {
            const RECT& r = section.rect;
            // A dark glass panel under the section, so text reads well over the scenery.
            if (m_canvasPixels != nullptr)
            {
                for (int y = std::max<int>(0, r.top - 4); y < std::min<int>(m_height, r.bottom + 6); y++)
                    for (int x = std::max<int>(0, r.left - 8); x < std::min<int>(m_width, r.right + 8); x++)
                        Blend(m_canvasPixels[y * m_width + x], 8, 10, 28, 0.72);
            }

            // Corners and a heading, like a display's frame.
            constexpr int ARM = 12;
            SelectObject(m_canvasDc, GetFont(Font::Section));
            SIZE size;
            GetTextExtentPoint32W(m_canvasDc, section.title.c_str(), int(section.title.size()), &size);
            RECT titleRect{ r.left + 2, r.top, r.right, r.top + 22 };
            DrawTextIn(m_canvasDc, section.title, titleRect, ICE, Font::Section, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            int lineY = r.top + 23;
            FillSolid(m_canvasDc, { r.left, lineY, r.right, lineY + 1 }, ICE_DARK);
            FillSolid(m_canvasDc, { r.left, lineY, r.left + std::min<int>(size.cx + 8, r.right - r.left), lineY + 1 }, ICE);
            FillSolid(m_canvasDc, { r.left, lineY + 1, r.left + 24, lineY + 2 }, MAGENTA);
            FillSolid(m_canvasDc, { r.right - ARM, lineY, r.right, lineY + 1 }, ICE_MID);
            FillSolid(m_canvasDc, { r.right - 1, lineY, r.right, lineY + ARM }, ICE_MID);
            FillSolid(m_canvasDc, { r.left, r.bottom - 1, r.left + ARM, r.bottom }, ICE_DARK);
            FillSolid(m_canvasDc, { r.left, r.bottom - ARM, r.left + 1, r.bottom }, ICE_DARK);
            FillSolid(m_canvasDc, { r.right - ARM, r.bottom - 1, r.right, r.bottom }, ICE_DARK);
            FillSolid(m_canvasDc, { r.right - 1, r.bottom - ARM, r.right, r.bottom }, ICE_DARK);
        }

        for (const RECT& field : m_fields)
        {
            FillSolid(m_canvasDc, field, FIELD);
            Outline(m_canvasDc, field, ICE_MID);
        }

        for (const Label& label : m_labels)
            DrawTextIn(m_canvasDc, label.text, label.rect, label.color, label.font, label.format);

        m_stale = false;
    }

    void Form::Invalidate()
    {
        m_stale = true;
        if (m_window != nullptr)
            InvalidateRect(m_window, nullptr, FALSE);
    }

    int Form::AddLabel(int x, int y, int width, int height, const std::wstring& text, COLORREF color, Font font, UINT format)
    {
        m_labels.push_back({ { x, y, x + width, y + height }, text, color, font, format });
        Invalidate();
        return int(m_labels.size()) - 1;
    }

    void Form::SetLabel(int label, const std::wstring& text, COLORREF color)
    {
        Label& item = m_labels[label];
        if (item.text == text && item.color == color)
            return;
        item.text = text;
        item.color = color;
        Invalidate();
    }

    void Form::AddSection(int x, int y, int width, int height, const wchar_t* title)
    {
        std::wstring upper = title;
        CharUpperW(upper.data());
        m_sections.push_back({ { x, y, x + width, y + height }, upper });
        Invalidate();
    }

    HWND Form::AddCombo(int id, int x, int y, int width, const wchar_t* const* items, size_t count, int selected)
    {
        HWND combo = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
            x, y, width, 300, m_window, HMENU(INT_PTR(id)), nullptr, nullptr);
        SendMessageW(combo, WM_SETFONT, WPARAM(GetFont(Font::Body)), TRUE);
        for (size_t i = 0; i < count; i++)
            SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(items[i]));
        SendMessageW(combo, CB_SETITEMHEIGHT, WPARAM(-1), FIELD_HEIGHT - 6);
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, ITEM_HEIGHT);
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
        SetWindowSubclass(combo, ComboProc, 1, 0);
        return combo;
    }

    HWND Form::AddEdit(int id, int x, int y, int width, const std::wstring& text)
    {
        m_fields.push_back({ x, y, x + width, y + FIELD_HEIGHT });
        HWND edit = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            x + 8, y + 6, width - 14, FIELD_HEIGHT - 11, m_window, HMENU(INT_PTR(id)), nullptr, nullptr);
        SendMessageW(edit, WM_SETFONT, WPARAM(GetFont(Font::Body)), TRUE);
        Invalidate();
        return edit;
    }

    HWND Form::AddButton(int id, int x, int y, int width, int height, const wchar_t* text, bool primary)
    {
        HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            x, y, width, height, m_window, HMENU(INT_PTR(id)), nullptr, nullptr);
        if (primary)
            SetPropW(button, L"primary", HANDLE(1));
        SetWindowSubclass(button, ButtonProc, 1, 0);
        return button;
    }

    HWND Form::AddProgress(int id, int x, int y, int width, int height)
    {
        return CreateWindowExW(0, ProgressClass(), nullptr, WS_CHILD | WS_VISIBLE, x, y, width, height, m_window, HMENU(INT_PTR(id)), nullptr, nullptr);
    }

    HWND Form::AddLog(int id, int x, int y, int width, int height)
    {
        m_fields.push_back({ x, y, x + width, y + height });
        HWND log = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            x + 6, y + 4, width - 8, height - 8, m_window, HMENU(INT_PTR(id)), nullptr, nullptr);
        SendMessageW(log, WM_SETFONT, WPARAM(GetFont(Font::Small)), TRUE);
        Invalidate();
        return log;
    }

    bool Form::Handle(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
    {
        switch (message)
        {
        case WM_ERASEBKGND:
            result = 1;
            return true;

        case WM_PAINT:
        {
            if (m_stale)
                Render();
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(m_window, &paint);
            BitBlt(dc, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right - paint.rcPaint.left, paint.rcPaint.bottom - paint.rcPaint.top,
                m_canvasDc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
            EndPaint(m_window, &paint);
            result = 0;
            return true;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        {
            HDC dc = HDC(wParam);
            SetTextColor(dc, message == WM_CTLCOLORSTATIC ? DIM : TEXT);
            SetBkColor(dc, FIELD);
            result = LRESULT(g_fieldBrush);
            return true;
        }

        case WM_MEASUREITEM:
        {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (measure->CtlType != ODT_COMBOBOX)
                return false;
            measure->itemHeight = ITEM_HEIGHT;
            result = TRUE;
            return true;
        }

        case WM_DRAWITEM:
        {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_BUTTON)
                DrawButton(*item);
            else if (item->CtlType == ODT_COMBOBOX)
                DrawComboItem(*item);
            else
                return false;
            result = TRUE;
            return true;
        }
        }
        return false;
    }
}
