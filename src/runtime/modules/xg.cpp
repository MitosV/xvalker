#include "runtime/modules/xg.hpp"
#include "runtime/common/runtime.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace xv::runtime::modules {
FormatSize format_size(std::uint32_t format) {
    const auto within = [&](std::uint32_t first, std::uint32_t last) {
        return format >= first && format <= last;
    };
    if (within(1, 4)) // R32G32B32A32
        return {16, false};
    if (within(5, 8)) // R32G32B32
        return {12, false};
    if (within(9, 22)) // R16G16B16A16, R32G32, D32 + S8
        return {8, false};
    if (within(23, 47)) // 10:10:10:2, 11:11:10, R8G8B8A8, R16G16, R32, D24 + S8
        return {4, false};
    if (within(48, 59)) // R8G8, R16
        return {2, false};
    if (within(60, 65)) // R8, A8
        return {1, false};
    if (within(70, 72) || within(79, 81)) // BC1, BC4
        return {8, true};
    if (within(73, 78) || within(82, 84) || within(94, 99)) // BC2, BC3, BC5, BC6H, BC7
        return {16, true};
    if (format == 85 || format == 86 || format == 115) // 5:6:5, 5:5:5:1, 4:4:4:4
        return {2, false};
    return {4, false}; // B8G8R8A8 and friends, and anything unlisted
}

std::uint64_t texture_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                            std::uint32_t array_size, std::uint32_t format) {
    const auto size = format_size(format);
    if (!mips) { // a full chain
        for (auto side = std::max(width, height); side; side >>= 1)
            ++mips;
    }
    std::uint64_t total = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const std::uint64_t w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
        total += size.blocks ? ((w + 3) / 4) * ((h + 3) / 4) * size.bytes : w * h * size.bytes;
    }
    total *= std::max(array_size, 1u);
    return (total + 4095) & ~std::uint64_t{4095};
}

namespace {
// XG_TEXTURE2D_DESC as titles fill it in: the Direct3D 11.X texture description followed
// by the tile mode and the pitch.
struct TextureDescription {
    std::uint32_t width, height, mips, array_size, format, sample_count, sample_quality, usage,
        bind_flags, cpu_access, misc_flags, esram_offset, esram_usage, tile_mode, pitch;
};
// Titles read the total size from the start of XG_RESOURCE_LAYOUT. The structure goes on
// with per-plane and per-mip tables, 0xd40 bytes in all, which are left zeroed but for the
// row pitch of the first mip in bytes, at 0x5c: a title multiplies it by the height to
// size a copy of the texture.
constexpr std::size_t layout_bytes = 0xd40, first_pitch = 0x5c;
struct Computer {
    void *const *vtable;
    std::atomic<std::uint32_t> references;
    TextureDescription description;
};
std::uint32_t add_ref(Computer *self) { return ++self->references; }
std::uint32_t release(Computer *self) {
    const auto left = --self->references;
    if (!left)
        delete self;
    return left;
}
std::int32_t texture2d_layout(const TextureDescription *description, void *layout) {
    if (!description || !layout)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    std::memset(layout, 0, layout_bytes);
    const auto &d = *description;
    const std::uint64_t header[2] = {
        texture_bytes(d.width, d.height, d.mips, d.array_size, d.format), 4096};
    std::memcpy(layout, header, sizeof header);
    const std::uint32_t counts[2] = {d.mips, 1}; // mip levels, planes
    std::memcpy(static_cast<char *>(layout) + sizeof header, counts, sizeof counts);
    const auto size = format_size(d.format);
    const std::uint32_t pitch = size.blocks ? (std::max(d.width, 1u) + 3) / 4 * size.bytes
                                            : std::max(d.width, 1u) * size.bytes;
    std::memcpy(static_cast<char *>(layout) + first_pitch, &pitch, sizeof pitch);
    return 0;
}
std::int32_t resource_layout(Computer *self, void *layout) {
    return texture2d_layout(&self->description, layout);
}
// XG_TEXTURE3D_DESC, and the part of XG_MIPLEVEL_LAYOUT that says where a mip level is.
struct VolumeDescription {
    std::uint32_t width, height, depth, mips, format, usage, bind_flags, cpu_access, misc_flags,
        esram_offset, esram_usage, tile_mode, pitch;
};
struct MipLayout {
    std::uint64_t size, offset, slice_size;
    std::uint32_t pitch_pixels, pitch_bytes, alignment, padded_width, padded_height, padded_depth,
        width, height, depth, samples;
};
constexpr std::size_t plane_size = 0x20, bytes_per_element = 0x38, first_mip = 0x40,
                      mip_bytes = 0x60;
std::int32_t texture3d_layout(const VolumeDescription *description, void *layout) {
    if (!description || !layout)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    std::memset(layout, 0, layout_bytes);
    const auto &d = *description;
    const auto size = format_size(d.format);
    auto mips = d.mips;
    if (!mips) {
        for (auto side = std::max({d.width, d.height, d.depth}); side; side >>= 1)
            ++mips;
    }
    mips = std::min(mips, 15u);
    const auto base = static_cast<char *>(layout);
    std::uint64_t total = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const std::uint32_t w = std::max(d.width >> mip, 1u), h = std::max(d.height >> mip, 1u),
                            depth = std::max(d.depth >> mip, 1u);
        const std::uint32_t across = size.blocks ? (w + 3) / 4 : w,
                            down = size.blocks ? (h + 3) / 4 : h;
        MipLayout level{};
        level.slice_size = std::uint64_t{across} * down * size.bytes;
        level.size = level.slice_size * depth;
        level.offset = total;
        level.pitch_pixels = size.blocks ? across * 4 : across;
        level.pitch_bytes = across * size.bytes;
        level.alignment = 1;
        level.padded_width = level.width = across;
        level.padded_height = level.height = down;
        level.padded_depth = level.depth = depth;
        level.samples = 1;
        std::memcpy(base + first_mip + mip * mip_bytes, &level, sizeof level);
        total += level.size;
    }
    total = (total + 4095) & ~std::uint64_t{4095};
    const std::uint64_t header[2] = {total, 4096};
    std::memcpy(base, header, sizeof header);
    const std::uint32_t counts[2] = {mips, 1}; // mip levels, planes
    std::memcpy(base + sizeof header, counts, sizeof counts);
    const std::uint64_t plane[3] = {total, 0, 4096}; // size, offset, alignment
    std::memcpy(base + plane_size, plane, sizeof plane);
    std::memcpy(base + bytes_per_element, &size.bytes, sizeof(std::uint32_t));
    return 0;
}
template <int Slot> std::int32_t unknown(Computer *) {
    std::fprintf(stderr, "[Runtime] unimplemented xg_x.dll texture computer method %d\n", Slot);
    std::fflush(stderr);
    return static_cast<std::int32_t>(0x80004001); // E_NOTIMPL
}
void *const computer_vtable[] = {
    reinterpret_cast<void *>(&add_ref),         reinterpret_cast<void *>(&release),
    reinterpret_cast<void *>(&resource_layout), reinterpret_cast<void *>(&unknown<3>),
    reinterpret_cast<void *>(&unknown<4>),      reinterpret_cast<void *>(&unknown<5>),
    reinterpret_cast<void *>(&unknown<6>),      reinterpret_cast<void *>(&unknown<7>),
    reinterpret_cast<void *>(&unknown<8>),      reinterpret_cast<void *>(&unknown<9>),
    reinterpret_cast<void *>(&unknown<10>),     reinterpret_cast<void *>(&unknown<11>),
};

std::int32_t create_texture2d_computer(const TextureDescription *description, Computer **computer) {
    if (!description || !computer)
        return static_cast<std::int32_t>(0x80070057); // E_INVALIDARG
    *computer = new Computer{computer_vtable, 1, *description};
    return 0;
}
std::int32_t create_texture_computer(const std::uint32_t *description, Computer **) {
    std::fprintf(stderr, "[Runtime] XG_RESOURCE_DESC:");
    for (int i = 0; i < 16; ++i)
        std::fprintf(stderr, " %x", description[i]);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    return static_cast<std::int32_t>(0x80004001);
}
// There is one layout, so one tile mode: 0.
std::uint32_t optimal_tile_mode(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                std::uint32_t, std::uint32_t, std::uint32_t) {
    return 0;
}
std::int32_t optimal_depth_stencil_tile_modes(std::uint32_t, std::uint32_t, std::uint32_t,
                                              std::uint32_t, std::uint32_t, std::uint32_t,
                                              std::uint32_t *depth, std::uint32_t *stencil) {
    if (depth)
        *depth = 0;
    if (stencil)
        *stencil = 0;
    return 0;
}
} // namespace

void install_xg(Registry &registry, std::string_view module) {
    registry.add(module, "XGCreateTexture2DComputer",
                 reinterpret_cast<void *>(&create_texture2d_computer));
    registry.add(module, "XGCreateTextureComputer",
                 reinterpret_cast<void *>(&create_texture_computer));
    registry.add(module, "XGComputeTexture2DLayout", reinterpret_cast<void *>(&texture2d_layout));
    registry.add(module, "XGComputeTexture3DLayout", reinterpret_cast<void *>(&texture3d_layout));
    registry.add(module, "XGComputeOptimalTileMode", reinterpret_cast<void *>(&optimal_tile_mode));
    registry.add(module, "XGComputeOptimalDepthStencilTileModes",
                 reinterpret_cast<void *>(&optimal_depth_stencil_tile_modes));
}
} // namespace xv::runtime::modules
