#include <windows.h>
#include <d3d8.h>
#include "gr_color.h"
#include "../main.h"
#include "../rf/graphics.h"
#include "../rf/geometry.h"
#include "../utils/perf-utils.h"
#include <common/BuildConfig.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/ShortTypes.h>
#include <algorithm>
#include <stdexcept>

#define TEXTURE_DITHERING 0

template<int SRC_BITS, int DST_BITS>
struct ColorChannelConverter;

template<int BITS>
struct ColorChannel
{
    static constexpr int bits = BITS;
    static constexpr int max = (1 << BITS) - 1;

    int value = 0;

    template<int DST_BITS>
    constexpr operator ColorChannel<DST_BITS>() const
    {
        return ColorChannelConverter<BITS, DST_BITS>{}(*this);
    }

    ColorChannel<BITS> operator+(ColorChannel<BITS> other) const
    {
        return ColorChannel<BITS>{value + other.value};
    }

    ColorChannel<BITS> operator-(ColorChannel<BITS> other) const
    {
        return ColorChannel<BITS>{value - other.value};
    }

    ColorChannel<BITS> operator*(int n) const
    {
        return ColorChannel<BITS>{value * n};
    }

    ColorChannel<BITS> operator/(int n) const
    {
        return ColorChannel<BITS>{value / n};
    }
};

template<int SRC_BITS, int DST_BITS>
struct ColorChannelConverter
{
    constexpr ColorChannel<DST_BITS> operator()(ColorChannel<SRC_BITS> src)
    {
        return {(src.value * ColorChannel<DST_BITS>::max + ColorChannel<SRC_BITS>::max / 2)
            / ColorChannel<SRC_BITS>::max};
    }
};

template<int DST_BITS>
struct ColorChannelConverter<0, DST_BITS>
{
    constexpr ColorChannel<DST_BITS> operator()(ColorChannel<0>)
    {
        return {ColorChannel<DST_BITS>::max};
    }
};

template<int BITS>
struct ColorChannelConverter<BITS, BITS>
{
    constexpr ColorChannel<BITS> operator()(ColorChannel<BITS> src)
    {
        return src;
    }
};

static_assert(static_cast<ColorChannel<8>>(ColorChannel<5>{31}).value == 255);
static_assert(static_cast<ColorChannel<8>>(ColorChannel<5>{0}).value == 0);
static_assert(static_cast<ColorChannel<5>>(ColorChannel<8>{255}).value == 31);
static_assert(static_cast<ColorChannel<5>>(ColorChannel<8>{0}).value == 0);
static_assert(static_cast<ColorChannel<8>>(ColorChannel<0>{0}).value == 255);
static_assert(static_cast<ColorChannel<0>>(ColorChannel<0>{0}).value == 0);

template<unsigned BITS, unsigned OFFSET>
struct ChannelBitOffsetTrait
{
    static constexpr unsigned bits = BITS;
    static constexpr unsigned offset = OFFSET;
};

template<unsigned BITS, unsigned INDEX>
struct ChannelIndexTrait
{
    static constexpr unsigned bits = BITS;
    static constexpr unsigned index = INDEX;
};

template<rf::BmFormat PF>
struct PixelFormatTrait;

template<>
struct PixelFormatTrait<rf::BM_FORMAT_ARGB_8888>
{
    using Pixel = uint32_t;
    using PaletteEntry = void;

    using Alpha = ChannelBitOffsetTrait<8, 24>;
    using Red   = ChannelBitOffsetTrait<8, 16>;
    using Green = ChannelBitOffsetTrait<8, 8>;
    using Blue  = ChannelBitOffsetTrait<8, 0>;
};

template<>
struct PixelFormatTrait<rf::BM_FORMAT_RGB_888>
{
    using Pixel = uint8_t[3];
    using PaletteEntry = void;

    using Alpha = ChannelIndexTrait<0, 0>;
    using Red   = ChannelIndexTrait<8, 2>;
    using Green = ChannelIndexTrait<8, 1>;
    using Blue  = ChannelIndexTrait<8, 0>;
};

template<>
struct PixelFormatTrait<rf::BM_FORMAT_ARGB_1555>
{
    using Pixel = uint16_t;
    using PaletteEntry = void;

    using Alpha = ChannelBitOffsetTrait<1, 15>;
    using Red   = ChannelBitOffsetTrait<5, 10>;
    using Green = ChannelBitOffsetTrait<5, 5>;
    using Blue  = ChannelBitOffsetTrait<5, 0>;
};

template<>
struct PixelFormatTrait<rf::BM_FORMAT_RGB_565>
{
    using Pixel = uint16_t;
    using PaletteEntry = void;

    using Alpha = ChannelBitOffsetTrait<0, 0>;
    using Red   = ChannelBitOffsetTrait<5, 11>;
    using Green = ChannelBitOffsetTrait<6, 5>;
    using Blue  = ChannelBitOffsetTrait<5, 0>;
};

template<>
struct PixelFormatTrait<rf::BM_FORMAT_ARGB_4444>
{
    using Pixel = uint16_t;
    using PaletteEntry = void;

    using Alpha = ChannelBitOffsetTrait<4, 12>;
    using Red   = ChannelBitOffsetTrait<4, 8>;
    using Green = ChannelBitOffsetTrait<4, 4>;
    using Blue  = ChannelBitOffsetTrait<4, 0>;
};

template<>
struct PixelFormatTrait<rf::BM_FORMAT_A_8>
{
    using Pixel = uint8_t;
    using PaletteEntry = void;

    using Alpha = ChannelBitOffsetTrait<8, 0>;
    using Red   = ChannelBitOffsetTrait<0, 0>;
    using Green = ChannelBitOffsetTrait<0, 0>;
    using Blue  = ChannelBitOffsetTrait<0, 0>;
};

template<>
struct PixelFormatTrait<rf::BM_FORMAT_BGR_888_INDEXED>
{
    using Pixel = uint8_t;
    using PaletteEntry = uint8_t[3];

    using Alpha = ChannelIndexTrait<0, 0>;
    using Red = ChannelIndexTrait<8, 0>;
    using Green = ChannelIndexTrait<8, 1>;
    using Blue = ChannelIndexTrait<8, 2>;
};

template<>
struct PixelFormatTrait<rf::BM_FORMAT_BGR_888>
{
    using Pixel = uint8_t[3];
    using PaletteEntry = void;

    using Alpha = ChannelIndexTrait<0, 0>;
    using Red   = ChannelIndexTrait<8, 0>;
    using Green = ChannelIndexTrait<8, 1>;
    using Blue  = ChannelIndexTrait<8, 2>;
};

template<rf::BmFormat FMT>
struct PixelColor
{
    ColorChannel<PixelFormatTrait<FMT>::Alpha::bits> a;
    ColorChannel<PixelFormatTrait<FMT>::Red::bits> r;
    ColorChannel<PixelFormatTrait<FMT>::Green::bits> g;
    ColorChannel<PixelFormatTrait<FMT>::Blue::bits> b;

    template<rf::BmFormat PF2>
    operator PixelColor<PF2>() const
    {
        return PixelColor<PF2>{a, r, g, b};
    }

    PixelColor<FMT> operator-(PixelColor<FMT> other) const
    {
        return PixelColor<FMT>{a - other.a, r - other.r, g - other.g, b - other.b};
    }

    PixelColor<FMT> operator+(PixelColor<FMT> other) const
    {
        return PixelColor<FMT>{a + other.a, r + other.r, g + other.g, b + other.b};
    }

    PixelColor<FMT> operator*(int n) const
    {
        return PixelColor<FMT>{a * n, r * n, g * n, b * n};
    }

    PixelColor<FMT> operator/(int n) const
    {
        return PixelColor<FMT>{a / n, r / n, g / n, b / n};
    }
};

template<rf::BmFormat PF>
class PixelsReader
{
    using PFT = PixelFormatTrait<PF>;
    const typename PFT::Pixel* ptr_;
    const typename PFT::PaletteEntry* palette_;

    template<typename CH, typename T>
    ColorChannel<CH::bits> get_channel(const T& val)
    {
        if constexpr (CH::bits == 0) {
            return {0};
        }
        else if constexpr (std::is_array_v<T>) {
            return {val[CH::index]};
        }
        else {
            constexpr int mask = (1 << CH::bits) - 1;
            return {static_cast<int>(val >> CH::offset) & mask};
        }
    }

    template<typename T>
    PixelColor<PF> get_color(const T& val)
    {
        return {
            get_channel<typename PFT::Alpha>(val),
            get_channel<typename PFT::Red>(val),
            get_channel<typename PFT::Green>(val),
            get_channel<typename PFT::Blue>(val)
        };
    }

public:
    PixelsReader(const void* ptr, const void* palette = nullptr) :
        ptr_(reinterpret_cast<const typename PFT::Pixel*>(ptr)),
        palette_(reinterpret_cast<const typename PFT::PaletteEntry*>(palette))
    {}

    PixelColor<PF> read()
    {
        const auto& val = *(ptr_++);
        if constexpr (std::is_void_v<typename PFT::PaletteEntry>) {
            return get_color(val);
        }
        else {
            return get_color(palette_[val]);
        }
    }
};

template<rf::BmFormat PF>
class PixelsWriter
{
    using PFT = PixelFormatTrait<PF>;
    typename PixelFormatTrait<PF>::Pixel* ptr_;

    template<typename CH>
    void set_channel([[ maybe_unused ]] ColorChannel<CH::bits> val)
    {
        if constexpr (CH::bits == 0) {
            // nothing to do
        }
        else {
            (*ptr_)[CH::index] = val.value;
        }
    }

public:
    PixelsWriter(void* ptr) :
        ptr_(reinterpret_cast<typename PixelFormatTrait<PF>::Pixel*>(ptr))
    {}

    void write([[ maybe_unused ]] PixelColor<PF> color)
    {
        if constexpr (std::is_void_v<typename PFT::PaletteEntry>) {
            if constexpr (!std::is_array_v<typename PFT::Pixel>) {
                *ptr_ =
                    (color.a.value << PFT::Alpha::offset) |
                    (color.r.value << PFT::Red::offset) |
                    (color.g.value << PFT::Green::offset) |
                    (color.b.value << PFT::Blue::offset);
            }
            else {
                set_channel<typename PFT::Alpha>(color.a);
                set_channel<typename PFT::Red>(color.r);
                set_channel<typename PFT::Green>(color.g);
                set_channel<typename PFT::Blue>(color.b);
            }
            ptr_++;
        }
        else {
            throw std::runtime_error{"writing indexed pixels is not supported"};
        }
    }
};

template<rf::BmFormat SRC_FMT, rf::BmFormat DST_FMT>
class SurfacePixelFormatConverter
{
    const uint8_t* src_ptr_;
    uint8_t* dst_ptr_;
    size_t src_pitch_;
    size_t dst_pitch_;
    size_t w_;
    size_t h_;
    const void* src_palette_;

public:
    SurfacePixelFormatConverter(const void* src_ptr, void* dst_ptr, size_t src_pitch, size_t dst_pitch, size_t w, size_t h, const void* src_palette) :
        src_ptr_(reinterpret_cast<const uint8_t*>(src_ptr)), dst_ptr_(reinterpret_cast<uint8_t*>(dst_ptr)),
        src_pitch_(src_pitch), dst_pitch_(dst_pitch), w_(w), h_(h), src_palette_(src_palette)
    {}

    void operator()()
    {
#if TEXTURE_DITHERING
        auto prev_row_deltas = std::make_unique<PixelColor<SRC_FMT>[]>(w_ + 2);
        auto cur_row_deltas = std::make_unique<PixelColor<SRC_FMT>[]>(w_ + 2);
        while (h_ > 0) {
            PixelsReader<SRC_FMT> rdr{src_ptr_, src_palette_};
            PixelsWriter<DST_FMT> wrt{dst_ptr_};
            PixelColor<SRC_FMT> delta;
            for (size_t x = 0; x < w_; ++x) {
                PixelColor<SRC_FMT> old_clr = rdr.read()
                    + delta                  * 7 / 16
                    + prev_row_deltas[x]     * 3 / 16
                    + prev_row_deltas[x + 1] * 5 / 16
                    + prev_row_deltas[x + 2] * 1 / 16;
                // TODO: make sure values are in range?
                PixelColor<DST_FMT> new_clr = old_clr;
                delta = old_clr - static_cast<PixelColor<SRC_FMT>>(new_clr);
                cur_row_deltas[x + 1] = delta;
                wrt.write(new_clr);
            }
            --h_;
            src_ptr_ += src_pitch_;
            dst_ptr_ += dst_pitch_;
            cur_row_deltas.swap(prev_row_deltas);
        }
#else
        while (h_ > 0) {
            PixelsReader<SRC_FMT> rdr{src_ptr_, src_palette_};
            PixelsWriter<DST_FMT> wrt{dst_ptr_};
            for (size_t x = 0; x < w_; ++x) {
                wrt.write(rdr.read());
            }
            --h_;
            src_ptr_ += src_pitch_;
            dst_ptr_ += dst_pitch_;
        }
#endif
    }
};

template<rf::BmFormat FMT>
struct PixelFormatHolder
{
    static constexpr rf::BmFormat value = FMT;
};

template<typename F>
void CallWithPixelFormat(rf::BmFormat pixel_fmt, F handler)
{
    switch (pixel_fmt) {
        case rf::BM_FORMAT_ARGB_8888:
            handler(PixelFormatHolder<rf::BM_FORMAT_ARGB_8888>());
            return;
        case rf::BM_FORMAT_RGB_888:
            handler(PixelFormatHolder<rf::BM_FORMAT_RGB_888>());
            return;
        case rf::BM_FORMAT_RGB_565:
            handler(PixelFormatHolder<rf::BM_FORMAT_RGB_565>());
            return;
        case rf::BM_FORMAT_ARGB_4444:
            handler(PixelFormatHolder<rf::BM_FORMAT_ARGB_4444>());
            return;
        case rf::BM_FORMAT_ARGB_1555:
            handler(PixelFormatHolder<rf::BM_FORMAT_ARGB_1555>());
            return;
        case rf::BM_FORMAT_A_8:
            handler(PixelFormatHolder<rf::BM_FORMAT_A_8>());
            return;
        case rf::BM_FORMAT_BGR_888_INDEXED:
            handler(PixelFormatHolder<rf::BM_FORMAT_BGR_888_INDEXED>());
            return;
        case rf::BM_FORMAT_BGR_888:
            handler(PixelFormatHolder<rf::BM_FORMAT_BGR_888>());
            return;
        default:
            throw std::runtime_error{"Unhandled pixel format"};
    }
}

bool ConvertSurfacePixelFormat(void* dst_bits_ptr, rf::BmFormat dst_fmt, const void* src_bits_ptr,
                               rf::BmFormat src_fmt, int width, int height, int dst_pitch, int src_pitch,
                               const uint8_t* palette)
{
#if DEBUG_PERF
    static auto& color_conv_perf = PerfAggregator::create("ConvertSurfacePixelFormat");
    ScopedPerfMonitor mon{color_conv_perf};
#endif
    try {
        CallWithPixelFormat(src_fmt, [=](auto s) {
            CallWithPixelFormat(dst_fmt, [=](auto d) {
                SurfacePixelFormatConverter<decltype(s)::value, decltype(d)::value> conv{
                    src_bits_ptr, dst_bits_ptr,
                    static_cast<size_t>(src_pitch), static_cast<size_t>(dst_pitch),
                    static_cast<size_t>(width), static_cast<size_t>(height),
                    palette,
                };
                conv();
            });
        });
        return true;
    }
    catch (const std::exception& e) {
        xlog::error("Pixel format conversion failed (%d -> %d): %s", src_fmt, dst_fmt, e.what());
        return false;
    }
}

CodeInjection LevelLoadLightmaps_color_conv_patch{
    0x004ED3E9,
    [](auto& regs) {
        // Always skip original code
        regs.eip = 0x004ED4FA;

        auto lightmap = reinterpret_cast<rf::GLightmap*>(regs.ebx);

        rf::GrLockInfo lock;
        if (!rf::GrLock(lightmap->bm_handle, 0, &lock, 2))
            return;

    #if 1 // cap minimal color channel value as RF does
        for (int i = 0; i < lightmap->w * lightmap->h * 3; ++i)
            lightmap->buf[i] = std::max(lightmap->buf[i], (uint8_t)(4 << 3)); // 32
    #endif

        bool success = ConvertSurfacePixelFormat(lock.bits, lock.pixel_format, lightmap->buf,
            rf::BM_FORMAT_BGR_888, lightmap->w, lightmap->h, lock.pitch, 3 * lightmap->w, nullptr);
        if (!success)
            xlog::error("ConvertBitmapFormat failed for lightmap (dest format %d)", lock.pixel_format);

        rf::GrUnlock(&lock);
    },
};

CodeInjection GSurface_CalculateLightmap_color_conv_patch{
    0x004F2F23,
    [](auto& regs) {
        // Always skip original code
        regs.eip = 0x004F3023;

        auto face_light_info = reinterpret_cast<void*>(regs.esi);
        rf::GLightmap& lightmap = *StructFieldRef<rf::GLightmap*>(face_light_info, 12);
        rf::GrLockInfo lock;
        if (!rf::GrLock(lightmap.bm_handle, 0, &lock, 1)) {
            return;
        }

        int offset_y = StructFieldRef<int>(face_light_info, 20);
        int offset_x = StructFieldRef<int>(face_light_info, 16);
        int src_width = lightmap.w;
        int dst_pixel_size = GetPixelFormatSize(lock.pixel_format);
        uint8_t* src_data = lightmap.buf + 3 * (offset_x + offset_y * src_width);
        uint8_t* dst_data = &lock.bits[dst_pixel_size * offset_x + offset_y * lock.pitch];
        int height = StructFieldRef<int>(face_light_info, 28);
        int src_pitch = 3 * src_width;
        bool success = ConvertSurfacePixelFormat(dst_data, lock.pixel_format, src_data, rf::BM_FORMAT_BGR_888,
            src_width, height, lock.pitch, src_pitch);
        if (!success)
            xlog::error("ConvertSurfacePixelFormat failed for geomod (fmt %d)", lock.pixel_format);
        rf::GrUnlock(&lock);
    },
};

CodeInjection GSurface_AllocLightmap_color_conv_patch{
    0x004E487B,
    [](auto& regs) {
        // Skip original code
        regs.eip = 0x004E4993;

        auto face_light_info = reinterpret_cast<void*>(regs.esi);
        rf::GLightmap& lightmap = *StructFieldRef<rf::GLightmap*>(face_light_info, 12);
        rf::GrLockInfo lock;
        if (!rf::GrLock(lightmap.bm_handle, 0, &lock, 1)) {
            return;
        }

        int offset_y = StructFieldRef<int>(face_light_info, 20);
        int src_width = lightmap.w;
        int offset_x = StructFieldRef<int>(face_light_info, 16);
        uint8_t* src_data_begin = lightmap.buf;
        int src_offset = 3 * (offset_x + src_width * StructFieldRef<int>(face_light_info, 20)); // src offset
        uint8_t* src_data = src_offset + src_data_begin;
        int height = StructFieldRef<int>(face_light_info, 28);
        int dst_pixel_size = GetPixelFormatSize(lock.pixel_format);
        uint8_t* dst_row_ptr = &lock.bits[dst_pixel_size * offset_x + offset_y * lock.pitch];
        int src_pitch = 3 * src_width;
        bool success = ConvertSurfacePixelFormat(dst_row_ptr, lock.pixel_format, src_data, rf::BM_FORMAT_BGR_888,
                                                 src_width, height, lock.pitch, src_pitch);
        if (!success)
            xlog::error("ConvertBitmapFormat failed for geomod2 (fmt %d)", lock.pixel_format);
        rf::GrUnlock(&lock);
    },
};

CodeInjection GProcTexUpdateWater_patch{
    0x004E68D1,
    [](auto& regs) {
        // Skip original code
        regs.eip = 0x004E6B68;

        uintptr_t waveform_info = static_cast<uintptr_t>(regs.esi);
        int src_bm_handle = *reinterpret_cast<int*>(waveform_info + 36);
        rf::GrLockInfo src_lock_data, dst_lock_data;
        if (!rf::GrLock(src_bm_handle, 0, &src_lock_data, 0)) {
            return;
        }
        int dst_bm_handle = *reinterpret_cast<int*>(waveform_info + 24);
        if (!rf::GrLock(dst_bm_handle, 0, &dst_lock_data, 2)) {
            rf::GrUnlock(&src_lock_data);
            return;
        }

        try {
            CallWithPixelFormat(src_lock_data.pixel_format, [=](auto s) {
                CallWithPixelFormat(dst_lock_data.pixel_format, [=](auto d) {
                    auto& byte_1370f90 = AddrAsRef<uint8_t[256]>(0x1370F90);
                    auto& byte_1371b14 = AddrAsRef<uint8_t[256]>(0x1371B14);
                    auto& byte_1371090 = AddrAsRef<uint8_t[512]>(0x1371090);

                    uint8_t* dst_row_ptr = dst_lock_data.bits;
                    int src_pixel_size = GetPixelFormatSize(src_lock_data.pixel_format);

                    for (int y = 0; y < dst_lock_data.height; ++y) {
                        int t1 = byte_1370f90[y];
                        int t2 = byte_1371b14[y];
                        uint8_t* off_arr = &byte_1371090[-t1];
                        PixelsWriter<decltype(d)::value> wrt{dst_row_ptr};
                        for (int x = 0; x < dst_lock_data.width; ++x) {
                            int src_x = t1;
                            int src_y = t2 + off_arr[t1];
                            int src_x_limited = src_x & (dst_lock_data.width - 1);
                            int src_y_limited = src_y & (dst_lock_data.height - 1);
                            const uint8_t* src_ptr = src_lock_data.bits + src_x_limited * src_pixel_size + src_y_limited * src_lock_data.pitch;
                            PixelsReader<decltype(s)::value> rdr{src_ptr};
                            wrt.write(rdr.read());
                            ++t1;
                        }
                        dst_row_ptr += dst_lock_data.pitch;
                    }
                });
            });
        }
        catch (const std::exception& e) {
            xlog::error("Pixel format conversion failed for liquid wave texture: %s", e.what());
        }

        rf::GrUnlock(&src_lock_data);
        rf::GrUnlock(&dst_lock_data);
    }
};

CodeInjection GSolid_GetAmbientColorFromLightmap_patch{
    0x004E5CE3,
    [](auto& regs) {
        // Skip original code
        regs.eip = 0x004E5D57;

        int x = regs.edi;
        int y = regs.ebx;
        auto& lm = *reinterpret_cast<rf::GLightmap*>(regs.esi);
        auto& color = *reinterpret_cast<rf::Color*>(regs.esp + 0x34 - 0x28);

        // Optimization: instead of locking the lightmap texture get color data from lightmap pixels stored in RAM
        const uint8_t* src_ptr = lm.buf + (y * lm.w + x) * 3;
        color.Set(src_ptr[0], src_ptr[1], src_ptr[2], 255);
    },
};

FunHook<unsigned()> BinkInitDeviceInfo_hook{
    0x005210C0,
    []() {
        unsigned bink_flags = BinkInitDeviceInfo_hook.CallTarget();
        const int BINKSURFACE32 = 3;

        if (g_game_config.true_color_textures && g_game_config.res_bpp == 32) {
            static auto& bink_bm_pixel_fmt = AddrAsRef<uint32_t>(0x018871C0);
            bink_bm_pixel_fmt = rf::BM_FORMAT_RGB_888;
            bink_flags = BINKSURFACE32;
        }

        return bink_flags;
    },
};

CodeInjection MonitorUpdateStatic_patch{
    0x004123AD,
    [](auto& regs) {
        // Note: default noise generation algohritm is not used because it's not uniform enough in high resolution
        static int noise_buf;
        if (regs.edx % 15 == 0)
            noise_buf = std::rand();
        bool white = noise_buf & 1;
        noise_buf >>= 1;

        auto& lock = *reinterpret_cast<rf::GrLockInfo*>(regs.esp + 0x2C - 0x20);
        auto pixel_ptr = reinterpret_cast<char*>(regs.esi);
        int bytes_per_pixel = GetPixelFormatSize(lock.pixel_format);

        std::fill(pixel_ptr, pixel_ptr + bytes_per_pixel, white ? '\0' : '\xFF');
        regs.esi += bytes_per_pixel;
        ++regs.edx;
        regs.eip = 0x004123DA;
    },
};

CodeInjection MonitorUpdateOff_patch{
    0x00412430,
    [](auto& regs) {
        auto& lock = *reinterpret_cast<rf::GrLockInfo*>(regs.esp + 0x34 - 0x20);
        regs.ecx = lock.height * lock.pitch;
        regs.eip = 0x0041243F;
    },
};

void GrColorInit()
{
    // True Color textures
    if (g_game_config.res_bpp == 32 && g_game_config.true_color_textures) {
        // Available texture formats (tested for compatibility)
        WriteMem<u32>(0x005A7DFC, D3DFMT_X8R8G8B8); // old: D3DFMT_R5G6B5
        WriteMem<u32>(0x005A7E00, D3DFMT_A8R8G8B8); // old: D3DFMT_X1R5G5B5
        WriteMem<u32>(0x005A7E04, D3DFMT_A8R8G8B8); // old: D3DFMT_A1R5G5B5, lightmaps
        WriteMem<u32>(0x005A7E08, D3DFMT_A8R8G8B8); // old: D3DFMT_A4R4G4B4
        WriteMem<u32>(0x005A7E0C, D3DFMT_A4R4G4B4); // old: D3DFMT_A8R3G3B2

        // use 32-bit texture for Bink rendering
        BinkInitDeviceInfo_hook.Install();
    }

    // lightmaps
    LevelLoadLightmaps_color_conv_patch.Install();
    // geomod
    GSurface_CalculateLightmap_color_conv_patch.Install();
    GSurface_AllocLightmap_color_conv_patch.Install();
    // water
    AsmWriter(0x004E68B0, 0x004E68B6).nop();
    GProcTexUpdateWater_patch.Install();
    // ambient color
    GSolid_GetAmbientColorFromLightmap_patch.Install();
    // fix pixel format for lightmaps
    WriteMem<u8>(0x004F5EB8 + 1, rf::BM_FORMAT_RGB_888);

    // monitor noise
    MonitorUpdateStatic_patch.Install();
    // monitor off state
    MonitorUpdateOff_patch.Install();

}
