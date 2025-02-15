//
//  ZEPTO-8 — Fantasy console emulator
//
//  Copyright © 2016—2020 Sam Hocevar <sam@hocevar.net>
//
//  This program is free software. It comes without any warranty, to
//  the extent permitted by applicable law. You can redistribute it
//  and/or modify it under the terms of the Do What the Fuck You Want
//  to Public License, Version 2, as published by the WTFPL Task Force.
//  See http://www.wtfpl.net/ for more details.
//

#if HAVE_CONFIG_H
#   include "config.h"
#endif

#include <fstream>   // std::ofstream
#include <lol/file>  // lol::file
#include <lol/msg>   // lol::msg
#include <lol/utils> // lol::ends_with
#include <lol/pegtl> // pegtl::*
#include <regex>     // std::regex_replace

#include <lol/sys/init.h> // lol::sys::get_data_path

#include "3rdparty/lodepng/lodepng.h"
extern "C" {
#include "3rdparty/quickjs/quickjs.h"
}

#include "zepto8.h"
#include "pico8/cart.h"
#include "pico8/pico8.h"

namespace z8::pico8
{

using lol::ivec2;
using lol::msg;
using lol::u8vec4;
using lol::PixelFormat;

using namespace tao;

bool cart::load(std::string const &filename)
{
    if (lol::ends_with(lol::tolower(filename), ".p8") && load_p8(filename))
        return true;

    if (lol::ends_with(lol::tolower(filename), ".lua") && load_lua(filename))
        return true;

    if (lol::ends_with(lol::tolower(filename), ".png") && load_png(filename))
        return true;

    if (lol::ends_with(lol::tolower(filename), ".js") && load_js(filename))
        return true;

    return false;
}

bool cart::load_png(std::string const &filename)
{
    // Open cartridge as PNG image
    std::vector<uint8_t> image;
    unsigned int width, height;
    unsigned int error = lodepng::decode(image, width, height, filename);

    if (error)
        return false;

    if (width * height != 160 * 205)
        return false;

    u8vec4 const *pixels = (u8vec4 const *)image.data();

    // Retrieve cartridge data from lower image bits
    std::vector<uint8_t> bytes(sizeof(m_rom) + 5);
    for (int n = 0; n < (int)bytes.size(); ++n)
    {
        u8vec4 p = pixels[n] * 64;
        bytes[n] = p.a + p.r / 4 + p.g / 16 + p.b / 64;
    }

    // Retrieve label from image pixels
    if (width >= LABEL_WIDTH + LABEL_X && height >= LABEL_HEIGHT + LABEL_Y)
    {
        m_label.resize(LABEL_WIDTH * LABEL_HEIGHT);
        for (int y = 0; y < LABEL_HEIGHT; ++y)
        for (int x = 0; x < LABEL_WIDTH; ++x)
        {
            lol::u8vec4 p = pixels[(y + LABEL_Y) * width + (x + LABEL_X)];
            m_label[y * LABEL_WIDTH + x] = palette::best(p, 32);
        }
    }

    set_bin(bytes);
    return true;
}

bool cart::load_lua(std::string const &filename)
{
    // Read file
    std::string code;
    if (!lol::file::read(lol::sys::get_data_path(filename), code))
        return false;

    // Remove CRLF for internal consistency
    code = std::regex_replace(code, std::regex("\r\n"), "\n");

    // PICO-8 saves some symbols in the .p8 file as Emoji/Unicode characters
    // but the runtime expects 8-bit characters instead.
    m_code = charset::utf8_to_pico8(code);
    memset(&m_rom, 0, sizeof(m_rom));
    return true;
}

bool cart::load_js(std::string const &filename)
{
    // Read file
    std::string code;
    if (!lol::file::read(lol::sys::get_data_path(filename), code))
        return false;

    // Find cart data
    auto start = code.find("[", code.find("var _cartdat="));
    auto end = code.find("]", start);

    if (start == std::string::npos || end == std::string::npos)
        return false;

    auto rt = JS_NewRuntime();
    auto ctx = JS_NewContext(rt);

    // The QuickJS API says this must be zero terminated
    std::vector<uint8_t> bytes(sizeof(m_rom) + 5);
    bool success = false;
    *(code.data() + end + 1) = '\0';
    auto bin = JS_ParseJSON(ctx, code.c_str() + start, end + 1 - start, filename.c_str());
    if (!JS_IsException(bin) && JS_IsArray(ctx, bin))
    {
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            uint32_t x;
            JSValue val = JS_GetPropertyUint32(ctx, bin, uint32_t(i));
            if (JS_IsUndefined(val) || JS_ToUint32(ctx, &x, val))
                break;
            bytes[i] = uint8_t(x);
            JS_FreeValue(ctx, val);
        }
        set_bin(bytes);
        success = true;
    }
    JS_FreeValue(ctx, bin);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    return success;
}

//
// Directly load a binary file to the cart memory
//

void cart::set_bin(std::vector<uint8_t> const &bytes)
{
    memcpy(&m_rom, bytes.data(), sizeof(m_rom));
    uint8_t const *vbytes = bytes.data() + sizeof(m_rom);
    int version = vbytes[0];
    int minor = (vbytes[1] << 24) | (vbytes[2] << 16) | (vbytes[3] << 8) | vbytes[4];

    // Retrieve code, with optional decompression
    m_code = code::decompress(m_rom.code().data());

    msg::debug("version: %d.%d code: %d chars\n", version, minor, (int)m_code.length());

    // Invalidate code cache
    m_lua.resize(0);
}

//
// A special parser object for the .p8 format
//

struct p8_reader
{
    //
    // Grammar rules
    //

    struct r_lua : TAO_PEGTL_STRING("__lua__") {};
    struct r_gfx : TAO_PEGTL_STRING("__gfx__") {};
    struct r_gff : TAO_PEGTL_STRING("__gff__") {};
    struct r_map : TAO_PEGTL_STRING("__map__") {};
    struct r_sfx : TAO_PEGTL_STRING("__sfx__") {};
    struct r_mus : TAO_PEGTL_STRING("__music__") {};
    struct r_lab : TAO_PEGTL_STRING("__label__") {};
    struct r_any : pegtl::seq<pegtl::two<'_'>, pegtl::plus<pegtl::alnum>, pegtl::two<'_'>> {};

    struct r_section_name : pegtl::sor<r_lua,
                                       r_gfx,
                                       r_gff,
                                       r_map,
                                       r_sfx,
                                       r_mus,
                                       r_lab,
                                       r_any> {};
    struct r_section_line : pegtl::seq<r_section_name, pegtl::eolf> {};

    struct r_data_line : pegtl::until<pegtl::eolf> {};
    struct r_data : pegtl::star<pegtl::not_at<r_section_line>,
                                pegtl::not_at<pegtl::eof>,
                                r_data_line> {};

    struct r_section : pegtl::seq<r_section_line, r_data> {};
    struct r_version : pegtl::star<pegtl::digit> {};

    struct r_header: pegtl::seq<TAO_PEGTL_STRING("pico-8 cartridge"), pegtl::until<pegtl::eol>,
                                TAO_PEGTL_STRING("version "), r_version, pegtl::until<pegtl::eol>> {};
    struct r_file : pegtl::seq<pegtl::opt<pegtl::utf8::bom>,
                               r_header,
                               r_data, /* data before the first section is ignored */
                               pegtl::star<r_section>,
                               pegtl::eof> {};

    //
    // Grammar actions
    //

    template<typename R> struct action {};

    //
    // Parser state
    //

    int m_version = -1;

    enum class section : int8_t
    {
        error = -1,
        header = 0,
        lua,
        gfx,
        gff,
        map,
        sfx,
        mus,
        lab,
    };

    section m_current_section;
    std::map<int8_t, std::vector<uint8_t>> m_sections;
    std::string m_code;

    //
    // Actual reader
    //

    void parse(std::string const &str)
    {
        pegtl::string_input<> in(str, "p8");
        pegtl::parse<r_file, action>(in, *this);
    }

};

template<>
struct p8_reader::action<p8_reader::r_version>
{
    template<typename Input>
    static void apply(Input const &in, p8_reader &r)
    {
        r.m_version = std::atoi(in.string().c_str());
    }
};

template<>
struct p8_reader::action<p8_reader::r_section_name>
{
    template<typename Input>
    static void apply(Input const &in, p8_reader &r)
    {
        if (in.string().find("lua") != std::string::npos)
            r.m_current_section = section::lua;
        else if (in.string().find("gfx") != std::string::npos)
            r.m_current_section = section::gfx;
        else if (in.string().find("gff") != std::string::npos)
            r.m_current_section = section::gff;
        else if (in.string().find("map") != std::string::npos)
            r.m_current_section = section::map;
        else if (in.string().find("sfx") != std::string::npos)
            r.m_current_section = section::sfx;
        else if (in.string().find("music") != std::string::npos)
            r.m_current_section = section::mus;
        else if (in.string().find("label") != std::string::npos)
            r.m_current_section = section::lab;
        else
        {
            msg::info("unknown section name %s\n", in.string().c_str());
            r.m_current_section = section::error;
        }
    }
};

template<>
struct p8_reader::action<p8_reader::r_data>
{
    template<typename Input>
    static void apply(Input const &in, p8_reader &r)
    {
        if (r.m_current_section == section::lua)
        {
            // Copy the code but remove CRLF for internal consistency
            r.m_code += std::regex_replace(in.string(), std::regex("\r\n"), "\n");
        }
        else
        {
            // Gfx section has nybbles swapped
            bool const is_swapped = r.m_current_section == section::gfx;
            // Label is base32 (0-9 a-v), others are hexadecimal
            bool const is_base32 = r.m_current_section == section::lab;

            // Decode data from this section
            auto &section = r.m_sections[int8_t(r.m_current_section)];
            for (auto *parser = (uint8_t const *)in.begin(); parser < (uint8_t const *)in.end(); ++parser)
            {
                if (is_base32)
                {
                    uint8_t ch = parser[0];
                    int8_t b = ch >= '0' && ch <= '9' ? ch - '0' :
                               ch >= 'a' && ch <= 'v' ? ch - 'a' + 10 :
                               ch >= 'A' && ch <= 'V' ? ch - 'A' + 10 :
                               -1;
                    if (b >= 0)
                        section.push_back(uint8_t(b));
                }
                else if ((parser[0] >= 'a' && parser[0] <= 'f')
                      || (parser[0] >= 'A' && parser[0] <= 'F')
                      || (parser[0] >= '0' && parser[0] <= '9'))
                {
                    char str[3] = { (char)parser[is_swapped ? 1 : 0],
                                    (char)parser[is_swapped ? 0 : 1], '\0' };
                    section.push_back((uint8_t)strtoul(str, nullptr, 16));
                    ++parser;
                }
            }
        }
    }
};

struct replacement
{
    replacement(char const *re, char const *str)
      : m_re(re),
        m_str(str)
    {}

    std::string replace(std::string const &str) const
    {
        return std::regex_replace(str, m_re, m_str);
    }

private:
    std::regex m_re;
    char const *m_str;
};

bool cart::load_p8(std::string const &filename)
{
    std::string s;
    if (!lol::file::read(lol::sys::get_data_path(filename), s))
        return false;

    msg::debug("loaded file %s\n", filename.c_str());

    p8_reader reader;
    reader.parse(s.c_str());

    if (reader.m_version < 0)
        return false;

    // PICO-8 saves some symbols in the .p8 file as Emoji/Unicode characters
    // but the runtime expects 8-bit characters instead.
    m_code = charset::utf8_to_pico8(reader.m_code);

    memset(&m_rom, 0, sizeof(m_rom));

    auto const &gfx = reader.m_sections[(int8_t)p8_reader::section::gfx];
    auto const &gff = reader.m_sections[(int8_t)p8_reader::section::gff];
    auto const &map = reader.m_sections[(int8_t)p8_reader::section::map];
    auto const &sfx = reader.m_sections[(int8_t)p8_reader::section::sfx];
    auto const &mus = reader.m_sections[(int8_t)p8_reader::section::mus];
    auto const &lab = reader.m_sections[(int8_t)p8_reader::section::lab];

    msg::debug("version: %d code: %d gfx: %d/%d gff: %d/%d map: %d/%d "
               "sfx: %d/%d mus: %d/%d lab: %d/%d\n",
               reader.m_version, (int)m_code.length(),
               (int)gfx.size(), (int)sizeof(m_rom.gfx),
               (int)gff.size(), (int)sizeof(m_rom.gfx_props),
               (int)map.size(), (int)(sizeof(m_rom.map) + sizeof(m_rom.map2)),
               (int)sfx.size() / (4 + 80) * (4 + 64), (int)sizeof(m_rom.sfx),
               (int)mus.size() / 5 * 4, (int)sizeof(m_rom.song),
               (int)lab.size(), LABEL_WIDTH * LABEL_HEIGHT);

    // The optional second chunk of gfx is contiguous, we can copy it directly
    memcpy(&m_rom.gfx, gfx.data(), std::min(sizeof(m_rom.gfx), gfx.size()));

    memcpy(&m_rom.gfx_props, gff.data(), std::min(sizeof(m_rom.gfx_props), gff.size()));

    // Map data + optional second chunk
    memcpy(&m_rom.map, map.data(), std::min(sizeof(m_rom.map), map.size()));
    if (map.size() > sizeof(m_rom.map))
    {
        size_t map2_count = std::min(sizeof(m_rom.map2),
                                     map.size() - sizeof(m_rom.map));
        // Use binary OR because some old versions of PICO-8 would store
        // a full gfx+gfx2 section AND a full map+map2 section, so we cannot
        // really decide which one is relevant.
        for (size_t i = 0; i < map2_count; ++i)
            m_rom.map2[i] |= map[sizeof(m_rom.map) + i];
    }

    // Song data is encoded slightly differently
    size_t song_count = std::min(sizeof(m_rom.song) / 4,
                                 mus.size() / 5);
    for (size_t i = 0; i < song_count; ++i)
    {
        m_rom.song[i].data[0] = mus[i * 5 + 1] | ((mus[i * 5] << 7) & 0x80);
        m_rom.song[i].data[1] = mus[i * 5 + 2] | ((mus[i * 5] << 6) & 0x80);
        m_rom.song[i].data[2] = mus[i * 5 + 3] | ((mus[i * 5] << 5) & 0x80);
        m_rom.song[i].data[3] = mus[i * 5 + 4] | ((mus[i * 5] << 4) & 0x80);
    }

    // SFX data is packed
    size_t sfx_count = std::min(sizeof(m_rom.sfx) / (4 + 32 * 2),
                                sfx.size() / (4 + 32 * 5 / 2));
    for (size_t i = 0; i < sfx_count; ++i)
    {
        // FIXME move this to the parser maybe?
        for (int j = 0; j < 32; ++j)
        {
            uint32_t ins = (sfx[4 + i * (4 + 80) + j * 5 / 2 + 0] << 16)
                         | (sfx[4 + i * (4 + 80) + j * 5 / 2 + 1] << 8)
                         | (sfx[4 + i * (4 + 80) + j * 5 / 2 + 2]);
            // We read unaligned data; must realign it if j is odd
            ins = (j & 1) ? ins & 0xfffff : ins >> 4;

            m_rom.sfx[i].notes[j].key = (ins & 0x3f000) >> 12;
            m_rom.sfx[i].notes[j].instrument = (ins & 0x700) >> 8;
            m_rom.sfx[i].notes[j].volume = (ins & 0x70) >> 4;
            m_rom.sfx[i].notes[j].effect = ins & 0xf;
        }

        m_rom.sfx[i].editor_mode = sfx[i * (4 + 32 * 5 / 2) + 0];
        m_rom.sfx[i].speed       = sfx[i * (4 + 32 * 5 / 2) + 1];
        m_rom.sfx[i].loop_start  = sfx[i * (4 + 32 * 5 / 2) + 2];
        m_rom.sfx[i].loop_end    = sfx[i * (4 + 32 * 5 / 2) + 3];
    }

    // Optional cartridge label
    m_label.resize(std::min(lab.size(), size_t(LABEL_WIDTH * LABEL_HEIGHT)));
    memcpy(m_label.data(), lab.data(), m_label.size());

    // Invalidate code cache
    m_lua.resize(0);

    return true;
}

bool cart::save_png(std::string const &filename) const
{
    // Open blank cartridge
    std::vector<uint8_t> image;
    unsigned int width, height;
    unsigned int error = lodepng::decode(image, width, height,
                                         lol::sys::get_data_path("data/blank.png"));
    if (error != 0)
    {
        lol::msg::error("cannot load blank cart: %s\n", lodepng_error_text(error));
        return false;
    }

    u8vec4 *pixels = (u8vec4 *)image.data();

    // Apply label
    if (m_label.size() >= LABEL_WIDTH * LABEL_HEIGHT)
    {
        for (int y = 0; y < LABEL_HEIGHT; ++y)
        for (int x = 0; x < LABEL_WIDTH; ++x)
        {
            uint8_t col = m_label[y * LABEL_WIDTH + x] & 0x1f;
            pixels[(y + LABEL_Y) * width + (x + LABEL_X)] = palette::get8(col);
        }
    }

    // Create ROM data
    std::vector<uint8_t> const &rom = get_bin();

    // Write ROM to lower image bits
    for (size_t n = 0; n < rom.size(); ++n)
    {
        u8vec4 p(rom[n] & 0x30, rom[n] & 0x0c, rom[n] & 0x03, rom[n] & 0xc0);
        pixels[n] = pixels[n] / 4 * 4 + p / u8vec4(16, 4, 1, 64);
    }

    error = lodepng::encode(filename, image, width, height);
    if (error != 0)
    {
        lol::msg::error("cannot save cart: %s\n", lodepng_error_text(error));
        return false;
    }

    return true;
}

std::vector<uint8_t> cart::get_compressed_code() const
{
    return code::compress(m_code);
}

std::vector<uint8_t> cart::get_bin() const
{
    int const data_size = offsetof(memory, code);

    // Create ROM image
    std::vector<uint8_t> ret;

    // Copy data to ROM
    ret.resize(data_size);
    memcpy(ret.data(), &m_rom, data_size);

    // Copy code to ROM
    auto compressed = code::compress(m_code);
    ret.insert(ret.end(), compressed.begin(), compressed.end());

    msg::debug("compressed code length: %d/%d\n",
               int(compressed.size()), int(sizeof(memory::code)));
    ret.resize(sizeof(memory));

    ret.push_back(PICO8_VERSION);

    return ret;
}

bool cart::save_p8(std::string const &filename) const
{
    std::string ret = "pico-8 cartridge // http://www.pico-8.com\n";
    ret += lol::format("version %d\n", PICO8_VERSION);

    ret += "__lua__\n";
    ret += z8::pico8::charset::pico8_to_utf8(get_code());
    if (ret.back() != '\n')
        ret += '\n';

    // Export gfx section
    int gfx_lines = 0;
    for (int i = 0; i < (int)sizeof(m_rom.gfx); ++i)
        if (m_rom.gfx.data[i / 64][i % 64] != 0)
            gfx_lines = 1 + i / 64;

    for (int line = 0; line < gfx_lines; ++line)
    {
        if (line == 0)
            ret += "__gfx__\n";

        for (int i = 0; i < 64; ++i)
            ret += lol::format("%02x", uint8_t(m_rom.gfx.data[line][i] * 0x101 / 0x10));

        ret += '\n';
    }

    // Export label
    if (m_label.size() >= LABEL_WIDTH * LABEL_HEIGHT)
    {
        ret += "__label__\n";
        for (int i = 0; i < LABEL_WIDTH * LABEL_HEIGHT; ++i)
        {
            uint8_t col = m_label.data()[i];
            ret += "0123456789abcdefghijklmnopqrstuv"[col & 0x1f];
            if ((i + 1) % LABEL_WIDTH == 0)
                ret += '\n';
        }
        ret += '\n';
    }

    // Export gff section
    int gff_lines = 0;
    for (int i = 0; i < (int)sizeof(m_rom.gfx_props); ++i)
        if (m_rom.gfx_props[i] != 0)
            gff_lines = 1 + i / 128;

    for (int line = 0; line < gff_lines; ++line)
    {
        if (line == 0)
            ret += "__gff__\n";

        for (int i = 0; i < 128; ++i)
            ret += lol::format("%02x", m_rom.gfx_props[128 * line + i]);

        ret += '\n';
    }

    // Only serialise m_rom.map, because m_rom.map2 overlaps with m_rom.gfx
    // which has already been serialised.
    // FIXME: we could choose between map2 and gfx2 by looking at line
    // patterns, because the stride is different. See mandel.p8 for an
    // example.
    int map_lines = 0;
    for (int i = 0; i < (int)sizeof(m_rom.map); ++i)
        if (m_rom.map[i] != 0)
            map_lines = 1 + i / 128;

    for (int line = 0; line < map_lines; ++line)
    {
        if (line == 0)
            ret += "__map__\n";

        for (int i = 0; i < 128; ++i)
            ret += lol::format("%02x", m_rom.map[128 * line + i]);

        ret += '\n';
    }

    // Export sfx section
    int sfx_lines = 0;
    for (int i = 0; i < (int)sizeof(m_rom.sfx); ++i)
        if (((uint8_t const *)&m_rom.sfx)[i] != 0)
            sfx_lines = 1 + i / (int)sizeof(m_rom.sfx[0]);

    for (int line = 0; line < sfx_lines; ++line)
    {
        if (line == 0)
            ret += "__sfx__\n";

        uint8_t const *data = (uint8_t const *)&m_rom.sfx[line];
        ret += lol::format("%02x%02x%02x%02x", data[64], data[65], data[66], data[67]);
        for (int j = 0; j < 64; j += 2)
        {
            int pitch = data[j] & 0x3f;
            int instrument = ((data[j + 1] << 2) & 0x4) | (data[j] >> 6);
            int volume = (data[j + 1] >> 1) & 0x7;
            int effect = (data[j + 1] >> 4) & 0xf;
            ret += lol::format("%02x%1x%1x%1x", pitch, instrument, volume, effect);
        }

        ret += '\n';
    }

    // Export music section
    int music_lines = 0;
    for (int i = 0; i < (int)sizeof(m_rom.song); ++i)
        if (((uint8_t const *)&m_rom.song)[i] != 0)
            music_lines = 1 + i / (int)sizeof(m_rom.song[0]);

    for (int line = 0; line < music_lines; ++line)
    {
        if (line == 0)
            ret += "__music__\n";

        auto const &song = m_rom.song[line];
        int const flags = song.start | (song.loop << 1) | (song.stop << 2) | (song.mode << 3);
        ret += lol::format("%02x %02x%02x%02x%02x\n", flags,
                           song.sfx(0), song.sfx(1),
                           song.sfx(2), song.sfx(3));
    }

    ret += '\n';

    std::ofstream(filename) << ret;
    return true;
}

} // namespace z8::pico8

