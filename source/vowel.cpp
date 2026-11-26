// vowel, a level-8 effect on a card of level 10 (docs/level-8.md, level-10.md, module/build_r6e_card.h): a formant filter that makes any sound say vowels.
// In the unit's own lists of effects (a sound's shift + T3, the master's on the mixer), on fx train's wagons, standing
// on the unit's cwo. Built for open-field 0.12 (base 30). The sound is the portable core, vowel_core.h, unchanged.
//
// The six controls (vowel/PORT-0.12.md):
//   the effect's four values, turned by the unit's knobs (and its LFOs), each 0..32767 = the core's word >> 16:
//     0 vowel (blue), 1 shift (ochre), 2 motion (grey), 3 mix (red)
//   the shift layer, the pack's own: shift + blue turns RES, shift + red LEVEL (the core's words 4 and 5), on this
//   page only; kept as the level-7 member vowel_layer with the synth's and drum's sounds and the master.
// The tempo-locked sweep takes its tempo from the block's `period`; this pack names no address of the firmware.
// Its page (T3) is the pack's own picture vowel-face.svg (analysis/screens/designs/vowel/final/), handed to the base
// with op1f_page_stand_in and dressed from the frame function with the values as heard (level 10's op1f_param_heard,
// level 4's parts, moves and texts); should the picture not be had, the frame paints the values on the base's blank
// page as version 2 did.
//
// Threads: vowel_process on the audio thread; vowel_input on the key thread; vowel_frame, vowel_shown and
// vowel_changed on the display thread. They share single words only. Nothing is divided by a variable with / or %,
// nothing is 64-bit, nothing is float, nothing of a library is called.

#include "build_r6e_card.h"
#include "vowel_core.h"

// Every name here begins with vp_ / VP_ or is exported, so that nothing meets a name of the core's.
typedef unsigned int vp_u32;
enum { VP_ASK_EFFECT = 0, VP_BLOCK_LEVEL_8 = 44, VP_FRAME_LEVEL_4 = 40, VP_INPUT_LEVEL_2 = 36, VP_FRAMES = 64 };
enum { VP_PLACES = 17, VP_ALL = (1 << 17) - 1, VP_CENTRE = 16384, VP_TOP = 32767, VP_STEP = 1024, VP_GAP_FRAMES = 3 };
enum { VP_RES = 4, VP_LEVEL = 5, VP_KEPT_VERSION = 1, VP_SAID_MOST = 16 };
// MOTION as the core reads it, for the page's texts
static const int VP_DEAD = VOWEL_DEAD, VP_ZONE = VOWEL_ZONE, VP_AMOUNT_K = VOWEL_AMOUNT_K;

extern "C" const Op1fCard op1f_card_vowel OP1F_NAMED("op1f_card_vowel");
static const char VP_LOG_FILE[] = "op1f/vowel.cpp";       // the log shows "vowel"
static const char VP_KEY[] = "vowel_layer";
static const short VP_DEFAULTS[8] = VOWEL_DEFAULTS;

// ---------------------------------------------------------------- shared single words
static volatile vp_u32 vp_frame;                           // the base's frame number: the frame writes, process reads
static volatile vp_u32 vp_res[VP_PLACES], vp_level[VP_PLACES];   // RES, LEVEL a place (0..16): the frame writes
static volatile int vp_turned[2];                          // shift + blue, shift + red: detents in all, the input's alone
static volatile vp_u32 vp_shown_where, vp_shown_how;       // what `shown` said last
static volatile vp_u32 vp_told;                            // places whose member to read: `changed` sets, the frame takes

// ---------------------------------------------------------------- the sound (audio thread)
// A place's working memory (state_bytes = sizeof): all zero is a fresh one.
struct VpState { VowelState core; vp_u32 frame, period, bpm; };

// The tempo, bpm x 256, from `period` (a 96th of a beat in frames x 2^18 at 44,100): 27562.5 x 2^26 / period, rounded
// to nearest, by 32 compare-subtract steps, as udiv in module/build_r6c_chain.cpp: no divide instruction, no helper.
// The period is held to 20..400 bpm (the core's range); 0 (none) stays 0, which the core reads as 120.
static const vp_u32 VP_N_HI = 430u, VP_N_LO = 2852126720u;         // 27562.5 x 2^26 = 430 x 2^32 + VP_N_LO
static const vp_u32 VP_PERIOD_400 = 18063360u, VP_PERIOD_20 = 361267200u;
static vp_u32 vp_bpm_q8_of(vp_u32 period) {
    if (period == 0) return 0;
    if (period < VP_PERIOD_400) period = VP_PERIOD_400;
    if (period > VP_PERIOD_20) period = VP_PERIOD_20;
    vp_u32 q = 0, r = VP_N_HI;                             // the high word is under any period: its quotient is 0
    for (int i = 31; i >= 0; --i) {
        r = (r << 1) | ((VP_N_LO >> i) & 1u);              // r < 2 period < 2^30
        const vp_u32 take = r >= period ? 1u : 0u;
        r -= period & (0u - take);
        q = (q << 1) | take;
    }
    return q + ((r << 1) >= period ? 1u : 0u);
}

static inline vp_u32 vp_held(int v) { return (vp_u32)(v < 0 ? 0 : v > VP_TOP ? VP_TOP : v); }

extern "C" void vowel_process(const Op1fEffectBlock* now) OP1F_NAMED("vowel_process");
extern "C" void vowel_process(const Op1fEffectBlock* now) {
    if (now->bytes < (vp_u32)VP_BLOCK_LEVEL_8 || now->state == 0 || now->frames != (vp_u32)VP_FRAMES) return;   // the sound as it came
    VpState* st = (VpState*)now->state;
    if (now->fresh) {                                      // made anew: a fresh instance, as the train's was
        vp_u32* w = (vp_u32*)st;
        for (vp_u32 i = 0; i < (vp_u32)(sizeof(VpState) >> 2); ++i) w[i] = 0;
    }
    // No block for 3 or more frames (the effect switched off and on, or a host that skips silent blocks): forget the
    // sound that was cut off, as the wagon did. A fresh instance, or one before the first frame, has no gap.
    const vp_u32 f = vp_frame;
    if (st->frame != 0 && f - st->frame >= (vp_u32)VP_GAP_FRAMES) vowel_gap(&st->core);
    st->frame = f;
    if (now->period != st->period) { st->period = now->period; st->bpm = vp_bpm_q8_of(now->period); }
    const vp_u32 place = now->place;
    const int* v = now->values;
    unsigned words[8];
    for (int k = 0; k < 4; ++k) words[k] = vp_held(v[k]) << 16;
    words[VP_RES] = (place < (vp_u32)VP_PLACES ? vp_res[place] : (vp_u32)VP_DEFAULTS[VP_RES]) << 16;   // a pack's own place: the middle
    words[VP_LEVEL] = (place < (vp_u32)VP_PLACES ? vp_level[place] : (vp_u32)VP_DEFAULTS[VP_LEVEL]) << 16;
    words[6] = words[7] = 0;
    vowel_block(&st->core, now->left, now->right, words, st->bpm);
}

// ---------------------------------------------------------------- the knobs (key thread)
// On this page, shift + blue and shift + red are the pack's: 1024 a detent, held to +-64 detents, towards zero. Every
// other turn and key goes to the unit, which turns the four values.
extern "C" int vowel_input(const Op1fInput* e) OP1F_NAMED("vowel_input");
extern "C" int vowel_input(const Op1fInput* e) {
    if (e->bytes < (vp_u32)VP_INPUT_LEVEL_2 || !e->mine || e->what != (vp_u32)OP1F_TURN || !e->shift) return 0;
    if (e->key != 0u && e->key != 3u) return 0;
    const int t = e->turn < -65536 ? -65536 : e->turn > 65536 ? 65536 : e->turn;
    const int by = t >= 0 ? t >> 10 : -((-t) >> 10);
    const int k = e->key == 0u ? 0 : 1;
    vp_turned[k] = vp_turned[k] + by;
    return 1;
}

// ---------------------------------------------------------------- told (display thread, outside the frame)
extern "C" void vowel_shown(unsigned where, unsigned how) OP1F_NAMED("vowel_shown");
extern "C" void vowel_shown(unsigned where, unsigned how) {
    vp_shown_where = where;
    vp_shown_how = how;
}

extern "C" void vowel_changed(unsigned where, unsigned which, unsigned how) OP1F_NAMED("vowel_changed");
extern "C" void vowel_changed(unsigned where, unsigned which, unsigned how) {
    if (which > 7u || (how != (unsigned)OP1F_KEPT_LOADED && how != (unsigned)OP1F_KEPT_START && how != (unsigned)OP1F_KEPT_DROPPED)) return;
    const vp_u32 place = where == (unsigned)OP1F_KEEP_SYNTH ? which : where == (unsigned)OP1F_KEEP_DRUM ? 8u + which :
                         where == (unsigned)OP1F_KEEP_MASTER ? 16u : 32u;
    if (place < (vp_u32)VP_PLACES) vp_told = vp_told | (1u << place);
}

// ---------------------------------------------------------------- texts
static char* vp_put(char* p, const char* s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char* vp_num(char* p, vp_u32 v) {                   // the digits by subtraction: nothing is divided
    static const vp_u32 TEN[] = { 10000u, 1000u, 100u, 10u, 1u };
    int started = 0;
    if (v > 99999u) v = 99999u;
    for (int i = 0; i < 5; ++i) { char c = '0'; while (v >= TEN[i]) { v -= TEN[i]; ++c; } if (c != '0' || started || i == 4) { *p++ = c; started = 1; } }
    *p = 0; return p;
}
static vp_u32 vp_place(vp_u32 v) { return (v * 1024u + 16384u) >> 15; }   // VOWEL: 0..1024, 256 a letter

static void vp_text_vowel(char* t, vp_u32 v) {             // the nearest letter, the other neighbour and its share: "o+u 25%"
    static const char LETTER[5] = { 'a', 'e', 'i', 'o', 'u' };
    const vp_u32 at = vp_place(v);
    vp_u32 i = at >> 8, fr = at & 255u;
    if (i >= 4u) { i = 4u; fr = 0; }
    const vp_u32 near = fr < 128u ? i : i + 1u, other = fr < 128u ? i + 1u : i, share = fr < 128u ? fr : 256u - fr;
    const vp_u32 pct = (share * 100u + 128u) >> 8;
    *t++ = LETTER[near]; *t = 0;
    if (pct) { *t++ = '+'; *t++ = LETTER[other]; *t++ = ' '; vp_put(vp_num(t, pct), "%"); }
}
// The distance from the middle in tenths of 12, signed, then the unit: "+3.8 st", "-12.0 dB"
static void vp_text_tenths(char* t, vp_u32 v, const char* unit) {
    const int d = (int)v - VP_CENTRE;
    vp_u32 rest = ((vp_u32)(d < 0 ? -d : d) * 120u + 8192u) >> 14;
    if (rest && d > 0) *t++ = '+'; else if (rest && d < 0) *t++ = '-';
    vp_u32 whole = 0;
    while (rest >= 10u) { rest -= 10u; ++whole; }
    t = vp_num(t, whole); *t++ = '.'; t = vp_num(t, rest); vp_put(t, unit);
}
// MOTION's kind: 0 off (the middle), 1 the envelope (x its percent), 2 the sweep (x its zone 0..4, 1/1..1/16)
static int vp_motion_of(vp_u32 v, vp_u32* x) {
    const int d = (int)v - VP_CENTRE;
    if (d >= -VP_DEAD && d <= VP_DEAD) { *x = 0; return 0; }
    if (d < 0) {
        vp_u32 pct = ((((vp_u32)(-d - VP_DEAD) * (vp_u32)VP_AMOUNT_K) >> 10) * 100u + 16384u) >> 15;
        *x = pct > 100u ? 100u : pct; return 1;
    }
    const int z = d - VP_DEAD;
    *x = z < VP_ZONE ? 0u : z < 2 * VP_ZONE ? 1u : z < 3 * VP_ZONE ? 2u : z < 4 * VP_ZONE ? 3u : 4u; return 2;
}
static void vp_text_motion(char* t, vp_u32 v) {            // "off", "env 63%", or the sweep's note "1/4"
    static const char* const NOTE[5] = { "1/1", "1/2", "1/4", "1/8", "1/16" };
    vp_u32 x = 0;
    const int mode = vp_motion_of(v, &x);
    if (mode == 0) vp_put(t, "off");
    else if (mode == 1) vp_put(vp_num(vp_put(t, "env "), x), "%");
    else vp_put(t, NOTE[x]);
}
static void vp_text_percent(char* t, vp_u32 v) { vp_put(vp_num(t, (v * 100u + 16384u) >> 15), "%"); }
static void vp_text_value(char* t, vp_u32 which, vp_u32 v) {   // 0..3 the four values, 4 RES, 5 LEVEL
    if (which == 0u) vp_text_vowel(t, v);
    else if (which == 1u) vp_text_tenths(t, v, " st");
    else if (which == 2u) vp_text_motion(t, v);
    else if (which == (vp_u32)VP_LEVEL) vp_text_tenths(t, v, " dB");
    else vp_text_percent(t, v);
}

// ---------------------------------------------------------------- the frame (display thread)
static struct {
    vp_u32 started, to_read, to_write, said_page, page_lines, keep_lines;
    int seen[2];
    char text[64];
} vp;

static int vp_keep_of(vp_u32 place, unsigned* where, unsigned* which) {
    if (place < 8u) { *where = OP1F_KEEP_SYNTH; *which = place; return 1; }
    if (place < 16u) { *where = OP1F_KEEP_DRUM; *which = place - 8u; return 1; }
    if (place == 16u) { *where = OP1F_KEEP_MASTER; *which = 0; return 1; }
    return 0;
}

static const char* vp_place_said(vp_u32 place, char* out) {
    if (place >= 16u) return "the master";
    vp_num(vp_put(out, place >= 8u ? "drum " : "synth "), (place & 7u) + 1u);
    return out;
}

static void vp_say_keep(const char* what, vp_u32 place, int answer) {
    if (++vp.keep_lines > (vp_u32)VP_SAID_MOST) return;
    char p[16];
    op1f_log(2, VP_LOG_FILE, 3, "vowel: the shift layer of %s not %s (answer %d): RES and LEVEL go on as they are", vp_place_said(place, p), what, answer);
}

// The whole number after `key` in a member's text, or -1: what this pack wrote, read back.
static int vp_number_after(const char* t, const char* key) {
    for (vp_u32 i = 0; t[i]; ++i) {
        vp_u32 k = 0;
        while (key[k] && t[i + k] == key[k]) ++k;
        if (key[k]) continue;
        vp_u32 at = i + k, n = 0;
        int v = 0;
        while (t[at] >= '0' && t[at] <= '9' && n < 6u) { v = v * 10 + (t[at] - '0'); ++at; ++n; }
        return n == 0u || n == 6u ? -1 : v;
    }
    return -1;
}

static void vp_read(vp_u32 place) {                        // a place's member: its RES and LEVEL, or the middles
    unsigned where, which;
    if (!vp_keep_of(place, &where, &which)) return;
    const int got = op1f_value_get(&op1f_card_vowel, where, which, VP_KEY, vp.text, sizeof vp.text);
    if (got == OP1F_NOT_NOW) return;                       // asked again at the next frame
    vp.to_read &= ~(1u << place);
    vp.to_write &= ~(1u << place);                         // a load wins over a turn not yet written
    int res = VP_DEFAULTS[VP_RES], level = VP_DEFAULTS[VP_LEVEL];
    if (got >= 0) {
        const int r = vp_number_after(vp.text, "\"res\":"), l = vp_number_after(vp.text, "\"level\":");
        if (vp_number_after(vp.text, "\"version\":") == VP_KEPT_VERSION && r >= 0 && r <= VP_TOP && l >= 0 && l <= VP_TOP) { res = r; level = l; }
    } else if (got != OP1F_NO_FILE) {
        vp_say_keep("read", place, got);
    }
    vp_res[place] = (vp_u32)res;
    vp_level[place] = (vp_u32)level;
}

static void vp_write(vp_u32 place) {
    unsigned where, which;
    if (!vp_keep_of(place, &where, &which)) { vp.to_write &= ~(1u << place); return; }
    char* t = vp_put(vp.text, "{\"level\":");
    t = vp_put(vp_num(t, vp_level[place]), ",\"res\":");
    t = vp_put(vp_num(t, vp_res[place]), ",\"version\":");
    vp_put(vp_num(t, VP_KEPT_VERSION), "}");
    const int answer = op1f_value_set(&op1f_card_vowel, where, which, VP_KEY, vp.text);
    if (answer == OP1F_NOT_NOW) return;                    // set at the next frame
    vp.to_write &= ~(1u << place);
    if (answer != 0) vp_say_keep("kept", place, answer);
}

static vp_u32 vp_moved(vp_u32 was, int detents) {
    int v = (int)was + detents * VP_STEP;
    return (vp_u32)(v < 0 ? 0 : v > VP_TOP ? VP_TOP : v);
}

// A line when the page comes onto the screen, goes, or the effect is switched on or off under it: the first sixteen.
static void vp_say_page(vp_u32 how, vp_u32 where) {
    const vp_u32 up = how & (vp_u32)OP1F_SHOWN_UP, off = how & (vp_u32)OP1F_SHOWN_OFF;
    const vp_u32 said = up ? (up | off | ((where & 31u) << 4)) : 0u;
    if (said == vp.said_page) return;
    vp.said_page = said;
    if (++vp.page_lines > (vp_u32)VP_SAID_MOST) return;
    char p[16];
    if (up) op1f_log(3, VP_LOG_FILE, 1, "vowel: its page is on the screen for %s, switched %s; res %u, level %u", vp_place_said(where, p), off ? "off" : "on",
                     where < (vp_u32)VP_PLACES ? vp_res[where] : 0u, where < (vp_u32)VP_PLACES ? vp_level[where] : 0u);
    else op1f_log(3, VP_LOG_FILE, 2, "vowel: its page is no longer on the screen");
}

// The page: the base's blank page with the name; four columns in the knobs' colours (with shift held the first and the
// fourth show RES and LEVEL), the shift layer in a line under them, and "off" while the effect is switched off.
static const unsigned VP_KNOB[4] = { OP1F_COLOUR(0x3c, 0x60, 0x87), OP1F_COLOUR(0x8a, 0x65, 0x3c),
                                     OP1F_COLOUR(0xa2, 0xa4, 0xa6), OP1F_COLOUR(0xe5, 0x28, 0x17) };
static const unsigned VP_LIGHT_GREY = OP1F_COLOUR(0xcd, 0xd0, 0xc2), VP_GREY = OP1F_COLOUR(0xa2, 0xa4, 0xa6);
enum { VP_COLUMN_FIRST = 60, VP_COLUMN_PITCH = 120, VP_VALUE_Y = 86, VP_NAME_Y = 122, VP_LAYER_Y = 156, VP_OFF_Y = 180, VP_MIDDLE = 240 };

static void vp_centred(void* canvas, int middle, int y, const char* text, unsigned colour, int scale) {
    int n = 0;
    while (text[n]) ++n;
    op1f_paint_text(canvas, middle - (((6 * n - 1) * scale) >> 1), y, text, colour, scale);
}

static void vp_paint(void* canvas, vp_u32 where, vp_u32 how, int shift) {
    static const char* const NAME[6] = { "vowel", "shift", "motion", "mix", "res", "level" };
    const vp_u32 res = vp_res[where], level = vp_level[where];
    char t[24];
    for (vp_u32 i = 0; i < 4u; ++i) {
        const vp_u32 which = shift && i == 0u ? (vp_u32)VP_RES : shift && i == 3u ? (vp_u32)VP_LEVEL : i;
        int value = 0;
        if (which >= 4u) vp_text_value(t, which, which == (vp_u32)VP_RES ? res : level);
        else if (op1f_param_get(&op1f_card_vowel, VP_ASK_EFFECT, where, i, &value) == 0) vp_text_value(t, which, vp_held(value));
        else vp_put(t, "-");
        const int middle = VP_COLUMN_FIRST + (int)i * VP_COLUMN_PITCH;
        vp_centred(canvas, middle, VP_VALUE_Y, t, VP_KNOB[i], 2);
        vp_centred(canvas, middle, VP_NAME_Y, NAME[which], VP_LIGHT_GREY, 2);
    }
    char line[48];
    vp_text_percent(t, res);
    char* p = vp_put(vp_put(line, "shift: res "), t);
    vp_text_value(t, VP_LEVEL, level);
    vp_put(vp_put(p, "  level "), t);
    vp_centred(canvas, VP_MIDDLE, VP_LAYER_Y, line, VP_GREY, 2);
    if (how & (vp_u32)OP1F_SHOWN_OFF) vp_centred(canvas, VP_MIDDLE, VP_OFF_Y, "off", VP_GREY, 2);
}

// ---------------------------------------------------------------- the page (display thread)
// analysis/screens/designs/vowel/final/DESIGN.md: a head in profile whose lips, jaw and tongue take VOWEL's letter,
// scaled about the cheek by SHIFT (higher, a shorter tract: a smaller head) with the larynx sliding along the neck; a
// vowel chart with the tongue's point; at the throat the breath (envelope) or a wave (sweep) for MOTION; sound lines
// and a meter for MIX; the values around it. Values as heard (level 10: an LFO's move shows), RES and LEVEL the pack's
// own. The picture is asked for once; its parts are found once and the start-hidden ones hidden before the page is
// shown. A show, a move or a text is sent only when it changed. Without the picture, or with a part of it missing,
// the values are painted on the blank page (vp_paint).
enum { VP_LETTERS = 5, VP_STAND_TRIES = 64, VP_CELLS = 12, VP_TEXTS = 7 };
static const char VP_FACE[] = "vowel-face.svg";
// openness at rest of the five letters, from the core's tenor table (F1, unshifted), as the wagon had it
static const unsigned short VP_REST_OPEN[VP_LETTERS] = { 32767, 13054, 0, 13054, 7633 };
static const unsigned char VP_LIPS[VP_LETTERS] = { 0, 1, 1, 2, 2 };            // a open, e i spread, o u rounded
static const short VP_CREST[VP_LETTERS][2] = { { 167, 122 }, { 161, 112 }, { 156, 107 }, { 175, 114 }, { 179, 107 } };
static const short VP_SPOT[VP_LETTERS][2] = { { 291, 163 }, { 208, 112 }, { 176, 61 }, { 334, 112 }, { 334, 61 } };
static const short VP_SWEEP[5] = { 1024, 819, 655, 524, 419 };                // the wave's width a sweep zone
enum { VP_PIVOT_X = 190, VP_PIVOT_Y = 120, VP_TG_X = 262, VP_TG_Y = 112, VP_JAW_X = 4096, VP_JAW_Y = 11264 };
// the parts, by index: single ones, then the numbered groups
enum { VP_P_HD, VP_P_JAW, VP_P_LX, VP_P_TG, VP_P_SHK, VP_P_BW, VP_P_CHDIM, VP_P_UL, VP_P_LL = VP_P_UL + 3,
       VP_P_TN = VP_P_LL + 3, VP_P_L = VP_P_TN + 5, VP_P_BR = VP_P_L + 5, VP_P_SL = VP_P_BR + 3, VP_P_CELL = VP_P_SL + 4,
       VP_PARTS = VP_P_CELL + VP_CELLS };
// a part or a group: its name, how many, numbered (vw_ul0..2) or not, the first of them hidden at the start
static const struct { const char* name; unsigned char count, numbered, hide_from; } VP_GROUP[] = {
    { "vw_hd", 1, 0, 1 }, { "vw_jaw", 1, 0, 1 }, { "vw_lx", 1, 0, 1 }, { "vw_tg", 1, 0, 1 }, { "vw_shk", 1, 0, 1 },
    { "vw_bw", 1, 0, 0 }, { "vw_chdim", 1, 0, 0 }, { "vw_ul", 3, 1, 1 }, { "vw_ll", 3, 1, 1 }, { "vw_tn", 5, 1, 1 },
    { "vw_l", 5, 1, 0 }, { "vw_br", 3, 1, 0 }, { "vw_sl", 4, 1, 0 }, { "vw_b3s", VP_CELLS, 1, 0 } };
static const char* const VP_TEXT[VP_TEXTS] = { "vw_val0", "vw_val1", "vw_val2", "vw_val3", "vw_val4", "vw_val5", "vw_off" };
static struct {
    void* doc;                                             // the picture standing in, or 0: the blank page, painted
    vp_u32 asked, tries, stood;
    void* part[VP_PARTS];
    int on[VP_PARTS];                                      // as last shown (1) or hidden (0)
    int m[VP_PARTS][4];                                    // a, d, e, f as last moved (b = c = 0); drawn: 1024, 1024, 0, 0
    vp_u32 open_s, stamp;                                  // openness, smoothed; this frame's number for the two below
    int tg_x, tg_y;                                        // the tongue point in 1/256 px, smoothed
    vp_u32 want[VP_PARTS], moved[VP_PARTS];                // = stamp: shown this frame / its matrix set this frame
    int next[VP_PARTS][4];
    int sent[VP_PARTS][9];                                 // the nine numbers a part's move hands the base; b, c and the two 0 stay 0 from the start
    char said[VP_TEXTS][12];                               // the texts last written
    vp_u32 refused[VP_TEXTS];                              // refusals in a row of a text; at 64 logged, then tried every 64th frame
} vf;

static void vp_make_face() {
    void* d = op1f_picture(&op1f_card_vowel, VP_FACE);
    char name[16], lack[16];
    lack[0] = 0;
    if (d) {
        int p = 0;
        for (unsigned g = 0; g < sizeof VP_GROUP / sizeof VP_GROUP[0]; ++g)
            for (int j = 0; j < VP_GROUP[g].count; ++j, ++p) {
                char* q = vp_put(name, VP_GROUP[g].name);
                if (VP_GROUP[g].numbered) vp_num(q, (vp_u32)j);
                vf.part[p] = op1f_node(d, name);
                if (!vf.part[p] && !lack[0]) vp_put(lack, name);
            }
        for (int i = 0; i < VP_TEXTS; ++i) if (!op1f_node(d, VP_TEXT[i]) && !lack[0]) vp_put(lack, VP_TEXT[i]);
        if (!lack[0]) {                                    // everything there: the start-hidden parts hidden now
            p = 0;
            for (unsigned g = 0; g < sizeof VP_GROUP / sizeof VP_GROUP[0]; ++g)
                for (int j = 0; j < VP_GROUP[g].count; ++j, ++p) {
                    vf.m[p][0] = 1024; vf.m[p][1] = 1024; vf.m[p][2] = 0; vf.m[p][3] = 0;
                    vf.on[p] = j < VP_GROUP[g].hide_from;
                    if (!vf.on[p]) op1f_show(vf.part[p], 0);
                }
        }
    }
    for (int i = 0; i < VP_TEXTS; ++i) vf.said[i][0] = 1;  // nothing written yet: the first text, even "", is written
    vf.open_s = 16384u; vf.tg_x = VP_TG_X * 256; vf.tg_y = VP_TG_Y * 256;
    vf.doc = lack[0] ? 0 : d;
    if (d && lack[0]) op1f_log(2, VP_LOG_FILE, 7, "vowel: its picture %s has no part %s: the values are painted on the blank page", VP_FACE, lack);
    else op1f_log(3, VP_LOG_FILE, 5, "vowel: its picture %s %s", VP_FACE, d ? "made" : "NOT made: the values are painted on the blank page");
}

static void vp_say(void* d, int i, const char* text, int width, unsigned align) {
    vp_u32 n = 0;                                          // (a placeholder's stroke shows until its first text)
    while (text[n] && vf.said[i][n] == text[n]) ++n;
    if (text[n] == 0 && vf.said[i][n] == 0) return;        // the same text: not written again
    if (vf.refused[i] >= 64u && (vf.refused[i]++ & 63u) != 0) return;   // a lasting refusal: not every frame
    const int took = align == (unsigned)OP1F_LEFT ? op1f_text(d, VP_TEXT[i], text, width) : op1f_text_set(d, VP_TEXT[i], text, width, align);
    if (took == 0) {                                       // not taken: tried again at the next frame, after 64 refusals
        if (vf.refused[i] < 64u && ++vf.refused[i] == 64u) // in a row every 64th frame, and said once in the log
            op1f_log(2, VP_LOG_FILE, 8, "vowel: the base refused the text %s of its picture 64 times; tried less often now", VP_TEXT[i]);
        return;
    }
    vf.refused[i] = 0;
    n = 0;
    while (text[n] && n + 1u < sizeof vf.said[i]) { vf.said[i][n] = text[n]; ++n; }
    vf.said[i][n] = 0;
}

// This frame's wishes: a part shown; a part's matrix [a, 0, 0, 0, d, 0, e, f, 1024]
static void vp_on(int p) { vf.want[p] = vf.stamp; }
static void vp_mat(int p, int a, int d, int e, int f) {
    vf.next[p][0] = a; vf.next[p][1] = d; vf.next[p][2] = e; vf.next[p][3] = f;
    vf.moved[p] = vf.stamp;
}
// S(tx, ty): scaled by s about the pivot behind the cheek after a move of tx, ty (1024ths of a pixel); shown
static void vp_S(int p, int s, int tx, int ty) {
    vp_mat(p, s, s, ((s * tx) >> 10) + VP_PIVOT_X * (1024 - s), ((s * ty) >> 10) + VP_PIVOT_Y * (1024 - s));
    vp_on(p);
}
// The wishes sent: the moves of the parts to be shown, then the shows, then the hides (a new shape is in place and
// shown before the old one goes)
static void vp_send() {
    for (int p = 0; p < VP_PARTS; ++p) {
        if (vf.want[p] != vf.stamp || vf.moved[p] != vf.stamp) continue;
        int* was = vf.m[p];
        const int* to = vf.next[p];
        if (was[0] == to[0] && was[1] == to[1] && was[2] == to[2] && was[3] == to[3]) continue;
        int* m = vf.sent[p];                                // [a, 0, 0, 0, d, 0, e, f, 1024], kept with the part and not on the stack
        m[0] = to[0]; m[4] = to[1]; m[6] = to[2]; m[7] = to[3]; m[8] = 1024;
        op1f_move(vf.part[p], m);
        was[0] = to[0]; was[1] = to[1]; was[2] = to[2]; was[3] = to[3];
    }
    for (int p = 0; p < VP_PARTS; ++p) if (vf.want[p] == vf.stamp && !vf.on[p]) { op1f_show(vf.part[p], 1); vf.on[p] = 1; }
    for (int p = 0; p < VP_PARTS; ++p) if (vf.want[p] != vf.stamp && vf.on[p]) { op1f_show(vf.part[p], 0); vf.on[p] = 0; }
}

static int vp_half(int d) { return (d >= 0 ? d + 1 : d - 1) / 2; }   // half, away from 0: 1 -> 1, 3 -> 2
// The head, the chart's tongue point, the throat and the sound lines (not while the effect is switched off)
static void vp_head(const vp_u32* v, const int* have, int s) {
    const vp_u32 at = vp_place(v[0]);
    vp_u32 i = at >> 8, fr = at & 255u;
    if (i >= 4u) { i = 3u; fr = 256u; }
    const vp_u32 open = have[0] ? (VP_REST_OPEN[i] * (256u - fr) + VP_REST_OPEN[i + 1] * fr) >> 8 : 16384u;
    vf.open_s = (vf.open_s + open + 1u) >> 1;
    const int jx = (int)((vf.open_s * (vp_u32)VP_JAW_X) >> 15), jy = (int)((vf.open_s * (vp_u32)VP_JAW_Y) >> 15);
    const int letter = have[0] ? (int)((at + 128u) >> 8) : 0, k = VP_LIPS[letter];
    vp_S(VP_P_HD, s, 0, 0);
    vp_S(VP_P_UL + k, s, 0, 0);
    vp_S(VP_P_JAW, s, jx, jy);
    vp_S(VP_P_LL + k, s, jx, jy);
    int dx = 0, dy = 0;                                    // the tongue: the letter's shape slid toward the neighbour's
    const int dt = have[0] ? (int)at - letter * 256 : 0;
    if (dt) {
        const int nb = letter + (dt > 0 ? 1 : -1), share = dt < 0 ? -dt : dt;
        dx = (VP_CREST[nb][0] - VP_CREST[letter][0]) * share * 4;
        dy = (VP_CREST[nb][1] - VP_CREST[letter][1]) * share * 4;
    }
    vp_S(VP_P_TN + letter, s, jx + dx, jy + dy);
    const int sh = have[1] ? (int)v[1] - VP_CENTRE : 0;    // the larynx: up the neck with SHIFT, 12 px at the ends
    vp_S(VP_P_LX, s, jx - (sh * 4200) / 16384, jy - (sh * 11556) / 16384);
    int tx = VP_TG_X * 256, ty = VP_TG_Y * 256;            // the chart's tongue point, in 1/256 px
    if (have[0]) {
        tx = VP_SPOT[i][0] * (int)(256u - fr) + VP_SPOT[i + 1][0] * (int)fr;
        ty = VP_SPOT[i][1] * (int)(256u - fr) + VP_SPOT[i + 1][1] * (int)fr;
        vp_on(VP_P_L + letter);
    }
    vf.tg_x += vp_half(tx - vf.tg_x); vf.tg_y += vp_half(ty - vf.tg_y);   // half the way a frame, landing on it
    vp_mat(VP_P_TG, 1024, 1024, (vf.tg_x - VP_TG_X * 256) * 4, (vf.tg_y - VP_TG_Y * 256) * 4);
    vp_on(VP_P_TG);
    if (have[2]) {                                         // the throat: breath puffs or the sweep's wave
        vp_u32 x = 0;
        const int mode = vp_motion_of(v[2], &x);
        if (mode == 1) {
            const int n = x == 0u ? 0 : x <= 33u ? 1 : x <= 66u ? 2 : 3;
            for (int j = 0; j < n; ++j) vp_S(VP_P_BR + j, s, 0, 0);
        } else if (mode == 2) {
            const int sx = VP_SWEEP[x];
            vp_mat(VP_P_BW, (s * sx) >> 10, s, ((s * VP_PIVOT_X * (1024 - sx)) >> 10) + VP_PIVOT_X * (1024 - s), VP_PIVOT_Y * (1024 - s));
            vp_on(VP_P_BW);
        }
    }
    if (have[3]) {                                         // the sound lines leaving the mouth
        const int n = (int)((v[3] * 4u + 16384u) >> 15);
        for (int j = 0; j < n; ++j) vp_S(VP_P_SL + j, s, 0, 0);
    }
}

// The picture dressed for the place whose page is up
static void vp_dress(void* d, vp_u32 where, vp_u32 how) {
    const int off = (how & (vp_u32)OP1F_SHOWN_OFF) != 0;
    vp_u32 v[4] = { (vp_u32)VP_CENTRE, (vp_u32)VP_CENTRE, (vp_u32)VP_CENTRE, (vp_u32)VP_CENTRE };
    int have[4] = { 0, 0, 0, 0 };
    for (vp_u32 i = 0; i < 4u; ++i) { int value = 0; if (op1f_param_heard(&op1f_card_vowel, VP_ASK_EFFECT, where, i, &value) == 0) { v[i] = vp_held(value); have[i] = 1; } }
    char t[24];
    if (have[0]) vp_text_vowel(t, v[0]); else vp_put(t, "-");
    vp_say(d, 0, t, 112, OP1F_LEFT);
    if (have[1]) vp_text_tenths(t, v[1], " st"); else vp_put(t, "-");
    vp_say(d, 1, t, 100, OP1F_LEFT);
    if (have[2]) vp_text_motion(t, v[2]); else vp_put(t, "-");
    vp_say(d, 2, t, 100, OP1F_LEFT);
    if (have[3]) vp_text_percent(t, v[3]); else vp_put(t, "-");
    vp_say(d, 3, t, 60, OP1F_RIGHT);
    vp_text_percent(t, vp_res[where]);
    vp_say(d, 4, t, 60, OP1F_LEFT);
    vp_text_tenths(t, vp_level[where], " dB");
    vp_say(d, 5, t, 100, OP1F_RIGHT);
    vp_say(d, 6, off ? "off" : "", 60, OP1F_LEFT);
    ++vf.stamp;
    const int sh = have[1] ? (int)v[1] - VP_CENTRE : 0;
    const int s = 1024 - ((sh * 102 + 8192) >> 14);       // 922..1126: +-10 %, rounded
    vp_mat(VP_P_SHK, 1024, 1024, sh * 4, 0);              // the slider's handle, +-64 px
    vp_on(VP_P_SHK);
    const int cells = have[3] ? (int)((v[3] * (vp_u32)VP_CELLS + 16384u) >> 15) : 0;
    for (int j = 0; j < cells; ++j) vp_on(VP_P_CELL + j);
    if (off) vp_on(VP_P_CHDIM);
    else vp_head(v, have, s);
    vp_send();
}


extern "C" void vowel_frame(const Op1fFrame* now) OP1F_NAMED("vowel_frame");
extern "C" void vowel_frame(const Op1fFrame* now) {
    if (now->bytes < (vp_u32)VP_FRAME_LEVEL_4) return;
    vp_frame = now->frame;
    if (!vf.asked) { vf.asked = 1; vp_make_face(); }
    if (vf.doc && !vf.stood) {                             // the picture stands in for the effect's page from the next frame
        if (op1f_page_stand_in(&op1f_card_vowel, VP_ASK_EFFECT, vf.doc)) vf.stood = 1;
        else if (++vf.tries >= (vp_u32)VP_STAND_TRIES) {
            op1f_log(2, VP_LOG_FILE, 6, "vowel: the base did not take its picture as its page: the values are painted on the blank page");
            vf.doc = 0;
        }
    }
    if (!vp.started) {
        vp.started = 1;
        for (vp_u32 p = 0; p < (vp_u32)VP_PLACES; ++p) { vp_res[p] = (vp_u32)VP_DEFAULTS[VP_RES]; vp_level[p] = (vp_u32)VP_DEFAULTS[VP_LEVEL]; }
        vp.to_read = (vp_u32)VP_ALL;
        op1f_log(3, VP_LOG_FILE, 4, "vowel: ready; RES and LEVEL kept as '%s' with the sounds and the master", VP_KEY);
    }
    vp.to_read |= vp_told;
    vp_told = 0;
    for (vp_u32 p = 0; p < (vp_u32)VP_PLACES; ++p) if (vp.to_read & (1u << p)) vp_read(p);
    // the shift layer's turns, for the place whose page is up
    const vp_u32 how = vp_shown_how, where = vp_shown_where;
    const int t0 = vp_turned[0], t1 = vp_turned[1];
    const int d0 = t0 - vp.seen[0], d1 = t1 - vp.seen[1];
    vp.seen[0] = t0; vp.seen[1] = t1;
    const int up = (how & (vp_u32)OP1F_SHOWN_UP) && where < (vp_u32)VP_PLACES;
    if (up && (d0 || d1)) {
        vp_res[where] = vp_moved(vp_res[where], d0);
        vp_level[where] = vp_moved(vp_level[where], d1);
        vp.to_write |= 1u << where;
    }
    for (vp_u32 p = 0; p < (vp_u32)VP_PLACES; ++p) if (vp.to_write & (1u << p)) vp_write(p);
    vp_say_page(how, where);
    if (!up || now->page != &op1f_card_vowel) return;
    if (vf.doc && vf.stood) vp_dress(vf.doc, where, how);   // the base shows its own words over its blank page when the place is dry
    else if (now->canvas != 0) vp_paint(now->canvas, where, how, now->shift != 0);
}
