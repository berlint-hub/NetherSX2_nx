#include "dk_gpu.h"
#include "dk_backend.h"
#include <lsfg/backend/binding.hpp>
#include <lsfg/backend/cache_load.hpp>
#include <lsfg/backend/schedule.hpp>
#include <lsfg/common/dksh.hpp>
#include <lsfg/common/prepare.hpp>
#include <lsfg/common/version.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <memory>
#include <switch.h>

extern "C" {
#include "../config.h"
#include "../prefs.h"
}

namespace nxdk {
namespace {
namespace backend = lsfg::backend;
namespace graph = lsfg::graph;
std::atomic<int> available{0}, requested{0}, high_rate{0};
uint64_t last_source = 0;
double source_interval = 0;
unsigned source_samples = 0, slow_samples = 0;
int rate_decision = -1;
bool previous_request = false;

uint64_t monotonic_ns() {
  timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return uint64_t(value.tv_sec) * 1000000000 + value.tv_nsec;
}

class Session {
public:
  graph::Config config;
  lsfg::cache::Loaded cache;
  backend::Schedule schedule;
  backend::Plan plan;
  backend::DescriptorLayout descriptors;
  MemoryPtr code, image_memory, descriptor_memory, uniform_memory;
  std::vector<DkShader> shaders;
  std::vector<Image> images;
  std::vector<uint8_t> hazards;
  unsigned frame = 0, warmup = 0, generated = UINT32_MAX;
  uint32_t width = 0, height = 0;

  bool load_shaders() {
    backend::Arena arena;
    std::vector<uint32_t> offsets;
    for (const auto &pass : cache.passes) {
      lsfg::dksh::FileHeader header;
      memcpy(&header, pass.dksh.data(), sizeof(header));
      offsets.push_back(arena.place(header.code_size + DK_SHADER_CODE_UNUSABLE_SIZE,
                                    DK_SHADER_CODE_ALIGNMENT));
    }
    if (arena.overflowed()) return false;
    code = gpu().allocate(arena.block_size(), DkMemBlockFlags_CpuUncached |
        DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    if (!code) return false;
    shaders.resize(cache.passes.size());
    for (unsigned i = 0; i < cache.passes.size(); ++i) {
      const auto &pass = cache.passes[i];
      lsfg::dksh::FileHeader header;
      memcpy(&header, pass.dksh.data(), sizeof(header));
      memcpy(static_cast<uint8_t *>(code->cpu()) + offsets[i],
             pass.dksh.data() + header.control_size, header.code_size);
      DkShaderMaker maker;
      dkShaderMakerDefaults(&maker, code->block, offsets[i]);
      maker.control = pass.dksh.data();
      dkShaderInitialize(&shaders[i], &maker);
      if (!dkShaderIsValid(&shaders[i])) return false;
    }
    return true;
  }

  bool allocate(uint32_t w, uint32_t h) {
    gpu().finish();
    images.clear();
    image_memory.reset();
    descriptor_memory.reset();
    uniform_memory.reset();
    width = height = 0;
    const backend::Request request{.config=config, .precision=lsfg::cache::Precision::high,
        .output={w,h}, .memory_budget_bytes=128ull * 1024 * 1024};
    backend::Rejection why;
    if (!backend::accept(cache, request, plan, why)) {
      return false;
    }
    if (plan.max_scratch_bytes_per_warp > ScratchBytes ||
        !lsfg::succeeded(backend::describe(cache.graph, descriptors))) return false;

    backend::Arena arena;
    std::vector<dk::ImageLayout> layouts(plan.images.size());
    std::vector<uint32_t> offsets;
    generated = UINT32_MAX;
    for (unsigned i = 0; i < plan.images.size(); ++i) {
      const auto &image = plan.images[i];
      const DkImageFormat format = image.format == graph::Format::rgba8 ? DkImageFormat_RGBA8_Unorm :
          image.format == graph::Format::r8 ? DkImageFormat_R8_Unorm : DkImageFormat_RGBA16_Float;
      const uint32_t flags = DkImageFlags_UsageLoadStore |
          (image.role == graph::ImageRole::history || image.role == graph::ImageRole::generated ?
              DkImageFlags_Usage2DEngine : 0);
      dk::ImageLayoutMaker{gpu().device()}.setFormat(format).setFlags(flags)
          .setDimensions(image.extent.width, image.extent.height).initialize(layouts[i]);
      offsets.push_back(arena.place(layouts[i].getSize(), layouts[i].getAlignment()));
      if (image.role == graph::ImageRole::generated && generated == UINT32_MAX) generated = i;
    }
    if (arena.overflowed() || arena.block_size() > request.memory_budget_bytes || generated == UINT32_MAX)
      return false;
    image_memory = gpu().allocate(arena.block_size(), DkMemBlockFlags_GpuCached |
                                  DkMemBlockFlags_Image | DkMemBlockFlags_ZeroFillInit);
    descriptor_memory = gpu().allocate(uint64_t(descriptors.image_descriptors +
                                               backend::sampler_descriptor_count) * sizeof(DkImageDescriptor));
    if (plan.uniform_buffers)
      uniform_memory = gpu().allocate(uint64_t(plan.uniform_buffers) * backend::uniform_buffer_stride);
    if (!image_memory || !descriptor_memory || (plan.uniform_buffers && !uniform_memory)) return false;
    images.resize(plan.images.size());
    auto *table = static_cast<DkImageDescriptor *>(descriptor_memory->cpu());
    for (unsigned i = 0; i < images.size(); ++i) {
      const auto &layout = descriptors.images[i];
      auto &image = images[i];
      image.image.initialize(layouts[i], image_memory->block, offsets[i]);
      image.width = plan.images[i].extent.width;
      image.height = plan.images[i].extent.height;
      const auto view = image.view();
      if (layout.sampled != backend::no_descriptor)
        dkImageDescriptorInitialize(&table[layout.sampled], &view, false, false);
      if (layout.storage != backend::no_descriptor)
        dkImageDescriptorInitialize(&table[layout.storage], &view, true, false);
    }
    auto *samplers = reinterpret_cast<DkSamplerDescriptor *>(table + descriptors.image_descriptors);
    for (unsigned i = 0; i < backend::sampler_descriptor_count; ++i) {
      DkSampler sampler;
      dkSamplerDefaults(&sampler);
      sampler.minFilter = sampler.magFilter = DkFilter_Linear;
      sampler.mipFilter = DkMipFilter_Linear;
      const bool edge = i == unsigned(graph::Sampler::edge) || i == backend::introduced_sampler;
      for (auto &wrap : sampler.wrapMode) wrap = edge ? DkWrapMode_ClampToEdge : DkWrapMode_ClampToBorder;
      if (i == unsigned(graph::Sampler::border_white))
        for (auto &component : sampler.borderColor) component.value_f = 1.0f;
      dkSamplerDescriptorInitialize(&samplers[i], &sampler);
    }
    for (unsigned i = 0; i < plan.uniform_buffers; ++i) {
      const auto constants = graph::constant_buffer(i, config);
      memcpy(static_cast<uint8_t *>(uniform_memory->cpu()) + i * backend::uniform_buffer_stride,
             &constants, sizeof(constants));
    }
    hazards.assign(images.size(), 0);
    frame = warmup = 0;
    width = w; height = h;

    return true;
  }

  GeneratedFrame generate(const Image &source) {
    if ((source.width != width || source.height != height) && !allocate(source.width, source.height)) return {};
    const auto src = source.view(), dst = images[frame % graph::history_image_count].view();
    const DkImageRect rect{0,0,0,width,height,1};
    const DkCmdBuf commands = gpu().commands();
    gpu().barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    dkCmdBufBlitImage(commands, &src, &rect, &dst, &rect, 0, 0);
    dkCmdBufBindImageDescriptorSet(commands, descriptor_memory->gpu(), descriptors.image_descriptors);
    dkCmdBufBindSamplerDescriptorSet(commands, descriptor_memory->gpu() +
        descriptors.image_descriptors * sizeof(DkImageDescriptor), backend::sampler_descriptor_count);
    gpu().barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    std::fill(hazards.begin(), hazards.end(), 0);
    for (unsigned dispatch = 0; dispatch < cache.graph.dispatches.size(); ++dispatch) {
      backend::DispatchBinding binding;
      if (!lsfg::succeeded(backend::bind(cache, plan, descriptors, dispatch, frame, binding))) {
        width = height = 0;
        return {};
      }
      bool barrier = false;
      uint32_t invalidate = 0;
      for (unsigned i = 0; i < binding.texture_count; ++i)
        if (hazards[binding.textures[i].image] & 2) { barrier = true; invalidate |= DkInvalidateFlags_Image; }
      for (unsigned i = 0; i < binding.storage_count; ++i)
        if (hazards[binding.storages[i].image]) barrier = true;
      if (barrier) {
        gpu().barrier(DkBarrier_Primitives, invalidate);
        std::fill(hazards.begin(), hazards.end(), 0);
      }
      std::array<DkResHandle, backend::max_texture_slots> textures{};
      std::array<DkResHandle, backend::max_storage_slots> storages{};
      unsigned texture_slots = 0, storage_slots = 0;
      for (unsigned i = 0; i < binding.texture_count; ++i) {
        const auto &texture = binding.textures[i];
        textures[texture.slot] = dkMakeTextureHandle(texture.descriptor, texture.sampler);
        texture_slots = std::max(texture_slots, texture.slot + 1);
        hazards[texture.image] |= 1;
      }
      for (unsigned i = 0; i < binding.storage_count; ++i) {
        const auto &storage = binding.storages[i];
        storages[storage.slot] = dkMakeImageHandle(storage.descriptor);
        storage_slots = std::max(storage_slots, storage.slot + 1);
        hazards[storage.image] |= 2;
      }
      const DkShader *shader = &shaders[binding.pass];
      dkCmdBufBindShaders(commands, DkStageFlag_Compute, &shader, 1);
      if (texture_slots) dkCmdBufBindTextures(commands, DkStage_Compute, 0, textures.data(), texture_slots);
      if (storage_slots) dkCmdBufBindImages(commands, DkStage_Compute, 0, storages.data(), storage_slots);
      if (binding.uniform_slot != backend::no_slot)
        dkCmdBufBindUniformBuffer(commands, DkStage_Compute, binding.uniform_slot,
            uniform_memory->gpu() + binding.uniform_buffer * backend::uniform_buffer_stride,
            backend::uniform_buffer_stride);
      dkCmdBufDispatchCompute(commands, binding.groups_x, binding.groups_y, 1);
    }
    gpu().barrier(DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    invalidate_native_state();
    const GeneratedFrame result{&images[generated], &images[frame % graph::history_image_count]};
    frame = (frame + 1) % schedule.cycle;
    return ++warmup > schedule.warmup_frames ? result : GeneratedFrame{};
  }
};
std::unique_ptr<Session> session;
}

void lsfg_shutdown() {
  available = requested = high_rate = 0;
  if (session && gpu().device()) gpu().finish();
  session.reset();
  last_source = 0;
  source_interval = 0;
  source_samples = slow_samples = 0;
  rate_decision = -1;
  previous_request = false;
}

void lsfg_prepare() {
  lsfg_shutdown();
  if (!prefs_get_bool("Wrapper/LSFGEnabled", false)) return;
  const char *path = DATA_ROOT "/lsfg/Lossless.dll";
  FILE *file = fopen(path, "rb");
  if (!file) return;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  rewind(file);
  std::vector<uint8_t> dll;
  if (size > 0 && size <= 128 * 1024 * 1024) dll.resize(size);
  const bool read = !dll.empty() && fread(dll.data(), 1, dll.size(), file) == dll.size();
  fclose(file);
  if (!read) return;
  auto prepared = std::make_unique<Session>();
  prepared->config.performance = prefs_get_bool("Wrapper/LSFGPerformance", true);
  const double flow_scale = atof(prefs_get_string("Wrapper/LSFGFlowScale", "0.25"));
  prepared->config.flow_denominator = flow_scale >= 1.0 ? 1 :
      flow_scale >= 0.5 ? 2 : flow_scale >= 0.25 ? 4 : 8;
  lsfg::prepare::Options options;
  options.graph.performance = prepared->config.performance;
  const auto key = lsfg::cache::cache_key({dll, lsfg::cache::extractor_version,
      lsfg::version::spirv_cross_revision, lsfg::version::uam_revision,
      options.translation.glsl_version, lsfg::cache::pack_options(lsfg::cache::Precision::high, options.graph),
      lsfg::cache::backend_abi_version});
  const std::string directory = lsfg::cache::directory_for(DATA_ROOT "/lsfg/deko3d", key);
  if (!lsfg::succeeded(lsfg::cache::read(directory, prepared->cache))) {
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    lsfg::prepare::Result result;

    auto status = lsfg::prepare::run(dll, options, result);
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if (lsfg::succeeded(status)) status = lsfg::cache::write(directory, result.contents);
    if (lsfg::succeeded(status)) status = lsfg::cache::read(directory, prepared->cache);
    if (!lsfg::succeeded(status)) return;
    if (!lsfg::cache::same_modules(prepared->cache, result.contents)) return;
  }
  if (!lsfg::succeeded(graph::build(prepared->config, prepared->cache.graph))) return;
  lsfg::cache::describe(prepared->cache.header, prepared->cache.graph);
  if (!lsfg::succeeded(backend::schedule(prepared->cache.graph, prepared->schedule)) ||
      !prepared->load_shaders()) { return; }
  session = std::move(prepared);
  available = 1;

  requested = 0;
}

GeneratedFrame lsfg_generate(const Image &source) {
  const bool enabled = requested && available;
  const uint64_t now = monotonic_ns();
  uint64_t interval = last_source && now > last_source ? now - last_source : 0;
  last_source = now;
  if ((!enabled || rate_decision != 0) && interval >= 4000000 && interval <= 100000000) {
    source_interval = source_samples ? source_interval * 0.875 + interval * 0.125 : double(interval);
    if (source_samples < 120) ++source_samples;
  }
  if (enabled != previous_request) {
    previous_request = enabled;
    rate_decision = -1;
    slow_samples = 0;
    if (session) session->warmup = 0;
  }
  if (!enabled || !session) { high_rate = 0; return {}; }
  if (rate_decision < 0 && source_samples >= 8) rate_decision = source_interval < 30000000 ? 1 : 0;
  if (rate_decision == 1) {
    high_rate = 1;
    if (interval >= 31500000 && interval <= 100000000) ++slow_samples;
    else slow_samples = 0;
    if (slow_samples < 16) return {};
    rate_decision = 0;
  }
  high_rate = 0;
  if (rate_decision < 0) return {};
  const GeneratedFrame result = session->generate(source);
  if (!session->width) {
    available = requested = 0;
  }
  return result;
}
}

extern "C" int dk_lsfg_is_available(void) { return nxdk::available.load(); }
extern "C" int dk_lsfg_is_enabled(void) { return nxdk::requested.load() && nxdk::available.load(); }
extern "C" int dk_lsfg_is_high_fps_passthrough(void) { return nxdk::high_rate.load(); }
extern "C" void dk_lsfg_request_enabled(int enabled) { nxdk::requested = enabled && nxdk::available; }
