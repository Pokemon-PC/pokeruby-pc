/*
 * gpu_pc.h - software GBA PPU: renders one scanline at a time from the I/O
 * registers, palettes, VRAM and OAM at their native addresses.
 *
 * Rendering line by line (instead of once per frame) lets H-Blank effects of
 * the game - scanline DMA, HBlank callbacks changing scroll/window/blend
 * registers - show exactly as on hardware.
 */
#ifndef PRS_GPU_PC_H
#define PRS_GPU_PC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Latches the affine reference points (as at the start of VBlank). */
void pc_gpu_frame_start(void);
/* Renders line y (0..159) into 240 pixels of 0xAARRGGBB. */
void pc_gpu_render_line(int y, uint32_t *out);
/* Convenience: frame_start + 160 lines into a 240x160 buffer. */
void pc_gpu_render_frame(uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PRS_GPU_PC_H */
