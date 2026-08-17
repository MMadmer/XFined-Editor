#include "RedImageTool/RedImage.hpp"

#include <Windows.h>

#include <ft2build.h>
#include FT_BITMAP_H
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr size_t CharacterCount = 256;
constexpr size_t FirstPrintableCharacter = 32;
constexpr uint32_t DefaultFontHeight = 24;

struct LibraryHandle
{
    FT_Library value{};

    ~LibraryHandle()
    {
        if (value)
            FT_Done_FreeType(value);
    }
};

struct FaceHandle
{
    FT_Face value{};

    ~FaceHandle()
    {
        if (value)
            FT_Done_Face(value);
    }
};

struct GlyphHandle
{
    FT_Glyph value{};

    ~GlyphHandle()
    {
        if (value)
            FT_Done_Glyph(value);
    }
};

struct GlyphPlacement
{
    float advance{};
    float offsetY{};
};

struct GlyphCoordinates
{
    float left{};
    float top{};
    float right{};
};

size_t NextPowerOfTwoStrict(size_t value)
{
    size_t result = 1;
    while (result <= value)
    {
        if (result > std::numeric_limits<size_t>::max() / 2)
            return 0;

        result <<= 1;
    }
    return result;
}

bool ReadFont(const std::filesystem::path& path, std::vector<FT_Byte>& bytes)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return false;

    const std::streamoff size = stream.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > static_cast<uint64_t>(std::numeric_limits<FT_Long>::max()))
        return false;

    bytes.resize(static_cast<size_t>(size));
    stream.seekg(0, std::ios::beg);
    return static_cast<bool>(
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)));
}

// Dead Air font slots are CP1251 bytes, while FreeType expects Unicode codepoints.
FT_ULong ByteToCodepoint(size_t slot)
{
    if (slot + 1 == CharacterCount)
        return L' ';

    const char encoded = static_cast<char>(slot);
    wchar_t decoded{};
    if (MultiByteToWideChar(1251, MB_ERR_INVALID_CHARS, &encoded, 1, &decoded, 1) == 1)
        return static_cast<FT_ULong>(decoded);

    return static_cast<FT_ULong>(static_cast<unsigned char>(encoded));
}

bool RasterizeGlyph(
    FT_Face face,
    size_t slot,
    bool shocFormat,
    RedImageTool::RedImage& image,
    GlyphPlacement& placement)
{
    if (FT_Load_Char(face, ByteToCodepoint(slot), FT_LOAD_TARGET_NORMAL | FT_LOAD_FORCE_AUTOHINT | FT_LOAD_RENDER))
        return false;

    GlyphHandle glyph;
    if (FT_Get_Glyph(face->glyph, &glyph.value))
        return false;

    if (FT_Glyph_To_Bitmap(&glyph.value, FT_RENDER_MODE_NORMAL, nullptr, true))
        return false;

    const auto* bitmapGlyph = reinterpret_cast<FT_BitmapGlyph>(glyph.value);
    const FT_Bitmap& bitmap = bitmapGlyph->bitmap;
    if (!bitmap.buffer || !bitmap.width || !bitmap.rows || bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
        return false;

    const auto format = shocFormat ? RedImageTool::RedTexturePixelFormat::R8G8B8A8
                                   : RedImageTool::RedTexturePixelFormat::R8;
    image.Create(bitmap.width, bitmap.rows, 1, 1, format);
    auto* pixels = static_cast<uint8_t*>(*image);

    for (uint32_t y = 0; y < bitmap.rows; ++y)
    {
        const auto* row = bitmap.buffer + static_cast<ptrdiff_t>(y) * bitmap.pitch;
        for (uint32_t x = 0; x < bitmap.width; ++x)
        {
            if (shocFormat)
            {
                const size_t index = 4ull * (x + static_cast<size_t>(y) * bitmap.width);
                pixels[index] = 255;
                pixels[index + 1] = 255;
                pixels[index + 2] = 255;
                pixels[index + 3] = row[x];
            }
            else
            {
                pixels[x + static_cast<size_t>(y) * bitmap.width] = row[x];
            }
        }
    }

    placement.advance = static_cast<float>(face->glyph->metrics.horiAdvance) / 64.0f;
    placement.offsetY = static_cast<float>(face->glyph->metrics.horiBearingY) / 64.0f;
    return true;
}

bool BuildAtlas(
    const std::filesystem::path& fontPath,
    uint32_t fontHeight,
    bool shocFormat,
    RedImageTool::RedImage& atlas,
    std::array<GlyphCoordinates, CharacterCount>& coordinates,
    size_t& realHeight)
{
    std::vector<FT_Byte> fontBytes;
    if (!ReadFont(fontPath, fontBytes))
        return false;

    LibraryHandle library;
    if (FT_Init_FreeType(&library.value))
        return false;

    FaceHandle face;
    if (FT_New_Memory_Face(
            library.value,
            fontBytes.data(),
            static_cast<FT_Long>(fontBytes.size()),
            0,
            &face.value))
    {
        return false;
    }

    if (FT_Select_Charmap(face.value, FT_ENCODING_UNICODE) || FT_Set_Pixel_Sizes(face.value, 0, fontHeight))
        return false;

    realHeight = static_cast<size_t>(face.value->size->metrics.height >> 6);
    if (!realHeight)
        return false;

    std::array<RedImageTool::RedImage, CharacterCount> glyphImages;
    std::array<GlyphPlacement, CharacterCount> placements{};
    for (size_t slot = FirstPrintableCharacter; slot < CharacterCount; ++slot)
        RasterizeGlyph(face.value, slot, shocFormat, glyphImages[slot], placements[slot]);

    const int64_t baselineShift = static_cast<int64_t>(realHeight) - fontHeight;
    for (size_t slot = 0; slot < CharacterCount; ++slot)
    {
        placements[slot].offsetY = static_cast<float>(
            static_cast<int64_t>(fontHeight) -
            (static_cast<int64_t>(placements[slot].offsetY) + baselineShift));
    }

    const auto areaEstimate = static_cast<size_t>(std::sqrt(
        static_cast<double>((CharacterCount - FirstPrintableCharacter) * fontHeight * realHeight)));
    const size_t textureSize = NextPowerOfTwoStrict(areaEstimate);
    if (!textureSize)
        return false;

    atlas.Create(textureSize, textureSize, 1, 1, RedImageTool::RedTexturePixelFormat::R8G8B8A8);
    atlas.Fill(RedImageTool::RedColor(shocFormat ? 0u : 0xff000000u));

    size_t x = 0;
    size_t y = 0;
    for (size_t slot = FirstPrintableCharacter; slot < CharacterCount; ++slot)
    {
        const auto& glyph = glyphImages[slot];
        if (glyph.Empty())
            continue;

        if (x + glyph.GetWidth() + 4 > textureSize)
        {
            x = 0;
            y += realHeight;
        }

        const size_t offsetY = placements[slot].offsetY > 0.0f
            ? static_cast<size_t>(placements[slot].offsetY)
            : 0;
        if (y + offsetY + glyph.GetHeight() > textureSize)
            return false;

        atlas.Append(x, y + offsetY, glyph, 0, 0);
        coordinates[slot] = {
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(x) + placements[slot].advance};
        x += glyph.GetWidth() + 4;
    }

    atlas.GenerateMipmap();
    if (shocFormat)
        atlas.Convert(RedImageTool::RedTexturePixelFormat::BC3);

    return true;
}

std::filesystem::path OutputPath(
    const std::filesystem::path& fontPath,
    uint32_t resolution,
    std::wstring_view extension)
{
    auto result = fontPath;
    result.replace_extension();
    result += L"_" + std::to_wstring(resolution) + std::wstring(extension);
    return result;
}

bool WriteCoordinates(
    const std::filesystem::path& path,
    size_t realHeight,
    const std::array<GlyphCoordinates, CharacterCount>& coordinates)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return false;

    stream << "[symbol_coords]\nheight=" << realHeight << '\n' << std::fixed << std::setprecision(6);
    for (size_t slot = 0; slot < CharacterCount; ++slot)
    {
        stream << std::setfill('0') << std::setw(3) << slot << " = "
               << coordinates[slot].left << ','
               << coordinates[slot].top - 1.0f << ','
               << coordinates[slot].right << '\n';
    }
    return static_cast<bool>(stream);
}

bool GenerateFont(const std::filesystem::path& fontPath, uint32_t fontHeight, bool shocFormat)
{
    RedImageTool::RedImage atlas;
    std::array<GlyphCoordinates, CharacterCount> coordinates{};
    size_t realHeight{};
    if (!BuildAtlas(fontPath, fontHeight, shocFormat, atlas, coordinates, realHeight))
        return false;

    for (const uint32_t resolution : {800u, 1024u, 1600u})
    {
        const auto ddsPath = OutputPath(fontPath, resolution, L".dds");
        const auto iniPath = OutputPath(fontPath, resolution, L".ini");
        const std::string ddsName = ddsPath.string();
        if (!atlas.SaveToDds(ddsName.c_str()) || !WriteCoordinates(iniPath, realHeight, coordinates))
            return false;
    }
    return true;
}

bool PrintFreeTypeVersion()
{
    LibraryHandle library;
    if (FT_Init_FreeType(&library.value))
        return false;

    FT_Int major{};
    FT_Int minor{};
    FT_Int patch{};
    FT_Library_Version(library.value, &major, &minor, &patch);
    std::cout << "FreeType " << major << '.' << minor << '.' << patch << '\n';
    return true;
}

void PrintUsage()
{
    std::wcout << L"Usage: XrFontGenerate [-height N] [-shoc] <font-file>\n"
                  L"       XrFontGenerate --version\n";
}

bool ParseHeight(const wchar_t* text, uint32_t& height)
{
    wchar_t* end{};
    errno = 0;
    const unsigned long parsed = std::wcstoul(text, &end, 10);
    if (errno || !end || *end || parsed > 4096)
        return false;

    height = std::max<uint32_t>(8, static_cast<uint32_t>(parsed));
    return true;
}
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc == 2 && std::wstring_view(argv[1]) == L"--version")
        return PrintFreeTypeVersion() ? 0 : 1;

    if (argc == 2 && (std::wstring_view(argv[1]) == L"--help" || std::wstring_view(argv[1]) == L"-h"))
    {
        PrintUsage();
        return 0;
    }

    if (argc < 2)
    {
        PrintUsage();
        return 2;
    }

    uint32_t fontHeight = DefaultFontHeight;
    bool shocFormat = false;
    for (int index = 1; index < argc - 1; ++index)
    {
        const std::wstring_view argument(argv[index]);
        if (argument == L"-shoc")
        {
            shocFormat = true;
        }
        else if (argument == L"-height" && index + 1 < argc - 1)
        {
            if (!ParseHeight(argv[++index], fontHeight))
            {
                std::wcerr << L"Invalid font height.\n";
                return 2;
            }
        }
        else
        {
            std::wcerr << L"Unknown option: " << argument << '\n';
            return 2;
        }
    }

    const std::filesystem::path fontPath(argv[argc - 1]);
    if (!std::filesystem::is_regular_file(fontPath))
    {
        std::wcerr << L"Font file not found: " << fontPath.wstring() << '\n';
        return 1;
    }

    if (!GenerateFont(fontPath, fontHeight, shocFormat))
    {
        std::wcerr << L"Font generation failed: " << fontPath.wstring() << '\n';
        return 1;
    }

    return 0;
}
