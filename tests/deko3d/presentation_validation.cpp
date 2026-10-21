#include "dk_gpu.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {
unsigned checked = 0, acquired = 0, copied = 0, next_slot = 0;
int held_slot = -1;
unsigned interval = 1;
bool display_sync_active = false;
unsigned sync_present_notifications = 0;
bool fail_acquire = false;
std::array<const DkImage *, 3> slots{};
std::unordered_map<const DkImage *, unsigned> pixels;
struct Presented { unsigned value, interval; };
std::vector<Presented> presented;
std::vector<bool> waited;
nxdk::GeneratedFrame generated;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
  ++checked;
}
}

extern "C" {
bool saltysd_display_sync_active(void) { return display_sync_active; }
float saltysd_display_sync_rate(void) { return display_sync_active ? 50.0f : 0.0f; }
void saltysd_display_sync_note_present(void) {
  if (display_sync_active) ++sync_present_notifications;
}
int dkQueueAcquireImage(DkQueue, DkSwapchain) {
  require(held_slot < 0, "acquired a second display buffer before presenting the first");
  if (fail_acquire) return -1;
  ++acquired;
  return held_slot = next_slot++ % slots.size();
}
void dkQueuePresentImage(DkQueue, DkSwapchain, int slot) {
  require(held_slot == slot, "presented a display buffer that was not acquired");
  presented.push_back({pixels.at(slots[slot]), interval});
  held_slot = -1;
}
void dkSwapchainSetSwapInterval(DkSwapchain, uint32_t value) { interval = value; }
void dkCmdBufBlitImage(DkCmdBuf, const DkImageView *source, const DkImageRect *sr,
    const DkImageView *destination, const DkImageRect *dr, uint32_t, uint32_t) {
  require(held_slot >= 0 && destination->pImage == slots[held_slot],
      "copied into a display buffer that was not acquired");
  require(sr->width == 1280 && sr->height == 720 && dr->width == 1280 && dr->height == 720,
      "presentation changed the image dimensions");
  pixels[destination->pImage] = pixels.at(source->pImage);
  ++copied;
}
void dkDeviceDestroy(DkDevice) {}
void dkQueueDestroy(DkQueue) {}
void dkSwapchainDestroy(DkSwapchain) {}
void dkCmdBufDestroy(DkCmdBuf) {}
void dkMemBlockDestroy(DkMemBlock) {}
}

namespace nxdk {
bool Gpu::create_window(void *window, uint32_t width, uint32_t height) {
  if (!window) return false;
  m_queue = dk::Queue{reinterpret_cast<DkQueue>(uintptr_t(1))};
  m_swapchain = dk::Swapchain{reinterpret_cast<DkSwapchain>(uintptr_t(2))};
  for (unsigned i = 0; i < m_backbuffers.size(); ++i) {
    m_backbuffers[i] = std::make_shared<Image>();
    m_backbuffers[i]->width = width;
    m_backbuffers[i]->height = height;
    slots[i] = &m_backbuffers[i]->image;
  }
  return true;
}
DkCmdBuf Gpu::commands() { return reinterpret_cast<DkCmdBuf>(uintptr_t(3)); }
void Gpu::barrier(DkBarrier, uint32_t) {}
void Gpu::image_written(Image *image) { ++image->write_stamp; }
void Gpu::submit(bool wait) {
  require(held_slot >= 0, "submitted a presentation without an acquired display buffer");
  waited.push_back(wait);
}
DkImageView Image::view(unsigned level) const {
  DkImageView result;
  dkImageViewDefaults(&result, &image);
  result.mipLevelOffset = level;
  result.mipLevelCount = 1;
  return result;
}
GeneratedFrame lsfg_generate(const Image &source) {
  if (generated.image) {
    require(generated.source && generated.source != &source,
        "LSFG must retain the real frame outside the display buffer");
    pixels[&generated.source->image] = pixels.at(&source.image);
  }
  return generated;
}
}

int main() {
  try {
    nxdk::Gpu gpu;
    gpu.present();
    require(!acquired && presented.empty(), "presented without a window");
    require(gpu.create_window(reinterpret_cast<void *>(uintptr_t(1)), 1280, 720),
        "could not initialize presentation fixture");
    gpu.set_swap_interval(2);
    auto render = [&](unsigned value) {
      auto *target = gpu.backbuffer();
      require(target != nullptr, "could not acquire the real frame's display buffer");
      pixels[&target->image] = value;
      gpu.present();
      require(held_slot < 0, "presentation left a display buffer acquired");
    };
    for (unsigned i = 1; i <= 2; ++i) {
      render(i);
      require(presented.back().value == i && presented.back().interval == 2,
          "warmup presented the wrong real frame or interval");
    }
    require(acquired == 2 && !copied && presented.size() == 2,
        "warmup introduced extra copies or display buffer acquisitions");

    nxdk::Image interpolated;
    interpolated.width = 1280;
    interpolated.height = 720;
    std::array<nxdk::Image, 2> history;
    for (auto &image : history) { image.width = 1280; image.height = 720; }
    for (unsigned i = 3; i <= 14; ++i) {
      generated = {&interpolated, &history[i % history.size()]};
      pixels[&interpolated.image] = 1000 + i;
      const unsigned previous_acquired = acquired;
      const size_t previous_presented = presented.size();
      render(i);
      require(acquired == previous_acquired + 2 && presented.size() == previous_presented + 2,
          "frame generation acquired or presented the wrong number of display buffers");
      require(presented[previous_presented].value == 1000 + i && presented.back().value == i,
          "generated and real frames were lost, reversed or reused");
      require(presented[previous_presented].interval == 1 && presented.back().interval == 1 && interval == 2,
          "frame generation did not restore the configured swap interval");
      require(pixels.at(&generated.source->image) == i,
          "the real frame history changed when the generated frame was presented");
    }
    require(copied == 24, "frame generation introduced unnecessary presentation copies");
    generated = {};
    render(15);
    require(presented.back().value == 15 && presented.back().interval == 2 && copied == 24,
        "disabling frame generation did not restore direct presentation");
    require(waited.front(), "initial presentation was submitted before initialization completed");
    for (size_t i = 1; i < waited.size(); ++i)
      require(!waited[i], "steady-state presentation introduced a CPU wait");

    const unsigned notes_before_sync = sync_present_notifications;
    display_sync_active = true;
    render(16);
    require(interval == 1 && presented.back().interval == 1,
        "active display sync did not force swap interval one");
    require(sync_present_notifications == notes_before_sync + 1,
        "a successful synced present was not timestamped for fallback pacing");
    display_sync_active = false;
    render(17);
    require(interval == 2 && presented.back().interval == 2,
        "disabling display sync did not restore the configured swap interval");
    require(sync_present_notifications == notes_before_sync + 1,
        "present timestamps continued to be sampled after sync was disabled");

    const size_t previous_presented = presented.size();
    fail_acquire = true;
    gpu.present();
    require(presented.size() == previous_presented && held_slot < 0,
        "failed acquisition submitted or presented an invalid display buffer");
    std::cout << checked << " presentation checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
