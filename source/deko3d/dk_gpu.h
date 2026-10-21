#pragma once

#include <deko3d.hpp>
#include "dk_image_sync.h"
#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace nxdk {
constexpr uint32_t ScratchBytes = 0x10000;
constexpr uint32_t DescriptorCount = 16384;
constexpr uint32_t SamplerDescriptorCount = 4096;
constexpr unsigned SubmissionCount = 3;
uint32_t align_up(uint64_t value, uint32_t alignment);

inline void bind_render_targets(DkCmdBuf commands, const DkImageView *color,
                                 const DkImageView *depth) {
  dkCmdBufBindRenderTargets(commands, color ? &color : nullptr, color ? 1u : 0u, depth);
}

struct Memory {
  dk::UniqueMemBlock block;
  uint32_t size = 0;
  void *cpu() const { return dkMemBlockGetCpuAddr(block); }
  DkGpuAddr gpu() const { return dkMemBlockGetGpuAddr(block); }
};
using MemoryPtr = std::shared_ptr<Memory>;

struct Image {
  MemoryPtr memory;
  dk::Image image;
  uint32_t width = 0, height = 0, levels = 1;
  DkImageFormat format = DkImageFormat_None;
  uint64_t write_stamp = 0;
  bool is_depth() const {
    switch (format) {
      case DkImageFormat_S8: case DkImageFormat_Z16: case DkImageFormat_Z24X8:
      case DkImageFormat_ZF32: case DkImageFormat_Z24S8: case DkImageFormat_ZF32_X24S8:
        return true;
      default: return false;
    }
  }
  DkImageView view(unsigned level = 0) const;
};

struct Upload {
  void *cpu;
  DkGpuAddr gpu;
  uint32_t size;
};

struct GeneratedFrame {
  const Image *image = nullptr, *source = nullptr;
};

class Gpu {
public:
  bool initialize();
  bool create_window(void *window, uint32_t width, uint32_t height);
  void destroy_window();
  void shutdown();
  MemoryPtr allocate(uint64_t bytes, uint32_t flags = DkMemBlockFlags_CpuUncached |
                                                    DkMemBlockFlags_GpuCached);
  std::shared_ptr<Image> create_image(uint32_t width, uint32_t height, DkImageFormat format,
                                    uint32_t levels = 1, uint32_t flags = 0);
  Upload upload(uint32_t bytes, uint32_t alignment = DK_UNIFORM_BUF_ALIGNMENT);
  void retain(MemoryPtr memory);
  void submit(bool wait = false);
  void barrier(DkBarrier mode, uint32_t invalidate);
  void prepare_sampled_image(const Image *image);
  void image_written(Image *image);
  void finish();
  DkCmdBuf commands();
  DkDevice device() const { return m_device; }
  DkQueue queue() const { return m_queue; }
  uint64_t generation() const { return m_generation; }
  DkResHandle descriptor(const DkImageView &view, const DkSampler &sampler, bool storage = false);
  void reserve_descriptors(uint32_t count);
  void bind_descriptor_sets();
  Image *backbuffer();
  void present();
  void set_swap_interval(int interval);
  bool has_window() const { return bool(m_swapchain); }

private:
  using DescriptorKey = std::array<uint32_t, 16>;
  struct DescriptorHash {
    size_t operator()(const DescriptorKey &key) const {
      size_t hash = 1469598103934665603ull;
      for (uint32_t word : key) hash = (hash ^ word) * 1099511628211ull;
      return hash;
    }
  };
  struct Submission {
    Gpu *gpu = nullptr;
    dk::UniqueCmdBuf commands;
    std::vector<MemoryPtr> command_chunks;
    std::vector<MemoryPtr> upload_chunks;
    std::vector<MemoryPtr> retained;
    MemoryPtr descriptors;
    DkFence fence{};
    bool pending = false;
    uint32_t command_chunk = 0, upload_chunk = 0, upload_offset = 0;
    uint32_t descriptor_count = 0, sampler_count = 0;
    std::unordered_map<DescriptorKey, uint32_t, DescriptorHash> image_cache, sampler_cache;
  };
  static void grow_commands(void *user, DkCmdBuf commands, size_t bytes);
  void wait_for_submission(Submission &frame);
  void begin();
  void present_image(const Image &image);
  void present_backbuffer();
  dk::UniqueDevice m_device;
  dk::UniqueQueue m_queue;
  dk::UniqueSwapchain m_swapchain;
  std::array<std::shared_ptr<Image>, 3> m_backbuffers;
  std::array<Submission, SubmissionCount> m_submissions;
  unsigned m_current = 0;
  uint64_t m_generation = 0;
  bool m_presented = false;
  ImageVisibility m_image_visibility;
  int m_slot = -1, m_swap_interval = 1, m_applied_swap_interval = -1;
};
Gpu &gpu();
void invalidate_native_state();
void reset_shader_cache();
void lsfg_shutdown();
void lsfg_prepare();
GeneratedFrame lsfg_generate(const Image &source);
}
