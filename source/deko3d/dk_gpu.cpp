#include "dk_gpu.h"
#include "../saltysd_display.h"
#include "dk_render_state.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <switch.h>

extern "C" {
#include "../config.h"
#include "../error.h"
}

namespace nxdk {
namespace {
void device_error(void *, const char *context, DkResult result, const char *message) {
  if (result != DkResult_Success)
    fatal_error("Deko3D [%s]: %s", context ? context : "GPU", message ? message : "GPU error");
}
}
uint32_t align_up(uint64_t value, uint32_t alignment) {
  if (!alignment || (alignment & (alignment - 1)) ||
      value > UINT32_MAX - uint64_t(alignment - 1))
    return 0;
  return static_cast<uint32_t>((value + alignment - 1) & ~(uint64_t(alignment) - 1));
}

Gpu &gpu() { static Gpu instance; return instance; }

DkImageView Image::view(unsigned level) const {
  DkImageView result;
  dkImageViewDefaults(&result, &image);
  result.mipLevelOffset = level;
  result.mipLevelCount = 1;
  return result;
}

MemoryPtr Gpu::allocate(uint64_t bytes, uint32_t flags) {
  const uint32_t size = align_up(bytes, DK_MEMBLOCK_ALIGNMENT);
  if (!size || !m_device) return {};
  auto memory = std::make_shared<Memory>();
  memory->block = dk::MemBlockMaker{m_device, size}.setFlags(flags).create();
  if (!memory->block) return {};
  memory->size = size;
  return memory;
}

std::shared_ptr<Image> Gpu::create_image(uint32_t width, uint32_t height, DkImageFormat format,
                                       uint32_t levels, uint32_t flags) {
  if (!width || !height || width > 16384 || height > 16384 || !levels || levels > 15 ||
      format == DkImageFormat_None) return {};
  auto result = std::make_shared<Image>();
  dk::ImageLayout layout;
  dk::ImageLayoutMaker{m_device}.setFlags(flags).setFormat(format)
      .setDimensions(width, height).setMipLevels(levels).initialize(layout);
  result->memory = allocate(layout.getSize(), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
  if (!result->memory) return {};
  result->image.initialize(layout, result->memory->block, 0);
  result->width = width;
  result->height = height;
  result->levels = levels;
  result->format = format;
  return result;
}

void Gpu::grow_commands(void *user, DkCmdBuf commands, size_t bytes) {
  auto &frame = *static_cast<Submission *>(user);
  const uint32_t size = align_up(std::max<size_t>(bytes, 1024 * 1024), DK_MEMBLOCK_ALIGNMENT);
  if (!size) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
  if (frame.command_chunk == frame.command_chunks.size()) frame.command_chunks.emplace_back();
  auto &memory = frame.command_chunks[frame.command_chunk++];
  if (!memory || memory->size < size) memory = frame.gpu->allocate(size);
  if (!memory) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
  dkCmdBufAddMemory(commands, memory->block, 0, size);
}

bool Gpu::initialize() {
  if (m_device) return true;

  m_device = dk::DeviceMaker{}.setFlags(NativeDeviceFlags)
      .setCbDebug(device_error).create();
  if (!m_device) return false;

  m_queue = dk::QueueMaker{m_device}.setFlags(DkQueueFlags_Graphics | DkQueueFlags_Compute)
      .setCommandMemorySize(512 * 1024).setFlushThreshold(64 * 1024)
      .setPerWarpScratchMemorySize(ScratchBytes).create();
  if (!m_queue) { shutdown(); return false; }
  for (auto &frame : m_submissions) {
    frame.gpu = this;
    frame.descriptors = allocate(uint64_t(DescriptorCount) * sizeof(DkImageDescriptor) +
                                uint64_t(SamplerDescriptorCount) * sizeof(DkSamplerDescriptor));
    frame.commands = dk::CmdBufMaker{m_device}.setUserData(&frame)
        .setCbAddMem(grow_commands).create();
    if (!frame.descriptors || !frame.commands) { shutdown(); return false; }
  }
  begin();

  submit(true);

  return true;
}

void Gpu::wait_for_submission(Submission &frame) {
  if (!frame.pending) return;
  const DkResult result = dkFenceWait(&frame.fence, 5000000000LL);

  const bool queue_error = dkQueueIsInErrorState(m_queue);
  if (result != DkResult_Success || queue_error)
    fatal_error("Deko3D: GPU submission failed (result %d, queue_error=%u).", int(result), queue_error);
  frame.pending = false;
}

void Gpu::begin() {
  auto &frame = m_submissions[m_current];
  wait_for_submission(frame);
  frame.retained.clear();
  frame.command_chunk = frame.upload_chunk = frame.upload_offset = 0;
  frame.descriptor_count = frame.sampler_count = 0;
  frame.image_cache.clear();
  frame.sampler_cache.clear();
  frame.commands.clear();
  grow_commands(&frame, frame.commands, 1024 * 1024);
  dkCmdBufBarrier(frame.commands, DkBarrier_None,
                  DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader);
  ++m_generation;
  invalidate_native_state();
}

DkCmdBuf Gpu::commands() { return m_submissions[m_current].commands; }

void Gpu::barrier(DkBarrier mode, uint32_t invalidate) {
  dkCmdBufBarrier(commands(), mode, invalidate);
  m_image_visibility.barrier(mode, invalidate);
}

void Gpu::prepare_sampled_image(const Image *image) {
  if (image && m_image_visibility.needs_barrier(image->write_stamp))
    barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
}

void Gpu::image_written(Image *image) {
  if (image) m_image_visibility.written(image->write_stamp);
}

void Gpu::retain(MemoryPtr memory) {
  if (memory) m_submissions[m_current].retained.push_back(std::move(memory));
}

Upload Gpu::upload(uint32_t bytes, uint32_t alignment) {
  auto &frame = m_submissions[m_current];
  uint32_t offset = align_up(frame.upload_offset, alignment);
  for (;;) {
    if (frame.upload_chunk == frame.upload_chunks.size()) frame.upload_chunks.emplace_back();
    auto &memory = frame.upload_chunks[frame.upload_chunk];
    if (!memory || (!offset && memory->size < bytes))
      memory = allocate(std::max<uint32_t>(8 * 1024 * 1024, bytes));
    if (!memory) return {};
    if (uint64_t(offset) + bytes <= memory->size) {
      frame.upload_offset = offset + bytes;
      return {static_cast<uint8_t *>(memory->cpu()) + offset, memory->gpu() + offset, bytes};
    }
    ++frame.upload_chunk;
    frame.upload_offset = offset = 0;
  }
}

DkResHandle Gpu::descriptor(const DkImageView &view, const DkSampler &sampler, bool storage) {
  auto &frame = m_submissions[m_current];
  auto *images = static_cast<DkImageDescriptor *>(frame.descriptors->cpu());
  auto *samplers = reinterpret_cast<DkSamplerDescriptor *>(images + DescriptorCount);
  DkImageDescriptor image{};
  DkSamplerDescriptor sampling{};
  dkImageDescriptorInitialize(&image, &view, storage, false);
  dkSamplerDescriptorInitialize(&sampling, &sampler);
  static_assert(sizeof(image) <= sizeof(DescriptorKey));
  static_assert(sizeof(sampling) <= sizeof(DescriptorKey));
  DescriptorKey image_key{}, sampler_key{};
  memcpy(image_key.data(), &image, sizeof(image));
  memcpy(sampler_key.data(), &sampling, sizeof(sampling));
  auto [image_entry, new_image] = frame.image_cache.try_emplace(image_key, frame.descriptor_count);
  if (new_image) {
    if (frame.descriptor_count == DescriptorCount)
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
    images[frame.descriptor_count++] = image;
  }
  if (storage) return dkMakeImageHandle(image_entry->second);
  auto [sampler_entry, new_sampler] = frame.sampler_cache.try_emplace(sampler_key, frame.sampler_count);
  if (new_sampler) {
    if (frame.sampler_count == SamplerDescriptorCount)
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
    samplers[frame.sampler_count++] = sampling;
  }
  return dkMakeTextureHandle(image_entry->second, sampler_entry->second);
}

void Gpu::reserve_descriptors(uint32_t count) {
  const auto &frame = m_submissions[m_current];
  if (count > SamplerDescriptorCount)
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
  if (frame.descriptor_count + count > DescriptorCount ||
      frame.sampler_count + count > SamplerDescriptorCount) submit();
}

void Gpu::bind_descriptor_sets() {
  const auto &memory = m_submissions[m_current].descriptors;
  dkCmdBufBindImageDescriptorSet(commands(), memory->gpu(), DescriptorCount);
  dkCmdBufBindSamplerDescriptorSet(commands(), memory->gpu() +
                                 DescriptorCount * sizeof(DkImageDescriptor), SamplerDescriptorCount);
}

void Gpu::submit(bool wait) {
  if (!m_queue) return;
  auto &frame = m_submissions[m_current];

  dkCmdBufSignalFence(frame.commands, &frame.fence, true);
  m_queue.submitCommands(frame.commands.finishList());
  m_queue.flush();
  if (dkQueueIsInErrorState(m_queue))
    fatal_error("Deko3D: GPU submission left the queue in error state.");
  frame.pending = true;
  if (wait) wait_for_submission(frame);
  m_current = (m_current + 1) % SubmissionCount;
  begin();
}

void Gpu::finish() { if (m_queue) submit(true); }

bool Gpu::create_window(void *window, uint32_t width, uint32_t height) {
  destroy_window();
  if (!window) return false;
  if (!initialize()) return false;
  std::array<const DkImage *, 3> images;
  for (unsigned i = 0; i < images.size(); ++i) {
    m_backbuffers[i] = create_image(width, height, DkImageFormat_RGBA8_Unorm, 1,
        DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine);
    if (!m_backbuffers[i]) { destroy_window(); return false; }
    images[i] = &m_backbuffers[i]->image;
  }

  m_swapchain = dk::SwapchainMaker{m_device, window, images}.create();
  if (!m_swapchain) { destroy_window(); return false; }
  m_applied_swap_interval = -1;
  set_swap_interval(m_swap_interval);

  return true;
}

void Gpu::destroy_window() {
  if (m_swapchain) finish();
  m_slot = -1;
  m_swapchain = nullptr;
  m_applied_swap_interval = -1;
  m_backbuffers = {};
  invalidate_native_state();
}

void Gpu::shutdown() {
  if (m_queue) {
    DkFence fence{};
    m_queue.signalFence(fence, true);
    m_queue.flush();
    const DkResult result = dkFenceWait(&fence, 5000000000LL);
    if (result != DkResult_Success || dkQueueIsInErrorState(m_queue))
      fatal_error("Deko3D: shutdown fence failed (result %d).", int(result));
  }
  m_swapchain = nullptr;
  m_backbuffers = {};
  for (auto &frame : m_submissions) {
    frame.commands = nullptr;
    frame.command_chunks.clear();
    frame.upload_chunks.clear();
    frame.retained.clear();
    frame.descriptors.reset();
    frame.pending = false;
  }
  m_queue = nullptr;
  m_device = nullptr;
  m_current = 0;
  m_presented = false;
  m_image_visibility = {};
  m_slot = -1;
}
}
