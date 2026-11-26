/*
 * Terranigma 16:9 als Mod (Paket terranigma.widescreen, Feature widescreen).
 *
 * Nur Darstellung: Der PPU rendert 398x224, die Simulation bleibt 256 breit.
 * Das Plugin ist als "presentation plugin" registriert, es aendert weder CPU,
 * WRAM noch Savestate. Die Randpolitik (Option edge_policy) bestimmt, was in
 * den 71 Zusatzspalten pro Seite erscheint. Der Env-Schalter TERRA_WS
 * (natural|mirror|repeat|clamp) ueberschreibt die Option zu Testzwecken.
 */
#include "terranigma_widescreen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mod_runtime.h"
#include "snes/ws_shadow.h"
#include "snes/interp_bridge.h"
#include "cpu_state.h"
#include "common_rtl.h"
#include "beam_frame_driver.h"

enum { kPolicyNatural, kPolicyMirror, kPolicyRepeat, kPolicyClamp };

static int s_active;
static int s_policy = kPolicyNatural;

static int ParsePolicy(const char *v, int fallback)
{
    if (!v || !v[0]) return fallback;
    if (!strcmp(v, "natural")) return kPolicyNatural;
    if (!strcmp(v, "mirror"))  return kPolicyMirror;
    if (!strcmp(v, "repeat"))  return kPolicyRepeat;
    if (!strcmp(v, "clamp"))   return kPolicyClamp;
    return fallback;
}

static void WsReset(void)
{
    s_active = 0;
    WsShadowUseVram(0);                  /* Welt-Shadow aus: Original-Rendering */
    WsShadowUseVram(1);
    s_policy = kPolicyNatural;
}

/* --- Sprite-Culling (P7/P8) ---------------------------------------------------
 * Der OAM-Emitter bei $00:EC1C..F0xx lehnt Teile per `CMP #$0110 / BCS` (unsigned) ab:
 * A = Bildschirm-X + 16. Vor dem BCS Carry loeschen, wenn A im erweiterten Fenster liegt
 * (rechts: 0x110..0x110+M, links: negativ bis -M). Das OAM-X-Feld ist 9 Bit, der PPU
 * dekodiert Raender mit 71 px eindeutig. */
enum { kCullMargin = 96 };
static const uint32_t kCullSites[] = { 0xECBA, 0xED21, 0xEEA3, 0xEF2E, 0xEFBB, 0xF048 };

/* Raum (Kamera fest, [$826] <= [$828]): rechts/links liegt ein anderer Raum -> nichts erweitern. */
#define ROOM_MODE() ((g_ram[0x826] | (g_ram[0x827] << 8)) <= (g_ram[0x828] | (g_ram[0x829] << 8)))

static void CullHook(CpuState *cpu, uint32_t pc24)
{
    (void)pc24;
    if (!s_active || ROOM_MODE()) return;
    if (!cpu->_flag_C) return;                       /* wurde nicht abgelehnt */
    int a = (int16_t)cpu->A;
    if ((a >= 0x110 && a < 0x110 + kCullMargin) || (a < 0 && a >= -kCullMargin)) {
        cpu->_flag_C = 0;
        cpu->P = (uint8_t)(cpu->P & ~CPU_P_C);
    }
}

/* Objekt-Cull ($00:EAE6, Liste ab [$0DFC], X-Test bei $EB34..$EB4C): akzeptiert ein Objekt,
 * wenn sx-links in [0,256) (BCC bei $EB41) oder sx+rechts in [0,256) (BCS bei $EB4C).
 * Beide Fenster um kCullMargin erweitern, indem vor der Verzweigung Carry geloescht wird. */
static void ClearCarry(CpuState *cpu)
{
    cpu->_flag_C = 0;
    cpu->P = (uint8_t)(cpu->P & ~CPU_P_C);
}
static void ObjCullRight(CpuState *cpu, uint32_t pc24)    /* $EB41 BCC: A = sx - links */
{
    (void)pc24;
    if (!s_active || ROOM_MODE() || !cpu->_flag_C) return;   /* C=0: schon akzeptiert */
    int a = (int16_t)cpu->A;
    if (a >= 0x100 && a < 0x100 + kCullMargin) ClearCarry(cpu);
}
static void ObjCullLeft(CpuState *cpu, uint32_t pc24)     /* $EB4C BCS reject: A = sx + rechts */
{
    (void)pc24;
    if (!s_active || ROOM_MODE() || !cpu->_flag_C) return;
    int a = (int16_t)cpu->A;
    if (a < 0 && a >= -kCullMargin) ClearCarry(cpu);
}

static void WsActivate(void)
{
    char buf[32] = {0};
    s_active = 1;
    static int hooks_set;
    if (!hooks_set) {
        hooks_set = 1;
        for (unsigned i = 0; i < sizeof kCullSites / sizeof *kCullSites; i++) {
            interp_bridge_add_pre_opcode_hook(0x000000u | kCullSites[i], CullHook);
            interp_bridge_add_pre_opcode_hook(0xC00000u | kCullSites[i], CullHook);
        }
        interp_bridge_add_pre_opcode_hook(0x00EB41u, ObjCullRight);
        interp_bridge_add_pre_opcode_hook(0xC0EB41u, ObjCullRight);
        interp_bridge_add_pre_opcode_hook(0x00EB4Cu, ObjCullLeft);
        interp_bridge_add_pre_opcode_hook(0xC0EB4Cu, ObjCullLeft);
    }
    if (snes_mod_runtime_feature_option_value_c("terranigma.widescreen", "widescreen",
                                                "edge_policy", buf, sizeof buf))
        s_policy = ParsePolicy(buf, kPolicyNatural);
    s_policy = ParsePolicy(getenv("TERRA_WS"), s_policy);
}

int TerraWsActive(void) { return s_active; }

int TerraWsFrameWidth(void)
{
    return s_active ? kTerraWsFrameWidth : 256;
}

/* --- Spielwelt (BG2): Kachel aus der Weltkarte des Spiels berechnen ---------
 * Das Spiel streamt nur eine Spalte an der nativen Kante. Die Karte liegt im WRAM:
 * Layer-Struktur bei D=$0800 (Routine $8D:8000/$8D:81CA), Kartenarray (16 Bit,
 * 9-Bit-Metatile-ID) bei 7E:[$832], Maske [$862], Breite [$826]>>4 Metatiles,
 * Metatile-Definitionen (8 Byte: tl,tr,bl,br) bei 7E:[$83A]. 16x16 -> 4x 8x8. */
#include "common_rtl.h"

#define LW(a) ((unsigned)(g_ram[(a)] | (g_ram[(a) + 1] << 8)))
enum { kLayerD = 0x800, kWorldBias = 512 };

static int s_map_ok;
static unsigned s_cam_x;         /* Welt-Kamera X (ohne Versatz) dieses Frames */
static int s_blank_entry = -1;   /* Kachel ohne Pixel in BG2-Zeichensatz, -1 = keine */
static int s_edge_uniform[2];    /* 1 = Randspalte links/rechts ist in den sichtbaren Zeilen einheitlich */

/* Sucht im BG2-Zeichensatz (4bpp, 16 Words/Kachel) eine komplett leere Kachel. */
static int FindBlankTile(const Ppu *ppu)
{
    const uint16_t *vram = PpuRenderVram(ppu);
    unsigned base = (unsigned)PPU_bgTileAdr(ppu, 1);   /* BG2 */
    for (unsigned t = 0; t < 1024; t++) {
        unsigned a = base + t * 16;
        if (a + 16 > 0x8000) break;
        int zero = 1;
        for (int i = 0; i < 16; i++) if (vram[a + i]) { zero = 0; break; }
        if (zero) return (int)t;
    }
    return -1;
}

static int WorldTile(uint32_t tx, uint32_t ty, uint16_t *out)
{
    unsigned W = LW(kLayerD + 0x26) >> 4;
    unsigned mp = LW(kLayerD + 0x32), mask = LW(kLayerD + 0x62), td = LW(kLayerD + 0x3A);
    if (!W) return 0;
    unsigned idx = (((ty >> 1) * W) + (tx >> 1)) & mask;
    unsigned a = (mp + idx * 2) & 0x1FFFF;
    unsigned mid = LW(a) & 0x1FF;
    unsigned t = (td + mid * 8 + ((tx & 1) ? 2 : 0) + ((ty & 1) ? 4 : 0)) & 0x1FFFF;
    *out = (uint16_t)LW(t);
    return 1;
}

/* Sichtbarer Bereich der Karte in 8x8-Kacheln: Kamera darf von 0 bis [$826]-[$828], das Bild
 * ist 256 breit. Raeume liegen als Nebeneinander in einer Karte; ausserhalb ist ein anderer Raum. */
static int s_lo, s_hi;   /* erlaubter Kachelbereich [lo,hi) dieses Frames */

static void ComputeVisible(unsigned camx)
{
    int mapw = (int)(LW(kLayerD + 0x26) >> 4) * 2;
    int w26 = (int)LW(kLayerD + 0x26), w28 = (int)LW(kLayerD + 0x28);
    if (w26 <= w28) {                       /* Raum: Kamera fest, Raum = ein Bild breit */
        s_lo = (int)(camx >> 3);
        s_hi = s_lo + 32;
    } else {                                /* scrollende Karte */
        s_lo = 0;
        s_hi = (w26 - w28 + 256) >> 3;
    }
    if (s_hi > mapw) s_hi = mapw;
    if (s_lo < 0) s_lo = 0;
    if (s_hi < s_lo + 32) s_hi = s_lo + 32;
}

static WsShadowSource MapProvider(void *ctx, uint32_t tx, uint32_t ty, uint16_t *entry)
{
    (void)ctx;
    /* Welt-X traegt den Versatz kWorldBias (der Shadow ueberspringt negative Koordinaten). */
    int32_t x = (int32_t)tx - (kWorldBias >> 3);
    if (!s_map_ok) return kWsShadowMiss;
    /* Ausserhalb des erlaubten Bereichs [lo,hi) (Kartenrand bzw. Nachbarraum): leere Kachel ->
     * Hintergrundfarbe (CGRAM 0) scheint durch. */
    if (x < s_lo || x >= s_hi) {
        int side = x < s_lo ? 0 : 1;
        /* Einheitliche Randspalte (Raum: dunkle Kachel) -> wiederholen; sonst leere Kachel. */
        if (s_edge_uniform[side] || s_blank_entry < 0) { x = side ? s_hi - 1 : s_lo; }
        else { *entry = (uint16_t)s_blank_entry; return kWsShadowExact; }
    }
    if (!WorldTile((uint32_t)x, ty, entry)) return kWsShadowMiss;
    /* Aeusserste native Spalten (die das Spiel unter dem 8-px-Rand von BG3 versteckt): der
     * VRAM ist dort waehrend des Nachladens veraltet -> Kachel aus der Weltkarte erzwingen. */
    if (x <= (int32_t)((s_cam_x + 7) >> 3) || x >= (int32_t)((s_cam_x + 248) >> 3))
        return kWsShadowOverride;
    return kWsShadowExact;
}

/* --- BG3: dunkler 8-px-Rahmen (Spalte 0 und 31) -----------------------------------
 * Das Spiel verdeckt damit die nachladende Randspalte von BG2. Im Breitbild stuende der
 * Rahmen als Streifen auf der alten 4:3-Kante (und per Wrap im Rand). Der Welt-Shadow
 * kennt nur BG1/BG2 und der Viewport-Inset nur 4bpp, daher: waehrend des Zeichnens die
 * Tilemap-Spalten 0/31 von BG3 im VRAM auf 0 setzen und danach wiederherstellen
 * (TerraWsBeforeDraw/TerraWsAfterDraw). Textboxen (Inhalt in den Spalten 1..30) bleiben. */
static uint16_t s_bg3_saved[2][32];
static int s_bg3_zeroed;

static uint16_t s_bg3_saved_row[32];

static void Bg3ZeroBorder(Ppu *ppu, int first)
{
    if (first) s_bg3_zeroed = 0;
    if (!s_active || s_policy != kPolicyNatural || ppu->renderVram) return;
    /* Nur in Weltszenen (BG2-Weltkarte passt zum VRAM): dort ist BG3 nur der Rahmen und die Textbox
     * in den Spalten 1..30. Die Randspalten 0/31 sind immer leer zu zeichnen. Eine Mustererkennung
     * (Rahmenkachel, Mehrheit) reicht nicht: beim Oeffnen der Box fuellt das Spiel einzelne
     * Zeilen der Randspalten mit anderen Kacheln, und der gezeichnete Field-Snapshot weicht
     * vom Live-VRAM ab. Menues (Karte passt nicht) bleiben unveraendert. */
    if (!s_map_ok || (ppu->bgXsc[2] & 3) != 0) return;
    unsigned base = ((unsigned)(ppu->bgXsc[2] & 0xFC)) << 8;
    for (unsigned r = 0; r < 32; r++) {
        unsigned a0 = (base + r * 32) & 0x7fff, a1 = (base + r * 32 + 31) & 0x7fff;
        if (first) {
            s_bg3_saved[0][r] = ppu->vram[a0];
            s_bg3_saved[1][r] = ppu->vram[a1];
        }
        ppu->vram[a0] = 0;
        ppu->vram[a1] = 0;
    }
    /* Rahmenzeile 28: ragt durch den BG3-Scroll-Versatz in die letzte Bildzeile (schmaler Balken unten). */
    for (unsigned c = 0; c < 32; c++) {
        unsigned a = (base + 28 * 32 + c) & 0x7fff;
        if (first) s_bg3_saved_row[c] = ppu->vram[a];
        ppu->vram[a] = 0;
    }
    s_bg3_zeroed = 1;
}

static void Bg3RestoreBorder(Ppu *ppu)
{
    if (!s_bg3_zeroed) return;
    unsigned base = ((unsigned)(ppu->bgXsc[2] & 0xFC)) << 8;
    for (unsigned r = 0; r < 32; r++) {
        ppu->vram[(base + r * 32) & 0x7fff] = s_bg3_saved[0][r];
        ppu->vram[(base + r * 32 + 31) & 0x7fff] = s_bg3_saved[1][r];
    }
    for (unsigned c = 0; c < 32; c++) ppu->vram[(base + 28 * 32 + c) & 0x7fff] = s_bg3_saved_row[c];
    s_bg3_zeroed = 0;
}

/* Wie snes_beam_frame_driver_draw_ppu_frame(), aber mit dem BG3-Eingriff NACH
 * ppu_rasterRenderBegin (das laedt das aufgezeichnete VRAM des Fields und wuerde ihn
 * sonst ueberschreiben). Ohne aktiven Mod: unveraenderter Framework-Aufruf. */
void TerraWsDrawFrame(Ppu *ppu)
{
    if (!s_active || s_policy != kPolicyNatural || !ppu_rasterReplayReady()) {
        snes_beam_frame_driver_draw_ppu_frame();
        return;
    }
    ppu_rasterRenderBegin(ppu);
    Bg3ZeroBorder(ppu, 1);
    for (int line = 1; line <= 224; line++) {
        ppu_rasterApplyLine(ppu, line - 1);
        /* ppu_rasterApplyLine spielt die VRAM-Schreibzugriffe des Fields zeilenweise nach (z. B. die
         * Zeilen einer aufgehenden Textbox) und wuerde die Nullen in den Randspalten ueberschreiben. */
        if (s_bg3_zeroed) Bg3ZeroBorder(ppu, 0);
        ppu_renderRasterLine(ppu, line);
    }
    Bg3RestoreBorder(ppu);
    ppu_rasterRenderEnd(ppu);
}

/* Selbsttest: stimmt die Formel mit dem echten VRAM der 32x28 nativen Kacheln? */
static int MapMatchesVram(const Ppu *ppu, unsigned camx, unsigned camy)
{
    const uint16_t *vram = PpuRenderVram(ppu);
    unsigned base = ((unsigned)(ppu->bgXsc[1] & 0xFC)) << 8;
    int bad = 0;
    for (unsigned r = 0; r < 28; r++)
        for (unsigned c = 0; c < 32; c++) {
            uint32_t tx = (camx >> 3) + c, ty = (camy >> 3) + r;
            uint16_t e;
            if (!WorldTile(tx, ty, &e)) return 0;
            if (e != vram[(base + (ty & 31) * 32 + (tx & 31)) & 0x7fff]) bad++;
        }
    return bad <= 40;
}

void TerraWsApplyPolicy(Ppu *ppu)
{
    if (!s_active) return;
    /* BG2 (Welt): Welt-Shadow. Rand aus der Weltkarte (Provider), sonst aus dem Verlauf. */
    if (s_policy == kPolicyNatural) {
        const int L = 1;
        /* Phase aus dem PPU (P1), hohe Bits aus der WRAM-Kamera. */
        int wx = (int)LW(kLayerD + 0x1E), wy = (int)LW(kLayerD + 0x22);
        int dx = ((int)ppu->hScroll[L] - wx) & 1023, dy = ((int)ppu->vScroll[L] - wy) & 1023;
        if (dx >= 512) dx -= 1024;
        if (dy >= 512) dy -= 1024;
        unsigned camx = (unsigned)(wx + dx), camy = (unsigned)(wy + dy);
        s_map_ok = MapMatchesVram(ppu, camx, camy);
        s_cam_x = camx;
        s_blank_entry = FindBlankTile(ppu);
        {
            ComputeVisible(camx);
            for (int side = 0; side < 2; side++) {
                s_edge_uniform[side] = 0;
                if (!s_map_ok) continue;
                uint32_t ex = side ? (uint32_t)s_hi - 1 : (uint32_t)s_lo, ty0 = camy >> 3;
                uint16_t first, e;
                int same = WorldTile(ex, ty0, &first);
                for (uint32_t r = 1; same && r < 28; r++)
                    if (!WorldTile(ex, ty0 + r, &e) || e != first) same = 0;
                s_edge_uniform[side] = same;
            }
        }
        /* Raum: BG1/BG3 haben keine Weltkarte im Shadow, ihr Wrap darf nicht in den Rand laufen. */
        if (ROOM_MODE()) PpuSetWidescreenLayerClamp(ppu, 0x05);
        if (s_map_ok) {
            /* Provider wird nur ohne retainHistory gefragt (ws_shadow.c). */
            WsShadowSetWorld(L, camx + kWorldBias, camy);
            WsShadowSetScroll(L, ppu->hScroll[L], ppu->vScroll[L]);
            WsShadowSetRetainHistory(L, false);
            WsShadowSetWorldProvider(L, MapProvider, NULL, true);
            WsShadowFrame(ppu);
        } else {
            /* Statische Szene (Raum, Menue, Titel): kein Streaming, die Karte passt nicht zum
             * VRAM. Der Rand wiederholt die Randspalte (clamp) statt den VRAM zu wrappen. */
            WsShadowUseVram(L);
            PpuSetWidescreenLayerClamp(ppu, 0x07);
        }
    }
    switch (s_policy) {
    case kPolicyMirror: PpuSetWidescreenLayerMirror(ppu, 0x07); break;
    case kPolicyRepeat: PpuSetWidescreenLayerRepeat(ppu, 0x07); break;
    case kPolicyClamp:  PpuSetWidescreenLayerClamp(ppu, 0x07); break;
    default: break;
    }
}

SNES_MOD_CONSTRUCTOR(register_terranigma_widescreen)
{
    (void)snes_mod_register_reset_callback(WsReset);
    (void)snes_mod_register_presentation_plugin("terranigma.widescreen", WsActivate);
}
