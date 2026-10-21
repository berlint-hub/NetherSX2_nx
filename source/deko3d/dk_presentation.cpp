#include "dk_gpu.h"
#include "../saltysd_display.h"
#include <algorithm>

namespace nxdk {
Image *Gpu::backbuffer() {
  if (!m_swapchain) return nullptr;
  if (m_slot < 0) m_slot = m_queue.acquireImage(m_swapchain);
  if (m_slot < 0 || m_slot >= int(m_backbuffers.size())) return nullptr;
  return m_backbuffers[m_slot].get();
}

void Gpu::present() {
  set_swap_interval(m_swap_interval);
  Image *frame = backbuffer();
  if (!frame) return;
  const GeneratedFrame generated = lsfg_generate(*frame);
  if (generated.image) {
    m_swapchain.setSwapInterval(1);
    m_applied_swap_interval = 1;
    present_image(*generated.image);
    present_image(*generated.source);
    set_swap_interval(m_swap_interval);
  } else {
    present_backbuffer();
  }
}

void Gpu::present_image(const Image &image) {
  Image *target = backbuffer();
  if (!target) return;
  const DkImageView source = image.view(), destination = target->view();
  const DkImageRect rect{0, 0, 0, image.width, image.height, 1};
  barrier(DkBarrier_Full, DkInvalidateFlags_Image);
  dkCmdBufBlitImage(commands(), &source, &rect, &destination, &rect, 0, 0);
  image_written(target);
  present_backbuffer();
}

void Gpu::present_backbuffer() {
  const int slot = m_slot;
  if (slot < 0) return;
  submit(!m_presented);
  m_queue.presentImage(m_swapchain, slot);
  saltysd_display_sync_note_present();
  m_presented = true;
  m_slot = -1;
}

void Gpu::set_swap_interval(int interval) {
  m_swap_interval = std::max(0, interval);
  const int effective_interval = saltysd_display_sync_active() ? 1 : m_swap_interval;
  if (m_swapchain && effective_interval != m_applied_swap_interval) {
    m_swapchain.setSwapInterval(effective_interval);
    m_applied_swap_interval = effective_interval;
  }
}
}
