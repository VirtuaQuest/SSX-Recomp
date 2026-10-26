#include <stdafx.h>
#include "guest_format.h"
#include "memory.h"

uint64_t GuestRegisterArgs::Next()
{
    uint32_t index = m_index++;
    if (index < 8)
    {
        const PPCRegister* registers[] = { &m_ctx.r3, &m_ctx.r4, &m_ctx.r5, &m_ctx.r6, &m_ctx.r7, &m_ctx.r8, &m_ctx.r9, &m_ctx.r10 };
        return registers[index]->u64;
    }

    return *reinterpret_cast<be<uint64_t>*>(m_base + m_ctx.r1.u32 + 0x54 + (index - 8) * 8 - 4);
}

uint64_t GuestVaListArgs::Next()
{
    uint64_t value = *g_memory.Translate<be<uint64_t>>(m_address);
    m_address += 8;
    return value;
}

std::string FormatGuestString(const char* format, GuestArgs& args)
{
    std::string result;
    if (format == nullptr)
        return result;

    for (const char* p = format; *p != '\0'; p++)
    {
        if (*p != '%')
        {
            result += *p;
            continue;
        }

        // Collect the conversion specification: flags, width, precision, length.
        std::string spec = "%";
        p++;
        while (*p != '\0' && strchr("-+ #0", *p))
            spec += *p++;

        if (*p == '*')
        {
            spec += std::to_string(int32_t(args.Next()));
            p++;
        }
        while (*p >= '0' && *p <= '9')
            spec += *p++;

        if (*p == '.')
        {
            spec += *p++;
            if (*p == '*')
            {
                spec += std::to_string(int32_t(args.Next()));
                p++;
            }
            while (*p >= '0' && *p <= '9')
                spec += *p++;
        }

        bool longLong = false;
        bool wide = false;
        bool half = false;
        while (*p != '\0' && strchr("hlLIqjzt", *p))
        {
            if (*p == 'l' && p[1] == 'l') { longLong = true; p += 2; continue; }
            if (*p == 'I' && p[1] == '6' && p[2] == '4') { longLong = true; p += 3; continue; }
            if (*p == 'I' && p[1] == '3' && p[2] == '2') { p += 3; continue; }
            if (*p == 'q' || *p == 'L') longLong = true;
            if (*p == 'l' || *p == 'w') wide = true;
            if (*p == 'h') half = true;
            p++;
        }

        char conversion = *p;
        if (conversion == '\0')
            break;

        char buffer[512];
        switch (conversion)
        {
        case '%':
            result += '%';
            break;

        case 'd':
        case 'i':
        {
            int64_t value = longLong ? int64_t(args.Next()) : int64_t(int32_t(args.Next()));
            snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), (long long)value);
            result += buffer;
            break;
        }

        case 'u':
        case 'x':
        case 'X':
        case 'o':
        {
            uint64_t value = longLong ? args.Next() : uint64_t(uint32_t(args.Next()));
            snprintf(buffer, sizeof(buffer), (spec + "ll" + conversion).c_str(), (unsigned long long)value);
            result += buffer;
            break;
        }

        case 'p':
            snprintf(buffer, sizeof(buffer), "%08X", uint32_t(args.Next()));
            result += buffer;
            break;

        case 'c':
        case 'C':
            result += char(args.Next());
            break;

        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
        {
            uint64_t raw = args.Next();
            double value;
            memcpy(&value, &raw, sizeof(value));
            snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), value);
            result += buffer;
            break;
        }

        case 's':
        case 'S':
        {
            uint32_t address = uint32_t(args.Next());
            bool wideString = (conversion == 'S') != wide && !half;
            std::string text;
            if (address == 0)
            {
                text = "(null)";
            }
            else if (wideString)
            {
                for (auto* c = g_memory.Translate<be<uint16_t>>(address); c->get() != 0; c++)
                    text += char(c->get() < 0x80 ? c->get() : '?');
            }
            else
            {
                text = g_memory.Translate<const char>(address);
            }

            snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), text.c_str());
            result += text.size() < sizeof(buffer) - 1 ? std::string(buffer) : text;
            break;
        }

        case 'n':
            args.Next();
            break;

        default:
            result += spec + conversion;
            break;
        }
    }

    return result;
}
