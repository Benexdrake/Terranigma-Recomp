/* Terranigma 16:9 mod (Plugin "terranigma.widescreen"). Doku: docs/TERRANIGMA_WIDESCREEN_HANDOFF.md */
#ifndef TERRANIGMA_WIDESCREEN_H
#define TERRANIGMA_WIDESCREEN_H

#include "snes/ppu.h"

enum { kTerraWsFrameWidth = 398, kTerraWsFrameHeight = 224 };

/* 1, wenn das Feature aktiv ist (nur Darstellung). */
int TerraWsActive(void);
/* Breite des Frames: 398 mit Mod, sonst 256. */
int TerraWsFrameWidth(void);
/* Pro Frame in GameDrawPpuFrame aufrufen, nachdem PpuSetExtraSpace lief. */
void TerraWsApplyPolicy(Ppu *ppu);

/* Ersetzt snes_beam_frame_driver_draw_ppu_frame(): zeichnet das Bild und blendet dabei den
 * dunklen BG3-Rahmen aus (siehe terranigma_widescreen.c). */
void TerraWsDrawFrame(Ppu *ppu);

#endif
