#pragma once

// printf style formatting with arguments taken from guest code.
//
// Xbox 360 variadic arguments occupy 8 byte slots: the first eight in r3-r10, the rest in the caller's parameter
// area at r1 + 0x54 (continuing r1 + 0x14 + 8 * index). A guest va_list points at an array of such slots, with
// 32 bit values in the low half. Floating point varargs are passed as raw 64 bit doubles in the same slots.
class GuestArgs
{
public:
    virtual ~GuestArgs() = default;
    virtual uint64_t Next() = 0;
};

// Arguments following a fixed number of named parameters of the current guest call.
class GuestRegisterArgs : public GuestArgs
{
public:
    GuestRegisterArgs(const PPCContext& ctx, uint8_t* base, uint32_t firstIndex)
        : m_ctx(ctx), m_base(base), m_index(firstIndex)
    {
    }

    uint64_t Next() override;

private:
    const PPCContext& m_ctx;
    uint8_t* m_base;
    uint32_t m_index;
};

// Arguments from a guest va_list.
class GuestVaListArgs : public GuestArgs
{
public:
    explicit GuestVaListArgs(uint32_t vaList) : m_address(vaList) {}

    uint64_t Next() override;

private:
    uint32_t m_address;
};

std::string FormatGuestString(const char* format, GuestArgs& args);
