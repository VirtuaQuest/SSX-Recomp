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

        HFONT MakeFont(const wchar_t* name, int height, int weight)
        {
            return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, name);
        }

        void InitFonts()
        {
            if (g_fonts[0] != nullptr)
                return;

            // Bahnschrift has been part of Windows since Windows 10, Impact and Segoe UI for longer.
            bool condensed = HasFont(L"Bahnschrift Condensed");
            g_fonts[int(Font::Title)] = condensed ? MakeFont(L"Bahnschrift Condensed", 54, FW_BOLD) : MakeFont(L"Impact", 48, FW_NORMAL);
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
        void DrawSpaced(HDC dc, const std::wstring& text, int x, int y, COLORREF color, int spacing)
        {
            SetTextColor(dc, color);
            for (wchar_t c : text)
            {
                TextOutW(dc, x, y, &c, 1);
                SIZE size;
                GetTextExtentPoint32W(dc, &c, 1, &size);
                x += size.cx + spacing;
            }
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

            bool filled = !disabled && (primary || hover || pressed);
            COLORREF fill = disabled ? FIELD : pressed ? GREEN : filled ? LIME : FIELD;
            COLORREF ink = disabled ? DARK_GREEN : filled ? BACKGROUND : LIME;
            FillSolid(dc, rect, fill);
            Outline(dc, rect, disabled ? DARK_GREEN : LIME);
            if (focused)
            {
                RECT inner{ rect.left + 2, rect.top + 2, rect.right - 2, rect.bottom - 2 };
                Outline(dc, inner, filled ? BACKGROUND : GREEN);
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
                Outline(dc, rect, focused || hover ? LIME : GREEN);

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
                    Line(dc, cx - 5, cy - 2 + i, cx, cy + 3 + i, LIME);
                    Line(dc, cx, cy + 3 + i, cx + 6, cy - 3 + i, LIME);
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
            FillSolid(item.hDC, item.rcItem, selected ? LIME : FIELD);
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
                Outline(memory, rect, GREEN);
                int value = int(GetWindowLongPtrW(window, GWLP_USERDATA));
                constexpr int SEGMENT = 11, GAP = 3;
                int inner = width - 8;
                int count = (inner + GAP) / (SEGMENT + GAP);
                int lit = value * count / 1000;
                for (int i = 0; i < count; i++)
                {
                    int x = 4 + i * (SEGMENT + GAP);
                    FillSolid(memory, { x, 4, x + SEGMENT, height - 4 }, i < lit ? LIME : DARK_GREEN);
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

        // A hexagon: lime ring, dark inside.
        const double middle = (size - 1) / 2.0, outer = size * 0.48, inner = size * 0.30;
        auto hexagon = [&](double x, double y) { return std::max(std::abs(y), std::abs(y) * 0.5 + std::abs(x) * 0.866); };
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                double distance = hexagon(x - middle, y - middle);
                uint32_t pixel = 0;
                if (distance <= outer)
                    pixel = distance >= inner ? 0xFFAAF028 : 0xFF050A06;
                pixels[y * size + x] = pixel;
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
        m_canvasBitmap = CreateCompatibleBitmap(dc, width, height);
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

        // A dim green glow from the top, darker towards the edges, with faint scan lines.
        for (int y = 0; y < m_height; y++)
        {
            for (int x = 0; x < m_width; x++)
            {
                double dx = (x - m_width * 0.5) / (m_width * 0.75);
                double dy = (y - m_height * 0.18) / (m_height * 0.85);
                double glow = std::exp(-(dx * dx + dy * dy) * 2.2);
                double scan = y % 3 == 0 ? 0.82 : 1.0;
                int r = int((5 + 7 * glow) * scan), g = int((10 + 36 * glow) * scan), b = int((6 + 11 * glow) * scan);
                pixels[y * m_width + x] = uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
            }
        }

        // A faint honeycomb, fading downwards.
        constexpr double SIDE = 26.0;
        const double rowStep = SIDE * 1.5, columnStep = SIDE * std::sqrt(3.0);
        for (int row = -1; row * rowStep < m_height + SIDE; row++)
        {
            double fade = std::max(0.25, 1.0 - row * rowStep / m_height);
            COLORREF color = RGB(int(9 * fade) + 6, int(32 * fade) + 12, int(14 * fade) + 7);
            for (int column = -1; column * columnStep < m_width + SIDE; column++)
            {
                double cx = column * columnStep + (row % 2 != 0 ? columnStep / 2 : 0.0);
                double cy = row * rowStep;
                int px[3], py[3];
                for (int i = 0; i < 3; i++)
                {
                    double angle = 3.14159265 / 6.0 * (i * 2 + 1 + 6);
                    px[i] = int(cx + SIDE * std::cos(angle));
                    py[i] = int(cy + SIDE * std::sin(angle));
                }
                Line(m_backgroundDc, px[0], py[0], px[1], py[1], color);
                Line(m_backgroundDc, px[1], py[1], px[2], py[2], color);
                Line(m_backgroundDc, int(cx), int(cy + SIDE), px[1], py[1], color);
            }
        }

        // The heading.
        SelectObject(m_backgroundDc, GetFont(Font::Title));
        SetBkMode(m_backgroundDc, TRANSPARENT);
        std::wstring upper = m_title;
        CharUpperW(upper.data());
        for (int i = 3; i >= 1; i--)
        {
            int shade = 80 - i * 18;
            COLORREF glow = RGB(shade / 3, shade + 10, shade / 3);
            DrawSpaced(m_backgroundDc, upper, 24 - i, 8, glow, 3);
            DrawSpaced(m_backgroundDc, upper, 24 + i, 8, glow, 3);
            DrawSpaced(m_backgroundDc, upper, 24, 8 - i, glow, 3);
            DrawSpaced(m_backgroundDc, upper, 24, 8 + i, glow, 3);
        }
        DrawSpaced(m_backgroundDc, upper, 24, 8, LIME, 3);

        RECT subtitle{ 26, 62, m_width - 24, 80 };
        DrawTextIn(m_backgroundDc, m_subtitle, subtitle, DIM, Font::Small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // A line under it, with a tick at the end.
        FillSolid(m_backgroundDc, { 24, HEADER_HEIGHT - 2, m_width - 24, HEADER_HEIGHT - 1 }, GREEN);
        FillSolid(m_backgroundDc, { 24, HEADER_HEIGHT - 3, 150, HEADER_HEIGHT }, LIME);
        FillSolid(m_backgroundDc, { m_width - 44, HEADER_HEIGHT - 3, m_width - 24, HEADER_HEIGHT }, LIME);
    }

    void Form::Render()
    {
        BitBlt(m_canvasDc, 0, 0, m_width, m_height, m_backgroundDc, 0, 0, SRCCOPY);

        for (const Section& section : m_sections)
        {
            const RECT& r = section.rect;
            // Corners and a heading, like a display's frame.
            constexpr int ARM = 12;
            SelectObject(m_canvasDc, GetFont(Font::Section));
            SIZE size;
            GetTextExtentPoint32W(m_canvasDc, section.title.c_str(), int(section.title.size()), &size);
            RECT titleRect{ r.left + 2, r.top, r.right, r.top + 22 };
            DrawTextIn(m_canvasDc, section.title, titleRect, LIME, Font::Section, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            int lineY = r.top + 23;
            FillSolid(m_canvasDc, { r.left, lineY, r.right, lineY + 1 }, DARK_GREEN);
            FillSolid(m_canvasDc, { r.left, lineY, r.left + std::min<int>(size.cx + 8, r.right - r.left), lineY + 1 }, GREEN);
            FillSolid(m_canvasDc, { r.right - ARM, lineY, r.right, lineY + 1 }, GREEN);
            FillSolid(m_canvasDc, { r.right - 1, lineY, r.right, lineY + ARM }, GREEN);
            FillSolid(m_canvasDc, { r.left, r.bottom - 1, r.left + ARM, r.bottom }, DARK_GREEN);
            FillSolid(m_canvasDc, { r.left, r.bottom - ARM, r.left + 1, r.bottom }, DARK_GREEN);
            FillSolid(m_canvasDc, { r.right - ARM, r.bottom - 1, r.right, r.bottom }, DARK_GREEN);
            FillSolid(m_canvasDc, { r.right - 1, r.bottom - ARM, r.right, r.bottom }, DARK_GREEN);
        }

        for (const RECT& field : m_fields)
        {
            FillSolid(m_canvasDc, field, FIELD);
            Outline(m_canvasDc, field, GREEN);
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
