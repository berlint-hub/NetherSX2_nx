#include "dk_native.h"
#include "dk_backend.h"
#include "dk_profiles.h"
#include "dk_boundary.h"
#include "dk_presentation.h"
#include "dk_transfer.h"
#include "../saltysd_display.h"
#include <lsfg/common/sha256.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <switch.h>

extern "C" {
#include "../config.h"
#include "../hooks.h"
#include "../so_util.h"
extern so_module emu_mod;
}

namespace nxdk {
const CoreProfile *profile = nullptr;
uintptr_t core_address(uint32_t offset) { return uintptr_t(emu_mod.load_virtbase) + offset; }
void *core_imgui_io() { return *reinterpret_cast<void **>(core_address(profile->imgui_io)); }

std::string_view CoreString::view() const {
  if (!(bytes[0] & 1)) return {reinterpret_cast<const char *>(bytes + 1), size_t(bytes[0] >> 1)};
  size_t size;
  const char *data;
  memcpy(&size, bytes + 8, sizeof(size));
  memcpy(&data, bytes + 16, sizeof(data));
  return {data, size};
}
void CoreString::release() {
  if (bytes[0] & 1) { void *data; memcpy(&data, bytes + 16, sizeof(data)); free(data); }
  memset(bytes, 0, sizeof(bytes));
}
void CoreString::assign(std::string_view value) {
  release();
  if (value.size() < 23) {
    bytes[0] = value.size() << 1;
    memcpy(bytes + 1, value.data(), value.size());
  } else {
    const size_t capacity = (value.size() + 16) & ~size_t(15), encoded = capacity | 1;
    char *data = static_cast<char *>(malloc(capacity));
    if (!data) return;
    memcpy(data, value.data(), value.size());
    data[value.size()] = '\0';
    const size_t size = value.size();
    memcpy(bytes, &encoded, sizeof(encoded));
    memcpy(bytes + 8, &size, sizeof(size));
    memcpy(bytes + 16, &data, sizeof(data));
  }
}

std::string core_pixel_source(void *device, const PsSelector &selector) {
  CoreString source;

  reinterpret_cast<void (*)(CoreString *, void *, const PsSelector *)>(core_address(profile->ps_source))(
      &source, device, &selector);
  std::string result(source.view());
  source.release();
  return result;
}

namespace {
struct CoreOwner { void *pointer = nullptr; ~CoreOwner() {} };

static_assert(sizeof(CoreOwner) == sizeof(void *));
struct HostTexture {
  uintptr_t *vtable;
  NativeTexture *texture;
};
void host_texture_destruct(HostTexture *value) { destroy_texture(value->texture); value->texture = nullptr; }
void host_texture_delete(HostTexture *value) { host_texture_destruct(value); delete value; }
void *host_texture_handle(HostTexture *value) { return value->texture->image.get(); }
uint32_t host_texture_width(HostTexture *value) { return value->texture->abi.width; }
uint32_t host_texture_height(HostTexture *value) { return value->texture->abi.height; }
uintptr_t host_texture_methods[]{abi_entry<host_texture_destruct>(), abi_entry<host_texture_delete>(),
    abi_entry<host_texture_handle>(), abi_entry<host_texture_width>(), abi_entry<host_texture_height>()};

struct Download { DownloadAbi abi; MemoryPtr memory; };
void download_destruct(Download *value) { gpu().retain(value->memory); value->memory.reset(); }
void download_delete(Download *value) { download_destruct(value); delete value; }
void download_flush(Download *value) {
  if (value->abi.needs_flush) { gpu().finish(); value->abi.needs_flush = false; }
}
bool download_map(Download *value, const Rect *) {
  download_flush(value);
  value->abi.map = static_cast<const uint8_t *>(value->memory->cpu());
  return value->abi.map != nullptr;
}
void download_unmap(Download *value) { value->abi.map = nullptr; }
void download_copy(Download *value, const Rect *destination, TextureAbi *source,
                    const Rect *source_rect, unsigned level, bool transfer_pitch) {
  if (!value || !source || !destination || !source_rect || destination->x < 0 || destination->y < 0 ||
      source_rect->x < 0 || source_rect->y < 0 || source_rect->z <= source_rect->x ||
      source_rect->w <= source_rect->y || level >= unsigned(source->levels)) return;
  const uint32_t width = source_rect->z - source_rect->x, height = source_rect->w - source_rect->y;
  if (uint64_t(destination->x) + width > value->abi.width ||
      uint64_t(destination->y) + height > value->abi.height ||
      source_rect->z > std::max(1, source->width >> level) ||
      source_rect->w > std::max(1, source->height >> level)) return;
  const unsigned bytes = texture_bytes(value->abi.format);
  const uint32_t pitch = align_up(uint64_t(transfer_pitch ? destination->z : value->abi.width) * bytes, 16);
  const uint64_t offset = uint64_t(destination->y) * pitch + uint64_t(destination->x) * bytes;
  if (!pitch || offset + uint64_t(height - 1) * pitch + uint64_t(width) * bytes > value->memory->size) return;
  const DkImageView view = texture(source)->image->view(level);
  const DkImageRect rect{uint32_t(source_rect->x), uint32_t(source_rect->y), 0, width, height, 1};
  const DkCopyBuf output = copy_buffer(value->memory->gpu() + offset, pitch,
      width * bytes, height, value->memory->size - offset);
  gpu().barrier(DkBarrier_Full, DkInvalidateFlags_Image);
  dkCmdBufCopyImageToBuffer(gpu().commands(), &view, &rect, &output, 0);
  gpu().barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
  value->abi.pitch = pitch;
  value->abi.needs_flush = true;
}
uintptr_t download_methods[]{abi_entry<download_destruct>(), abi_entry<download_delete>(),
    abi_entry<download_copy>(), abi_entry<download_map>(), abi_entry<download_unmap>(), abi_entry<download_flush>()};
CoreOwner create_download(void *, uint32_t width, uint32_t height, uint8_t format) {
  const unsigned bytes = texture_bytes(format);
  if (!width || !height || !bytes || compressed_texture(format)) return {};
  const uint32_t pitch = align_up(uint64_t(width) * bytes, 16);
  const uint32_t size = align_up(uint64_t(pitch) * height, 16);
  if (!size) return {};
  auto memory = gpu().allocate(size);
  if (!memory) return {};
  auto result = new Download{};
  result->abi.vtable = download_methods;
  result->abi.width = width;
  result->abi.height = height;
  result->abi.format = format;
  result->abi.pitch = pitch;
  result->memory = std::move(memory);
  return {result};
}

TextureAbi *create_surface(void *, uint8_t type, int width, int height, int levels, uint8_t format) {
  auto result = create_texture(type, width, height, levels, format);
  return result ? &result->abi : nullptr;
}
void noop(void *) {}
void classic_merge(void *core, TextureAbi **sources, const FloatRect *sr, TextureAbi *target,
                     const FloatRect *dr, const uint64_t *pmode, const uint64_t *extbuf,
                     const FloatRect *background) {
  merge_outputs(core, sources, sr, target, dr, pmode, extbuf, background, false);
}
void invalidate_target(void *, TextureAbi *target) { if (target) target->state = 2; }
void clear_target(void *, TextureAbi *target, const FloatRect *color) {
  if (color) clear_color(texture(target), &color->x);
}
void clear_packed_target(void *, TextureAbi *target, uint32_t color) {
  const float components[]{float(color & 255) / 255, float((color >> 8) & 255) / 255,
                            float((color >> 16) & 255) / 255, float(color >> 24) / 255};
  clear_color(texture(target), components);
}
void clear_depth_target(void *, TextureAbi *target) { clear_depth(texture(target)); }
void clear_stencil_target(void *, TextureAbi *target, uint8_t value) { clear_stencil(texture(target), value); }
void copy_rect(void *, TextureAbi *source, TextureAbi *target, const Rect *rect, unsigned x, unsigned y) {
  if (rect) copy_texture(texture(source), texture(target), *rect, x, y);
}
void stretch_mask(void *, TextureAbi *source, const FloatRect *sr, TextureAbi *destination,
                   const FloatRect *dr, bool red, bool green, bool blue, bool alpha) {
  if (source && sr && destination && dr)
    draw_rect(texture(source), *sr, texture(destination)->image.get(), *dr,
        utility_kernel("shaders/opengl/convert.glsl", "ps_copy", DkStage_Fragment), false,
        unsigned(red) | (unsigned(green) << 1) | (unsigned(blue) << 2) | (unsigned(alpha) << 3));
}
bool old_download(void *, TextureAbi *source, const Rect *rect, MapAbi *map) {
  return rect && map && download_texture(texture(source), *rect, *map);
}
bool old_download_convert(void *core, TextureAbi *source, const FloatRect *sr, const Size *size,
                           uint8_t format, unsigned shader, MapAbi *map, bool linear) {
  if (!size || !sr || !map || size->x <= 0 || size->y <= 0) return false;
  auto target = create_texture(4, size->x, size->y, 1, format);
  if (!target) return false;
  const FloatRect dr{0,0,float(size->x),float(size->y)};
  stretch_rect(core, source, sr, &target->abi, &dr, shader, linear);
  const bool result = download_texture(target, {0,0,size->x,size->y}, *map);
  destroy_texture(target);
  return result;
}

WindowAbi *window_info(void *core) { return reinterpret_cast<WindowAbi *>(static_cast<uint8_t *>(core) + 8); }
unsigned host_api(void *) { return 5; }
void *host_device(void *) { return gpu().device(); }
void *host_context(void *) { return gpu().queue(); }
void *host_surface(void *) { return nwindowGetDefault(); }
bool host_has_device(void *) { return gpu().device() != nullptr; }
bool host_has_surface(void *) { return gpu().has_window(); }
bool host_create(void *core, const WindowAbi *window, unsigned vsync) {
  if (!window || !gpu().initialize()) return false;
  *window_info(core) = *window;
  auto &wi = *window_info(core);
  wi.width = screen_width;
  wi.height = screen_height;
  wi.scale = window->scale > 0 ? window->scale : 1.0f;
  const float display_rate = saltysd_display_sync_rate();
  wi.refresh_rate = display_rate > 0.0f ? display_rate : 60.0f;
  *reinterpret_cast<unsigned *>(static_cast<uint8_t *>(core) + 0x3c) = vsync;
  gpu().set_swap_interval(vsync ? 1 : 0);

  if (!wi.type || !wi.window) {
    return true;
  }
  return gpu().create_window(wi.window, wi.width, wi.height);
}
bool host_true(void *) { return true; }
void host_destroy_surface(void *core) {
  gpu().destroy_window();
  *window_info(core) = {};
}
bool host_change(void *core, const WindowAbi *window) {
  if (!window) return false;
  *window_info(core) = *window;
  auto &wi = *window_info(core);

  if (!wi.type || !wi.window) {
    gpu().destroy_window();

    return true;
  }
  if (!wi.width || !wi.height) { wi.width = screen_width; wi.height = screen_height; }
  return gpu().create_window(wi.window, wi.width, wi.height);
}
void host_resize(void *core, int width, int height, float scale) {
  if (width <= 0 || height <= 0) return;
  auto &wi = *window_info(core);
  wi.scale = scale;
  if (wi.width != unsigned(width) || wi.height != unsigned(height)) {
    wi.width = width; wi.height = height;
    if (wi.type && wi.window) gpu().create_window(wi.window, width, height);
  }
}
bool host_false(void *) { return false; }
bool host_fullscreen(void *, bool, unsigned, unsigned, float) { return false; }
struct AdapterList { uint8_t vectors[48]{}; };
AdapterList host_adapters(void *) { return {}; }
CoreString host_driver(void *) { CoreString string; string.assign("Deko3D native GS backend (Tegra X1)"); return string; }
CoreOwner host_create_texture(void *, unsigned width, unsigned height, const void *data,
                               unsigned pitch, bool) {
  auto image = create_texture(3, width, height, 1, 1);
  if (!image) return {};
  if (data && !update_texture(image, {0,0,int(width),int(height)}, data, pitch ? pitch : width * 4)) {
    destroy_texture(image);
    return {};
  }
  return {new HostTexture{host_texture_methods, image}};
}
void host_update_texture(void *, HostTexture *image, unsigned x, unsigned y, unsigned width,
                          unsigned height, const void *data, unsigned pitch) {
  if (image) update_texture(image->texture, {int(x),int(y),int(x + width),int(y + height)}, data, pitch);
}
unsigned host_begin_present(void *, bool skip) {
  if (skip || !gpu().has_window()) return 1;
  Image *image = gpu().backbuffer();
  if (!image) return 2;
  bind_target(image, nullptr);
  dkCmdBufClearColorFloat(gpu().commands(), 0, DkColorMask_RGBA, 0, 0, 0, 1);
  gpu().image_written(image);
  return 0;
}
bool classic_begin_present(void *core, bool skip) {
  return classic_present_result(host_begin_present(core, skip), [] {
    reinterpret_cast<void (*)()>(core_address(profile->imgui_end_frame))();
  });
}
void host_end_present(void *core) {
  reinterpret_cast<void (*)()>(core_address(profile->imgui_render))();
  ui_render();
  gpu().present();
  const float display_rate = saltysd_display_sync_rate();
  if (display_rate > 0.0f) window_info(core)->refresh_rate = display_rate;
  __atomic_add_fetch(&dk_present_count, 1, __ATOMIC_RELAXED);
}
void host_vsync(void *core, unsigned mode) {
  *reinterpret_cast<unsigned *>(static_cast<uint8_t *>(core) + 0x3c) = mode;
  gpu().set_swap_interval(mode ? 1 : 0);
}
bool host_imgui(void *) {
  return ui_initialize();
}
void host_imgui_destroy(void *) { ui_shutdown(); }
bool host_imgui_font(void *) {
  return ui_update_font();
}
bool host_refresh(void *, float *rate) {
  const float display_rate = saltysd_display_sync_rate();
  if (rate) *rate = display_rate > 0.0f ? display_rate : 60.0f;
  return true;
}
bool host_timing(void *, bool) { return false; }
float host_gpu_time(void *) { return 0; }

void replace_slot(uintptr_t *table, unsigned slot, uintptr_t replacement) { table[slot] = replacement; }
bool verify_table(unsigned offset, const uint32_t *methods, unsigned count) {
  if (uint64_t(offset) + count * 8 > emu_mod.load_size) return false;
  const auto *table = reinterpret_cast<const uintptr_t *>(uintptr_t(emu_mod.load_base) + offset);
  for (unsigned i = 0; i < count; ++i)
    if (table[i] != core_address(methods[i])) return false;
  return true;
}
}
}

extern "C" { volatile int dk_present_count = 0; }

extern "C" int dk_install_native_backend(const char *path) {
  using namespace nxdk;
  profile = core_is_3668() ? &Profile3668 : g_core_version == CORE_VER_V22N_4248 ? &Profile4248 : nullptr;
  if (!profile || !path) return 0;

  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  lsfg::Sha256 hash;
  std::array<uint8_t, 65536> bytes;
  size_t count;
  while ((count = fread(bytes.data(), 1, bytes.size(), file)) != 0) hash.update({bytes.data(), count});
  const bool read = !ferror(file);
  fclose(file);
  if (!read || strcmp(lsfg::to_hex(hash.finish()).data(), profile->sha256) ||
      !verify_table(profile->device_table, profile->device_methods, profile->device_method_count) ||
      !verify_table(profile->host_table, profile->host_methods, 31)) return 0;

  auto *table = reinterpret_cast<uintptr_t *>(uintptr_t(emu_mod.load_base) + profile->device_table);
  const bool classic = profile->device_method_count == 32;
  replace_slot(table, 0, abi_entry<create_surface>());
  replace_slot(table, 1, classic ? abi_entry<classic_merge>() : abi_entry<merge_outputs>());
  replace_slot(table, 2, abi_entry<interlace>());
  replace_slot(table, 3, abi_entry<fxaa>());
  replace_slot(table, 4, abi_entry<shadeboost>());
  replace_slot(table, 5, abi_entry<sharpen>());
  replace_slot(table, 6, abi_entry<destroy_native_device>());
  replace_slot(table, 8, abi_entry<create_native_device>());
  replace_slot(table, 10, abi_entry<noop>());
  replace_slot(table, 11, abi_entry<noop>());
  unsigned slot = classic ? 14 : 12;
  replace_slot(table, slot++, abi_entry<clear_target>());
  replace_slot(table, slot++, abi_entry<clear_packed_target>());
  replace_slot(table, slot++, abi_entry<invalidate_target>());
  replace_slot(table, slot++, abi_entry<clear_depth_target>());
  replace_slot(table, slot++, abi_entry<clear_stencil_target>());
  slot += 3;
  if (classic) {
    replace_slot(table, slot++, abi_entry<old_download>());
    replace_slot(table, slot++, abi_entry<old_download_convert>());
    replace_slot(table, slot++, abi_entry<noop>());
  } else replace_slot(table, slot++, abi_entry<create_download>());
  replace_slot(table, slot++, abi_entry<copy_rect>());
  replace_slot(table, slot++, abi_entry<stretch_rect>());
  replace_slot(table, slot++, abi_entry<stretch_mask>());
  replace_slot(table, slot++, abi_entry<present_rect>());
  if (!classic) {
    replace_slot(table, slot++, abi_entry<multi_stretch>());
    replace_slot(table, slot++, abi_entry<update_clut>());
  }
  replace_slot(table, slot++, abi_entry<render_hardware>());
  replace_slot(table, slot++, abi_entry<noop>());

  const uint32_t trampoline[]{0x58000050, 0xd61f0200};
  void *destructor = reinterpret_cast<void *>(uintptr_t(emu_mod.load_base) + profile->device_methods[6]);
  memcpy(destructor, trampoline, sizeof(trampoline));
  const uintptr_t target = abi_entry<destroy_native_device>();
  memcpy(static_cast<uint8_t *>(destructor) + 8, &target, sizeof(target));

  auto *host = reinterpret_cast<uintptr_t *>(uintptr_t(emu_mod.load_base) + profile->host_table);
  const uintptr_t replacements[]{abi_entry<host_api>(), abi_entry<host_device>(), abi_entry<host_context>(),
    abi_entry<host_surface>(), abi_entry<host_has_device>(), abi_entry<host_has_surface>(),
    abi_entry<host_create>(), abi_entry<host_true>(), abi_entry<host_true>(), abi_entry<host_true>(),
    abi_entry<host_destroy_surface>(), abi_entry<host_change>(), abi_entry<host_resize>(),
    abi_entry<host_false>(), abi_entry<host_false>(), abi_entry<host_fullscreen>(), abi_entry<host_adapters>(),
    abi_entry<host_driver>(), abi_entry<host_create_texture>(), abi_entry<host_update_texture>(),
    classic ? abi_entry<classic_begin_present>() : abi_entry<host_begin_present>(),
    abi_entry<host_end_present>(), abi_entry<host_vsync>(),
    abi_entry<host_imgui>(), abi_entry<host_imgui_destroy>(), abi_entry<host_imgui_font>(),
    abi_entry<host_refresh>(), abi_entry<host_timing>(), abi_entry<host_gpu_time>()};
  for (unsigned i = 0; i < std::size(replacements); ++i) host[i + 2] = replacements[i];

  return 1;
}

extern "C" void dk_native_shutdown(void) {
  using namespace nxdk;

  if (gpu().device()) gpu().finish();
  ui_shutdown();
  lsfg_shutdown();
  native_device_shutdown();
  texture_shutdown();
  reset_shader_cache();
  gpu().shutdown();
}
