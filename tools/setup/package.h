#pragma once

// Reading the player's own files: the executable from a disc image or game folder, and Xbox 360 content packages (LIVE, PIRS, CON).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace package
{
    constexpr uint32_t CONTENT_MARKETPLACE = 0x00000002; // DLC.

    // Lower case hexadecimal.
    std::string Sha1(const void* data, size_t size);

    // A file in the root of a game: a disc image's or a folder's. False with a reason if it can't be read.
    bool ReadGameFile(const std::filesystem::path& game, const char* name, std::vector<uint8_t>& out, std::wstring& error);

    struct Info
    {
        bool valid{};
        uint32_t titleId{}, contentType{};
        std::wstring name;
    };

    Info ReadInfo(const std::filesystem::path& path);

    // Unpacks a package into a folder, checking every block against the package's hashes.
    bool Extract(const std::filesystem::path& path, const std::filesystem::path& folder, std::wstring& error);
}
