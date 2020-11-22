#include <memory>
#include <unordered_map>
#include <cstddef>
#include <exception>
#include <cctype>
#include <stdexcept>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <windows.h>
#include "../rf/common.h"
#include "../rf/graphics.h"
#include "../rf/bmpman.h"
#include "../rf/multi.h"
#include "../utils/string-utils.h"
#include "graphics_internal.h"
#include "gr_color.h"

#include <ft2build.h>
#include FT_FREETYPE_H

struct ParsedFontName
{
    std::string name;
    int size_x;
    int size_y;
    bool digits_only;
};

template<typename T = int>
class TextureAtlasPacker {
public:
    void add(int w, int h, T userdata);
    void pack();
    std::pair<int, int> get_size() const;
    std::pair<int, int> get_pos(T userdata) const;

private:
    struct Item
    {
        int w, h, area;
        T userdata;
    };
    int total_pixels_ = 0;
    std::pair<int, int> atlas_size_;
    std::vector<Item> items_;
    std::unordered_map<T, std::pair<int, int>> packed_pos_map_;

    void update_size();
    bool try_pack();
};

class GrNewFont
{
public:
    GrNewFont(std::string_view name);
    void draw(int x, int y, std::string_view text, rf::GrMode state) const;
    void draw_aligned(rf::GrTextAlignment align, int x, int y, std::string_view text, rf::GrMode state) const;
    void get_size(int* w, int* h, std::string_view text) const;

    const std::string& get_name() const
    {
        return name_;
    }

    int get_height() const
    {
        return height_;
    }

private:
    struct GlyphInfo
    {
        int bm_x;
        int bm_y;
        int bm_w;
        int bm_h;
        int x;
        int y;
        int advance_x;
    };

    std::string name_;
    int bitmap_;
    int height_;
    int baseline_y_;
    int line_spacing_;
    std::vector<GlyphInfo> glyphs_;
    int char_map_[256];
};

constexpr int ttf_font_flag = 0x1000;

FT_Library g_freetype_lib = nullptr;
int g_default_font_id = 0;
std::vector<GrNewFont> g_fonts;

static inline ParsedFontName ParseFontName(std::string_view name)
{
    auto name_splitted = StringSplit(name, ':');
    auto file_name_sv = name_splitted[0];
    auto size_y_sv = name_splitted.size() > 1 ? name_splitted[1] : "12";
    auto size_x_sv = name_splitted.size() > 2 ? name_splitted[2] : "0";
    auto flags_sv = name_splitted.size() > 3 ? name_splitted[3] : "";
    std::string file_name_str{file_name_sv};
    std::string size_y_str{size_y_sv};
    int size_y = std::stoi(size_y_str);
    std::string size_x_str{size_x_sv};
    int size_x = std::stoi(size_x_str);
    bool digits_only = StringContains(flags_sv, 'd');
    return {file_name_str, size_x, size_y, digits_only};
}

static bool LoadFileIntoBuffer(const char* name, std::vector<unsigned char>& buffer)
{
    rf::File file;
    if (file.Open(name) != 0) {
        xlog::error("Failed to open file %s", name);
        return false;
    }
    auto len = file.GetSize();
    buffer.resize(len);
    int total_bytes_read = 0;
    while (len - total_bytes_read > 0) {
        int num_bytes_read = file.Read(buffer.data() + total_bytes_read, len - total_bytes_read);
        if (num_bytes_read <= 0) {
            break;
        }
        total_bytes_read += num_bytes_read;
    }
    file.Close();
    if (total_bytes_read != len) {
        xlog::error("Cannot read all file bytes");
        return false;
    }
    return true;
}

template<typename T>
inline void TextureAtlasPacker<T>::add(int w, int h, T userdata)
{
    int area = w * h;
    items_.push_back({w, h, area, userdata});
    total_pixels_ += area;
}

template<typename T>
inline void TextureAtlasPacker<T>::pack()
{
    update_size();
    xlog::trace("Texture atlas usage: %.2f", total_pixels_ * 100.0f / (atlas_size_.first * atlas_size_.second));
    // Sort by area (from largest to smallest)
    std::sort(items_.begin(), items_.end(), [](Item& a, Item& b) {
        return a.area > b.area;
    });
    // Set positions
    if (!try_pack()) {
        atlas_size_ = {2 * atlas_size_.first, 2 * atlas_size_.second};
        if (!try_pack()) {
            throw std::runtime_error{"unable to pack texture atlas"};
        }
    }
}

template<typename T>
inline bool TextureAtlasPacker<T>::try_pack()
{
    // Set positions
    int cur_x = 0;
    int cur_y = 0;
    int current_row_h = 0;
    for (auto& item : items_) {
        if (cur_x + item.w > atlas_size_.first) {
            cur_x = 0;
            cur_y += current_row_h;
            current_row_h = 0;
        }
        if (cur_y + item.h > atlas_size_.second) {
            return false;
        }
        packed_pos_map_[item.userdata] = std::pair{cur_x, cur_y};
        cur_x += item.w;
        current_row_h = std::max(current_row_h, item.h);
    }
    return true;
}

template<typename T>
std::pair<int, int> TextureAtlasPacker<T>::get_size() const
{
    return atlas_size_;
}

template<typename T>
inline std::pair<int, int> TextureAtlasPacker<T>::get_pos(T userdata) const
{
    auto it = packed_pos_map_.find(userdata);
    if (it == packed_pos_map_.end()) {
        throw std::runtime_error{"element not found in texture atlas"};
    }
    return it->second;
}

template<typename T>
inline void TextureAtlasPacker<T>::update_size()
{
    // calculate area + some margin for place that we don't use
    int total_pixels_adjusted = total_pixels_ * 9 / 8;
    // find squere root of calculated area (atlas is a squere)
    auto size_before_rounding = std::sqrt(total_pixels_adjusted);
    // round to next power of two
    auto exp = std::ceil(std::log(size_before_rounding) / std::log(2));
    int size = static_cast<int>(std::pow(2, exp));
    atlas_size_ = std::pair{size, size};
}

GrNewFont::GrNewFont(std::string_view name) :
    name_{name}
{
    auto [filename, size_x, size_y, digits_only] = ParseFontName(name);
    std::vector<unsigned char> buffer;
    xlog::trace("Loading font %s size %d", filename.c_str(), size_y);
    if (!LoadFileIntoBuffer(filename.c_str(), buffer)) {
        xlog::error("LoadFileIntoBuffer failed for %s", filename.c_str());
        throw std::runtime_error{"failed to load font"};
    }

    FT_Face face;
    FT_Error error = FT_New_Memory_Face(g_freetype_lib, buffer.data(), buffer.size(), 0, &face);
    if (error) {
        xlog::error("FT_New_Memory_Face failed: %d", error);
        throw std::runtime_error{"failed to load font"};
    }

    error = FT_Set_Pixel_Sizes(face, size_x, size_y);
    if (error) {
        xlog::error("FT_Set_Pixel_Sizes failed: %d", error);
        throw std::runtime_error{"failed to load font"};
    }

    xlog::trace("raw height %d ascender %d descender %d", face->height, face->ascender, face->descender);
    xlog::trace("scaled height %ld ascender %ld descender %ld", face->size->metrics.height / 64,
        face->size->metrics.ascender / 64, face->size->metrics.descender / 64);
    line_spacing_ = face->size->metrics.height / 64;
    height_ = line_spacing_; //(face->size->metrics.ascender - face->size->metrics.descender) / 64;
    baseline_y_ = face->size->metrics.ascender / 64;
    xlog::trace("line_spacing %d height %d baseline_y %d", line_spacing_, height_, baseline_y_);

    // Prepare lookup table for translating Windows 1252 characters (encoding used by RF) into Unicode codepoints

    static std::pair<int, int> win_1252_char_ranges[]{
        {0x20, 0x7E},
        {0x8C, 0x8C},
        {0x99, 0x99},
        {0x9C, 0x9C},
        {0x9F, 0x9F},
        {0xA6, 0xA7},
        {0xAA, 0xAB},
        {0xAE, 0xAE},
        {0xB0, 0xB0},
        {0xC0, 0xC2},
        {0xC4, 0xCB},
        {0xCE, 0xCF},
        {0xD2, 0xD4},
        {0xD6, 0xD6},
        {0xD9, 0xDC},
        {0xDF, 0xE2},
        {0xE4, 0xEB},
        {0xEE, 0xEF},
        {0xF2, 0xF4},
        {0xF6, 0xF6},
        {0xF9, 0xFC},
    };

    // Prepare character mapping and array of unicode code points
    std::fill(char_map_, char_map_ + std::size(char_map_), -1);
    std::vector<int> unicode_code_points;
    for (auto& range : win_1252_char_ranges) {
        for (auto c = range.first; c < range.second + 1; ++c) {
            if (digits_only && !std::isdigit(c)) {
                continue;
            }
            char windows_1252_char = static_cast<char>(c);
            wchar_t unicode_char = 0;
            MultiByteToWideChar(1252, 0, &windows_1252_char, 1, &unicode_char, 1);
            unicode_code_points.push_back(unicode_char);
            auto char_idx = static_cast<unsigned char>(windows_1252_char);
            char_map_[char_idx] = unicode_code_points.size() - 1;
        }
    }

    TextureAtlasPacker atlas_packer;

    for (auto codepoint : unicode_code_points) {
        error = FT_Load_Char(face, codepoint, FT_LOAD_BITMAP_METRICS_ONLY);
        if (error) {
            xlog::error("FT_Load_Char failed: %d", error);
            continue;
        }
        FT_GlyphSlot slot = face->glyph;
        atlas_packer.add(slot->bitmap.width, slot->bitmap.rows, codepoint);
    }

    atlas_packer.pack();
    auto [atlas_w, atlas_h] = atlas_packer.get_size();

    xlog::trace("Creating font texture atlas %dx%d", atlas_w, atlas_h);
    bitmap_ = rf::BmCreate(rf::BM_FORMAT_ARGB_8888, atlas_w, atlas_h);
    if (bitmap_ == -1) {
        xlog::error("BmCreate failed for font texture");
        throw std::runtime_error{"failed to load font"};
    }
    rf::GrLockInfo lock;
    if (!rf::GrLock(bitmap_, 0, &lock, 2)) {
        xlog::error("GrLock failed for font texture");
        throw std::runtime_error{"failed to load font"};
    }

    auto bitmap_bits = reinterpret_cast<unsigned char*>(lock.bits);
    glyphs_.reserve(unicode_code_points.size());

    for (auto codepoint : unicode_code_points) {
        error = FT_Load_Char(face, codepoint, FT_LOAD_RENDER);
        if (error) {
            xlog::error("FT_Load_Char failed: %d", error);
            continue;
        }
        FT_GlyphSlot slot = face->glyph;
        FT_Bitmap& bitmap = slot->bitmap;
        int glyph_bm_w = static_cast<int>(bitmap.width);
        int glyph_bm_h = static_cast<int>(bitmap.rows);

        auto [glyph_bm_x, glyph_bm_y] = atlas_packer.get_pos(codepoint);

        xlog::trace("glyph %x bitmap x %d y %d w %d h %d left %d top %d advance %ld", codepoint, glyph_bm_x, glyph_bm_y,
            glyph_bm_w, glyph_bm_h, slot->bitmap_left, slot->bitmap_top, slot->advance.x >> 6);

        GlyphInfo glyph_info;
        glyph_info.advance_x = slot->advance.x >> 6;
        glyph_info.bm_x = glyph_bm_x;
        glyph_info.bm_y = glyph_bm_y;
        glyph_info.bm_w = glyph_bm_w;
        glyph_info.bm_h = glyph_bm_h;
        glyph_info.x = slot->bitmap_left;
        glyph_info.y = -slot->bitmap_top;

        int pixel_size = GetPixelFormatSize(lock.pixel_format);
        auto dst_ptr = bitmap_bits + glyph_bm_y * lock.pitch + glyph_bm_x * pixel_size;
        ConvertSurfacePixelFormat(dst_ptr, lock.pixel_format, bitmap.buffer, rf::BM_FORMAT_A_8, bitmap.width, bitmap.rows, lock.pitch, bitmap.pitch);

        glyphs_.push_back(glyph_info);
    }

    rf::GrUnlock(&lock);
    rf::GrTcacheAddRef(bitmap_);
}

void GrNewFont::draw(int x, int y, std::string_view text, rf::GrMode state) const
{
    if (x == rf::center_x) {
        draw_aligned(rf::GR_ALIGN_CENTER, rf::gr_screen.clip_width / 2, y, text, state);
        return;
    }
    int pen_x = x;
    int pen_y = y + baseline_y_;
    for (auto ch : text) {
        if (ch == '\n') {
            pen_x = x;
            y += line_spacing_;
        }
        else {
            auto glyph_idx = char_map_[static_cast<unsigned char>(ch)];
            if (glyph_idx != -1) {
                auto& glyph_info = glyphs_[glyph_idx];
                if (glyph_info.bm_w) {
                    //rf::GrRect(pen_x + glyph_info.x, pen_y + glyph_info.y, glyph_info.bm_w, glyph_info.bm_h);
                    rf::GrBitmapEx(bitmap_, pen_x + glyph_info.x, pen_y + glyph_info.y, glyph_info.bm_w, glyph_info.bm_h, glyph_info.bm_x, glyph_info.bm_y, state);
                }
                pen_x += glyph_info.advance_x;
            }
        }
    }
}

void GrNewFont::draw_aligned(rf::GrTextAlignment alignment, int x, int y, std::string_view text, rf::GrMode state) const
{
    size_t cur_pos = 0;
    while (cur_pos < text.size()) {
        auto line_end_pos = text.find('\n', cur_pos);
        if (line_end_pos == std::string_view::npos) {
            line_end_pos = text.size();
        }
        size_t line_len = line_end_pos - cur_pos;
        auto line = text.substr(cur_pos, line_len);
        cur_pos += line_len + 1;

        int w, h;
        get_size(&w, &h, line);

        int line_x = x;
        if (alignment == rf::GR_ALIGN_CENTER) {
            line_x -= w / 2;
        }
        else if (alignment == rf::GR_ALIGN_RIGHT) {
            line_x -= w;
        }
        draw(line_x, y, line, state);
        y += line_spacing_;
    }
}

void GrNewFont::get_size(int* w, int* h, std::string_view text) const
{
    *w = 0;
    *h = line_spacing_;
    int cur_line_w = 0;
    for (auto ch : text) {
        if (ch == '\n') {
            *w = std::max(*w, cur_line_w);
            cur_line_w = 0;
            *h += line_spacing_;
        }
        else {
            auto glyph_idx = char_map_[static_cast<unsigned char>(ch)];
            if (glyph_idx != -1) {
                auto& glyph_info = glyphs_[glyph_idx];
                cur_line_w += glyph_info.advance_x;
            }
        }
    }
    *w = std::max(*w, cur_line_w);
}

void InitFreeTypeLib()
{
    FT_Error error = FT_Init_FreeType(&g_freetype_lib);
    if (error) {
        xlog::error("FT_Init_FreeType failed: %d", error);
    }
}

int GetDefaultFontId()
{
    return g_default_font_id;
}

void SetDefaultFontId(int font_id)
{
    g_default_font_id = font_id;
}

FunHook<int(const char*, int)> GrLoadFont_hook{
    0x0051F6E0,
    [](const char *name, int reserved) {
        if (StringEndsWith(name, ".vf")) {
            return GrLoadFont_hook.CallTarget(name, reserved);
        }
        else if (rf::is_dedicated_server) {
           return -1;
        }
        else {
            for (unsigned i = 0; i < g_fonts.size(); ++i) {
                auto& font = g_fonts[i];
                if (font.get_name() == name) {
                    return static_cast<int>(i | ttf_font_flag);
                }
            }
            try {
                GrNewFont font{name};
                g_fonts.push_back(font);
                return static_cast<int>((g_fonts.size() - 1) | ttf_font_flag);
            }
            catch (std::exception& e) {
                xlog::error("Failed to load font %s: %s", name, e.what());
                return -1;
            }
        }
    },
};

FunHook<bool(const char*)> GrSetDefaultFont_hook{
    0x0051FE20,
    [](const char* name) {
        int font = rf::GrLoadFont(name, -1);
        if (font >= 0) {
            g_default_font_id = font;
            return true;
        }
        return false;
    },
};

FunHook<int(int)> GrGetFontHeight_hook{
    0x0051F4D0,
    [](int font_num) {
        if (font_num == -1) {
            font_num = g_default_font_id;
        }
        if (font_num & ttf_font_flag) {
            auto& font = g_fonts[font_num & ~ttf_font_flag];
            return font.get_height();
        }
        else {
            return GrGetFontHeight_hook.CallTarget(font_num);
        }
    },
};

FunHook<void(int, int, const char*, int, rf::GrMode)> GrString_hook{
    0x0051FEB0,
    [](int x, int y, const char *text, int font_num, rf::GrMode mode) {
        if (font_num == -1) {
            font_num = g_default_font_id;
        }
        if (font_num & ttf_font_flag) {
            auto& font = g_fonts[font_num & ~ttf_font_flag];
            font.draw(x, y, text, mode);
        }
        else {
            GrString_hook.CallTarget(x, y, text, font_num, mode);
        }
    },
};

FunHook<void(int*, int*, const char*, int, int)> GrGetTextSize_hook{
    0x0051F530,
    [](int *out_width, int *out_height, const char *text, int text_len, int font_num) {
        if (font_num == -1) {
            font_num = g_default_font_id;
        }
        if (font_num & ttf_font_flag) {
            auto& font = g_fonts[font_num & ~ttf_font_flag];
            std::string_view text_sv;
            if (text_len < 0) {
                text_sv = std::string_view{text};
            }
            else {
                text_sv = std::string_view{text, static_cast<size_t>(text_len)};
            }
            font.get_size(out_width, out_height, text_sv);
        }
        else {
            GrGetTextSize_hook.CallTarget(out_width, out_height, text, text_len, font_num);
        }
    },
};

void ApplyFontPatches()
{
    // Support TrueType fonts
    GrLoadFont_hook.Install();
    GrSetDefaultFont_hook.Install();
    GrGetFontHeight_hook.Install();
    GrString_hook.Install();
    GrGetTextSize_hook.Install();
    InitFreeTypeLib();
}
