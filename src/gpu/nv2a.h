// The NV2A GPU at its register level: MMIO registers, the DMA pusher that walks D3D's push buffer,
// interrupts and vertical blank. The XDK's Direct3D runs unmodified (recompiled) on top of it.
#pragma once

#include <cstdint>

namespace gpu {

constexpr uint32_t kMmioBase = 0xFD000000;
constexpr uint32_t kMmioSize = 0x01000000;

// register blocks (offsets into the MMIO range)
namespace reg {
constexpr uint32_t PMC_BOOT_0 = 0x000000;
constexpr uint32_t PMC_INTR_0 = 0x000100;
constexpr uint32_t PMC_INTR_EN_0 = 0x000140;

constexpr uint32_t PFIFO_INTR_0 = 0x002100;
constexpr uint32_t PFIFO_INTR_EN_0 = 0x002140;
constexpr uint32_t PFIFO_RAMHT = 0x002210;
constexpr uint32_t PFIFO_RUNOUT_STATUS = 0x002400;
constexpr uint32_t PFIFO_CACHE1_PUSH1 = 0x003204;
constexpr uint32_t PFIFO_CACHE1_STATUS = 0x003214;
constexpr uint32_t PFIFO_CACHE1_DMA_PUSH = 0x003220;
constexpr uint32_t PFIFO_CACHE1_DMA_STATE = 0x003228;
constexpr uint32_t PFIFO_CACHE1_DMA_INSTANCE = 0x00322C;
constexpr uint32_t PFIFO_CACHE1_DMA_PUT = 0x003240;
constexpr uint32_t PFIFO_CACHE1_DMA_GET = 0x003244;
constexpr uint32_t PFIFO_CACHE1_REF = 0x003248;
constexpr uint32_t PFIFO_CACHE1_DMA_SUBROUTINE = 0x00324C;

// video overlay (Bink plays through it)
constexpr uint32_t PVIDEO_BUFFER = 0x008700;  // bits of the buffers queued for display
constexpr uint32_t PVIDEO_STOP = 0x008704;
constexpr uint32_t PVIDEO_BASE_0 = 0x008900;
constexpr uint32_t PVIDEO_OFFSET_0 = 0x008920;
constexpr uint32_t PVIDEO_SIZE_IN_0 = 0x008928;
constexpr uint32_t PVIDEO_POINT_OUT_0 = 0x008948;
constexpr uint32_t PVIDEO_SIZE_OUT_0 = 0x008950;
constexpr uint32_t PVIDEO_FORMAT_0 = 0x008958;

constexpr uint32_t PTIMER_INTR_0 = 0x009100;
constexpr uint32_t PTIMER_INTR_EN_0 = 0x009140;
constexpr uint32_t PTIMER_NUMERATOR = 0x009200;
constexpr uint32_t PTIMER_DENOMINATOR = 0x009210;
constexpr uint32_t PTIMER_TIME_0 = 0x009400;
constexpr uint32_t PTIMER_TIME_1 = 0x009410;
constexpr uint32_t PTIMER_ALARM_0 = 0x009420;  // compared with TIME_0; D3D paces vblank with it in some modes

constexpr uint32_t PFB_CFG0 = 0x100200;
constexpr uint32_t PFB_CSTATUS = 0x10020C;
constexpr uint32_t PFB_WBC = 0x100410;  // write-back cache, bit 16 flushes

// per channel FIFO control the driver writes PUT through (channel 0)
constexpr uint32_t USER_DMA_PUT = 0x800040;
constexpr uint32_t USER_DMA_GET = 0x800044;
constexpr uint32_t USER_REF = 0x800048;

constexpr uint32_t PGRAPH_INTR = 0x400100;
constexpr uint32_t PGRAPH_NSOURCE = 0x400108;
constexpr uint32_t PGRAPH_INTR_EN = 0x400140;
constexpr uint32_t PGRAPH_CTX_USER = 0x400148;
constexpr uint32_t PGRAPH_STATUS = 0x400700;
constexpr uint32_t PGRAPH_TRAPPED_ADDR = 0x400704;
constexpr uint32_t PGRAPH_TRAPPED_DATA_LOW = 0x400708;
constexpr uint32_t PGRAPH_INCREMENT = 0x40071C;
constexpr uint32_t PGRAPH_FIFO = 0x400720;
constexpr uint32_t PGRAPH_SEMAPHORE = 0x400B10;  // last semaphore release value, shifted left by two

constexpr uint32_t PCRTC_INTR_0 = 0x600100;
constexpr uint32_t PCRTC_INTR_EN_0 = 0x600140;
constexpr uint32_t PCRTC_START = 0x600800;
constexpr uint32_t PCRTC_RASTER = 0x600808;

constexpr uint32_t PRAMDAC_NVPLL_COEFF = 0x680500;
constexpr uint32_t PRAMDAC_MPLL_COEFF = 0x680504;
constexpr uint32_t PRAMDAC_VPLL_COEFF = 0x680508;

constexpr uint32_t PRAMIN = 0x700000;
}  // namespace reg

void Init();
uint32_t Read(uint32_t offset, unsigned size);
void Write(uint32_t offset, uint32_t value, unsigned size);

// physical address of the scanned out frame buffer (PCRTC_START), 0 before D3D set one
uint32_t DisplayAddress();

// the video overlay: shown since a buffer was queued and until it was stopped
bool OverlayActive();
int OverlayBuffer();  // 0 or 1, the buffer last queued

// the gamma ramp D3D loads into the RAMDAC palette (SetGammaRamp): 256 entries of R, G, B; false while it
// is the identity or was never written
bool GammaRamp(uint8_t out[256][3]);
// D3D's BlockOnTime (f_0050AEA0) is about to wait for time 'time' (its record in ebp): kept for hang reports
void TraceBlockOnTime(uint32_t record, uint32_t time);

// physical (GPU) address to host pointer
uint8_t* Physical(uint32_t address);

}  // namespace gpu
