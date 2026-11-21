/* SPDX-License-Identifier: GPL-3.0-only */
/* SCENEDRIFT for the M-VAVE FM-1, ported from the Scenedrift HTML toy.
 *
 *   4 lanes       LEAD, BASS, PLUCK, AIR: one saw+FM engine, each lane its own length (polyrhythm)
 *   16 steps      the white keys. Tap: on/off. Hold a step: KNOB1-4 lock PITCH / LEVEL / SEND / HOLD,
 *                 SELECT sets its trig condition, EDIT turns its locks on/off, D#5 clears them
 *   XY morph      ALGORITHM = X, PRESETS = Y: blends the four scenes across 20 sound settings
 *
 *   black keys    F#3 G#3 A#3 C#4  lane select (hold one + KNOB1: its length)
 *                 D#4 RANDOM   F#4 G#4 A#4 C#5  scenes A-D (tap: jump there; hold SAVE + tap: store)
 *                 D#5 CLEAR the lane     F#5 hold 1 s: INIT
 *   pages         EDIT osc  SEL character  ENV  LFO  FX delay  GLO global  ARP system. SELECT cycles them
 *   OCT- / OCT+   everything an octave down / up      PLAY start / stop      SAVE (tap, stopped) store in flash
 *
 * Changes from the HTML version, for the chip: two notes per lane (the newest replaces the older), a CPU meter
 * that thins the unison (7 -> 5 -> 3 saws) when the chip runs short, mono delays panned apart, Master on the
 * hardware MASTER knob. Integers only: the chip has no FPU. */
#include "fm1.h"

#define STEPS 16
#define LANES 4
#define SLOTS 2 /* notes per lane */
#define NOTES (LANES * SLOTS)
#define MAXUNI 7
#define EVQ 32
#define SUB 16
#define DLEN 32768u /* delay buffers: 743 ms each */
#define DMASK (DLEN - 1)

/* ================================================================ settings */

enum { /* the 20 the XY pad blends, then the rest */
  K_DETUNE, K_DMIX, K_SUB, K_LEVEL, K_WIDTH, K_ATK, K_HOLD, K_REL, K_DRIVE, K_MODAMT, K_MODRAT, K_TRANS,
  K_LFORATE, K_LFODEPTH, K_DLYA, K_DLYB, K_FB, K_WET, K_GRAVITY, K_TRAIL,
  K_LFODEST, K_SWING, K_ROOT, NK
};
#define MORPH 20
enum { U_PCT, U_MS, U_X100, U_HZ100, U_DEST, U_NOTE };

typedef struct {
  int16_t min, max, step;
  uint8_t unit;
  const char *label;
} kdef_t;
static const kdef_t KD[NK] = {
  {0, 100, 1, U_PCT, "DETUNE"},   {0, 100, 1, U_PCT, "D.MIX"},     {0, 100, 1, U_PCT, "SUB"},
  {0, 100, 1, U_PCT, "LEVEL"},    {0, 100, 1, U_PCT, "WIDTH"},     {1, 400, 2, U_MS, "ATTACK"},
  {0, 600, 5, U_MS, "HOLD"},      {10, 1500, 10, U_MS, "RELEASE"}, {0, 100, 1, U_PCT, "DRIVE"},
  {0, 100, 1, U_PCT, "MOD AMT"},  {50, 400, 5, U_X100, "MOD RAT"}, {0, 100, 1, U_PCT, "TRANS"},
  {1, 800, 2, U_HZ100, "RATE"},   {0, 100, 1, U_PCT, "DEPTH"},     {20, 700, 2, U_MS, "TIME A"},
  {20, 700, 2, U_MS, "TIME B"},   {0, 90, 1, U_PCT, "FEEDBK"},     {0, 100, 1, U_PCT, "WET"},
  {0, 100, 1, U_PCT, "GRAVITY"},  {0, 100, 1, U_PCT, "TRAIL"},     {0, 2, 1, U_DEST, "DEST"},
  {0, 100, 1, U_PCT, "SWING"},    {45, 56, 1, U_NOTE, "ROOT"},
};
static const int16_t KDEFAULT[NK] = {40, 65, 30, 70, 60, 8, 70, 220, 15, 25, 100, 20,
                                     35, 40, 180, 266, 45, 38, 45, 55, 1, 0, 45};
static const int16_t SCENE_DEFAULT[4][MORPH] = {
  {15, 50, 20, 65, 40, 5, 50, 150, 5, 10, 100, 10, 20, 15, 150, 220, 25, 20, 30, 30},  /* A clean */
  {60, 80, 35, 75, 70, 3, 50, 180, 55, 55, 150, 50, 50, 50, 120, 180, 40, 30, 60, 40}, /* B driven */
  {35, 60, 25, 60, 55, 50, 400, 800, 10, 20, 75, 5, 15, 60, 320, 440, 60, 65, 25, 85}, /* C wash */
  {20, 40, 15, 55, 30, 2, 20, 80, 20, 35, 200, 70, 80, 20, 90, 140, 15, 15, 70, 15},   /* D sparse */
};

/* lanes: fixed character, as the HTML's */
typedef struct {
  const char *name;
  uint16_t color;
  int8_t oct;
  uint8_t hold, mod, send; /* x100 */
  uint8_t len;
} lane_t;
static const lane_t LANE[LANES] = {
  {"LEAD", RGB(255, 61, 127), 0, 100, 100, 100, 16},
  {"BASS", RGB(255, 180, 84), -1, 60, 40, 30, 5},
  {"PLUCK", RGB(122, 91, 196), 1, 25, 140, 80, 7},
  {"AIR", RGB(201, 174, 252), 2, 160, 60, 100, 11},
};

#define NO_PITCH (-128)
#define NO_BYTE 255
#define NO_HOLD 0xFFFF
enum { C_ALL, C_PROB, C_RATIO };
typedef struct {
  uint8_t on, locks_on;
  int8_t pitch;   /* NO_PITCH or -12..12 */
  uint8_t level;  /* NO_BYTE or 0..100 */
  uint8_t send;   /* NO_BYTE or 0..100 */
  uint8_t cond;   /* index into CONDS */
  uint16_t hold;  /* NO_HOLD or 0..600 ms */
} step_t;

/* trig conditions: always, probability 10..90 %, then n:m (fires on pass n of every m) */
static const uint8_t CONDS[][3] = {
  {C_ALL, 0, 0},    {C_PROB, 10, 0},  {C_PROB, 20, 0},  {C_PROB, 30, 0},  {C_PROB, 40, 0},  {C_PROB, 50, 0},
  {C_PROB, 60, 0},  {C_PROB, 70, 0},  {C_PROB, 80, 0},  {C_PROB, 90, 0},  {C_RATIO, 1, 2}, {C_RATIO, 2, 2},
  {C_RATIO, 1, 3}, {C_RATIO, 2, 3}, {C_RATIO, 3, 3}, {C_RATIO, 1, 4}, {C_RATIO, 2, 4}, {C_RATIO, 3, 4},
  {C_RATIO, 4, 4},
};
#define NCOND (int)(sizeof CONDS / sizeof CONDS[0])

#define MAGIC 0x53440001u
typedef struct {
  uint32_t magic;
  uint8_t bpm, sync, unison, octave;
  uint8_t xy[2]; /* 0..100 */
  uint8_t len[LANES];
  int16_t k[NK];
  int16_t scene[4][MORPH];
  step_t step[LANES][STEPS];
} store_t;
static store_t S;

static void reset_lane(int l) {
  S.len[l] = LANE[l].len;
  for (int s = 0; s < STEPS; s++) {
    step_t *st = &S.step[l][s];
    st->on = st->locks_on = 0;
    st->pitch = NO_PITCH;
    st->level = st->send = NO_BYTE;
    st->hold = NO_HOLD;
    st->cond = 0;
  }
}

static void init_all(void) {
  S.magic = MAGIC;
  S.bpm = 132;
  S.sync = 0;
  S.unison = MAXUNI;
  S.octave = 2; /* 2 = no shift; 0..4 */
  S.xy[0] = S.xy[1] = 50;
  for (int i = 0; i < NK; i++) S.k[i] = KDEFAULT[i];
  for (int c = 0; c < 4; c++)
    for (int i = 0; i < MORPH; i++) S.scene[c][i] = SCENE_DEFAULT[c][i];
  for (int l = 0; l < LANES; l++) reset_lane(l);
  S.step[0][0].on = 1; /* deliberately sparse: open space to fill */
}

/* ================================================================ tables */

static const int16_t TANH[257] = {
  -32767, -32690, -32610, -32530, -32447, -32363, -32277, -32189, -32099, -32007, -31914, -31818, -31721, -31621,
  -31520, -31416, -31310, -31202, -31092, -30980, -30865, -30748, -30629, -30507, -30383, -30256, -30127, -29995,
  -29861, -29724, -29585, -29442, -29297, -29149, -28999, -28845, -28689, -28530, -28368, -28202, -28034, -27863,
  -27688, -27511, -27330, -27146, -26958, -26768, -26574, -26377, -26176, -25972, -25764, -25553, -25338, -25120,
  -24898, -24673, -24444, -24212, -23975, -23735, -23491, -23244, -22993, -22738, -22479, -22217, -21950, -21680,
  -21406, -21128, -20847, -20561, -20272, -19979, -19682, -19381, -19077, -18768, -18456, -18140, -17821, -17498,
  -17171, -16840, -16506, -16168, -15827, -15482, -15133, -14781, -14426, -14068, -13706, -13341, -12973, -12601,
  -12227, -11850, -11470, -11087, -10701, -10312, -9921,  -9528,  -9132,  -8733,  -8333,  -7930,  -7525,  -7118,
  -6709,  -6299,  -5886,  -5473,  -5057,  -4641,  -4223,  -3804,  -3384,  -2963,  -2541,  -2119,  -1696,  -1272,
  -848,   -424,   0,      424,    848,    1272,   1696,   2119,   2541,   2963,   3384,   3804,   4223,   4641,
  5057,   5473,   5886,   6299,   6709,   7118,   7525,   7930,   8333,   8733,   9132,   9528,   9921,   10312,
  10701,  11087,  11470,  11850,  12227,  12601,  12973,  13341,  13706,  14068,  14426,  14781,  15133,  15482,
  15827,  16168,  16506,  16840,  17171,  17498,  17821,  18140,  18456,  18768,  19077,  19381,  19682,  19979,
  20272,  20561,  20847,  21128,  21406,  21680,  21950,  22217,  22479,  22738,  22993,  23244,  23491,  23735,
  23975,  24212,  24444,  24673,  24898,  25120,  25338,  25553,  25764,  25972,  26176,  26377,  26574,  26768,
  26958,  27146,  27330,  27511,  27688,  27863,  28034,  28202,  28368,  28530,  28689,  28845,  28999,  29149,
  29297,  29442,  29585,  29724,  29861,  29995,  30127,  30256,  30383,  30507,  30629,  30748,  30865,  30980,
  31092,  31202,  31310,  31416,  31520,  31621,  31721,  31818,  31914,  32007,  32099,  32189,  32277,  32363,
  32447,  32530,  32610,  32690,  32767,
};
/* (s / 100) ^ 0.62, Q15: swing front-loaded so it shuffles well before 100 % */
static const uint16_t SWING_POW[101] = {
  0,     1886,  2898,  3726,  4454,  5115,  5727,  6301,  6845,  7363,  7860,  8339,  8801,  9249,  9684,
  10107, 10520, 10923, 11317, 11702, 12081, 12452, 12816, 13174, 13526, 13873, 14215, 14551, 14883, 15210,
  15533, 15852, 16168, 16479, 16787, 17091, 17392, 17690, 17985, 18277, 18566, 18853, 19137, 19418, 19697,
  19973, 20247, 20519, 20788, 21056, 21321, 21585, 21846, 22106, 22363, 22619, 22873, 23126, 23376, 23625,
  23873, 24119, 24363, 24606, 24847, 25087, 25326, 25563, 25799, 26034, 26267, 26499, 26730, 26959, 27188,
  27415, 27641, 27866, 28090, 28313, 28534, 28755, 28974, 29193, 29411, 29627, 29843, 30057, 30271, 30484,
  30696, 30907, 31117, 31326, 31535, 31742, 31949, 32155, 32360, 32564, 32767,
};
/* 2^(c / 1200), Q16, c = -35..35 cents */
static const uint16_t CENT_LO[36] = {64224, 64261, 64299, 64336, 64373, 64410, 64447, 64485, 64522, 64559, 64596, 64634,
                                     64671, 64708, 64746, 64783, 64821, 64858, 64896, 64933, 64971, 65008, 65046, 65083,
                                     65121, 65159, 65196, 65234, 65272, 65309, 65347, 65385, 65423, 65460, 65498, 65535};
static const uint32_t CENT_HI[36] = {65536, 65574, 65612, 65650, 65688, 65726, 65764, 65802, 65840, 65878, 65916, 65954,
                                     65992, 66030, 66068, 66106, 66144, 66183, 66221, 66259, 66297, 66336, 66374, 66412,
                                     66451, 66489, 66528, 66566, 66605, 66643, 66682, 66720, 66759, 66797, 66836, 66874};
static uint32_t cents_ratio(int c) { /* Q16 */
  if (c < -35) c = -35;
  if (c > 35) c = 35;
  return c < 0 ? CENT_LO[c + 35] : CENT_HI[c];
}

/* ================================================================ shared state */

static volatile uint8_t playing_audio; /* audio: the clock runs */
static volatile int8_t play_pos[LANES] = {-1, -1, -1, -1};
static volatile uint32_t tick_count;
static volatile uint16_t trig_count[LANES]; /* audio -> loop: notes fired (particles) */
static volatile uint16_t load_pct;          /* audio -> loop: CPU use of the last blocks, % */
static volatile uint8_t uni_cap = MAXUNI;   /* loop -> audio: saws a note may use */

/* ================================================================ the sound */

typedef struct {
  uint8_t active, stage, lane, uni;
  int32_t env, peak, ainc, rdec; /* Q24 */
  int32_t hold_left;
  uint32_t ph[MAXUNI], inc[MAXUNI];
  int16_t gl[MAXUNI], gr[MAXUNI]; /* per-saw gain and pan, Q15 */
  uint32_t sph, sinc;
  int32_t sgain; /* Q15 */
  uint32_t mph, minc;
  int32_t mdev;  /* frequency deviation scale: inc units at full mod, Q15 of the base inc */
  int32_t send;  /* Q15 */
  int32_t tenv, tpeak, tpos; /* noise transient */
} note_t;
static note_t N[NOTES];

typedef struct {
  int32_t wait;
  uint8_t used, lane;
  int8_t pitch;
  uint8_t level, send;
  uint16_t hold;
} event_t;
static event_t EVQ_[EVQ];

static int16_t dly_a[DLEN] POOL, dly_b[DLEN] POOL;
static uint32_t dw;
static int32_t lp_a, lp_b;
static int32_t ta_q8, tb_q8; /* smoothed delay times, frames Q8 */
static uint32_t lfo_ph;
static int32_t lim_env = 32767;

static int32_t frames_per_step(void) {
  uint32_t b = clock_bpm();
  if (b < 2000) b = (uint32_t)S.bpm * 100u;
  return (int32_t)(66150000u / b);
}
static int32_t ms_frames(int32_t ms) { return ms * 441 / 10; }

static void start_note(int lane, int pitch_lock, int level, int send, int hold_ms) {
  const int16_t *k = S.k;
  /* the newest replaces the older of the lane's two notes */
  note_t *a = &N[lane * SLOTS], *b = &N[lane * SLOTS + 1], *n;
  n = !a->active ? a : !b->active ? b : (a->env < b->env ? a : b);
  int note = k[K_ROOT] + pitch_lock + LANE[lane].oct * 12 + (S.octave - 2) * 12;
  if (note < 12) note = 12;
  if (note > 120) note = 120;
  uint32_t base = note_inc(note);
  int uni = S.unison < uni_cap ? S.unison : uni_cap;
  n->active = 0;
  n->lane = (uint8_t)lane;
  n->uni = (uint8_t)uni;
  for (int i = 0; i < uni; i++) {
    /* centred position -1..1 in Q15 */
    int32_t c = uni > 1 ? (int32_t)((i * 2 - (uni - 1)) * 32768 / (uni - 1)) : 0;
    int cents = (int)((c * k[K_DETUNE] * 35) / (32768 * 100));
    n->inc[i] = (uint32_t)(((uint64_t)base * cents_ratio(cents)) >> 16);
    int32_t ac = c < 0 ? -c : c;
    int32_t cw = 32768 - (int32_t)(((int64_t)ac * (100 - k[K_DMIX])) / 100); /* centre weight */
    int32_t g = (19661 + ((13107 * cw) >> 15)) / uni;                         /* (0.6 + 0.4 cw) / N */
    int32_t pan = (int32_t)(((int64_t)c * k[K_WIDTH]) / 100);                 /* -1..1 */
    uint32_t ang = (uint32_t)(pan + 32768) << 13;                             /* (pan + 1) pi / 4 */
    n->gl[i] = (int16_t)((g * sine(ang + 1073741824u)) >> 15);
    n->gr[i] = (int16_t)((g * sine(ang)) >> 15);
  }
  n->sinc = note_inc(note - 12 > 0 ? note - 12 : 0);
  n->sgain = k[K_SUB] * 29491 / 100; /* x 0.9 */
  int modamt = k[K_MODAMT] * LANE[lane].mod; /* x10000 */
  n->minc = (uint32_t)(((uint64_t)base * (uint32_t)k[K_MODRAT] * 655u) >> 16); /* x ratio / 100 */
  n->mdev = modamt > 0 ? (int32_t)((int64_t)modamt * 3 * 32768 / 10000) : 0; /* deviation = mod x 3 x base */
  n->send = send * LANE[lane].send * 32768 / 10000;
  if (n->send > 32767) n->send = 32767;
  int32_t peak = level * 32767 / 100;
  n->peak = peak << 9;
  int32_t atk = ms_frames(k[K_ATK] < 2 ? 2 : k[K_ATK]);
  int32_t rel = ms_frames(k[K_REL] < 10 ? 10 : k[K_REL]);
  n->ainc = n->peak / atk + 1;
  n->rdec = n->peak / rel + 1;
  int32_t h = hold_ms * LANE[lane].hold / 100;
  n->hold_left = ms_frames(h < 10 ? 10 : h);
  n->stage = 0;
  if (n->env > n->peak) n->env = n->peak;
  n->tpeak = k[K_TRANS] * 26214 / 100; /* x 0.8 */
  n->tpos = 0;
  n->active = 1;
  trig_count[lane]++;
}

static void enqueue(int lane, int pitch, int level, int send, int hold, int32_t wait) {
  for (int i = 0; i < EVQ; i++) {
    event_t *e = &EVQ_[i];
    if (!e->used) {
      e->wait = wait;
      e->lane = (uint8_t)lane;
      e->pitch = (int8_t)pitch;
      e->level = (uint8_t)level;
      e->send = (uint8_t)send;
      e->hold = (uint16_t)hold;
      e->used = 1;
      return;
    }
  }
}

static uint8_t cond_count[LANES][STEPS];
static uint32_t master_tick, clock_ticks;

static int fires(int l, int s) {
  step_t *st = &S.step[l][s];
  if (!st->on) return 0;
  const uint8_t *c = CONDS[st->cond < NCOND ? st->cond : 0];
  if (c[0] == C_PROB) return (int)(random32() % 100u) < c[1];
  if (c[0] == C_RATIO) {
    cond_count[l][s] = (uint8_t)(cond_count[l][s] % c[2] + 1);
    return cond_count[l][s] == c[1];
  }
  return 1;
}

static void do_tick(void) { /* one 16th */
  int32_t fps = frames_per_step();
  for (int l = 0; l < LANES; l++) {
    int len = S.len[l] ? S.len[l] : 1;
    int s = (int)(master_tick % (uint32_t)len);
    play_pos[l] = (int8_t)s;
    if (!fires(l, s)) continue;
    /* swing keyed off the lane's own step, so a repeating step swings the same on every pass */
    int32_t sw = (s & 1) ? (int32_t)(((int64_t)SWING_POW[S.k[K_SWING]] * fps) >> 16) : 0;
    step_t *st = &S.step[l][s];
    int use = st->locks_on;
    int pitch = use && st->pitch != NO_PITCH ? st->pitch : 0;
    int level = use && st->level != NO_BYTE ? st->level : S.k[K_LEVEL];
    int send = use && st->send != NO_BYTE ? st->send : 70;
    int hold = use && st->hold != NO_HOLD ? st->hold : S.k[K_HOLD];
    enqueue(l, pitch, level, send, hold, sw);
  }
  master_tick++;
  tick_count = master_tick;
}

static void on_clock(int what) {
  if (what == CLOCK_START) {
    master_tick = clock_ticks = 0;
    for (int l = 0; l < LANES; l++)
      for (int s = 0; s < STEPS; s++) cond_count[l][s] = 0;
    playing_audio = 1;
  } else if (what == CLOCK_CONTINUE) {
    playing_audio = 1;
  } else if (what == CLOCK_STOP) {
    playing_audio = 0;
    for (int l = 0; l < LANES; l++) play_pos[l] = -1;
  } else if (what == CLOCK_TICK && playing_audio) {
    if (clock_ticks % 6 == 0) do_tick();
    clock_ticks++;
  }
}

/* the saw, its jump smoothed (polyBLEP) so high notes don't alias as harshly */
static inline int32_t saw(uint32_t ph, uint32_t inc) {
  int32_t s = (int32_t)(ph >> 16) - 32768;
  if ((int32_t)inc > 4096) {
    uint32_t d = inc >> 12;
    if (ph < inc) { /* just after the jump */
      int32_t t = (int32_t)(((ph >> 12) << 15) / d); /* 0..32767 */
      s -= ((t + t - ((t * t) >> 15) - 32768));
    } else if (ph > ~inc) { /* just before it */
      int32_t t = -(int32_t)((((~ph) >> 12) << 15) / d); /* -32767..0 */
      s -= (((t * t) >> 15) + t + t + 32768);
    }
  }
  return s;
}

static void render(int32_t *out, int32_t *send_out, uint32_t n) {
  for (int v = 0; v < NOTES; v++) {
    note_t *x = &N[v];
    if (!x->active) continue;
    for (uint32_t f = 0; f < n; f++) {
      /* envelope: linear attack, hold, linear release */
      if (x->stage == 0) {
        x->env += x->ainc;
        if (x->env >= x->peak) x->env = x->peak, x->stage = 1;
      } else if (x->stage == 1) {
        if (--x->hold_left <= 0) x->stage = 2;
      } else {
        x->env -= x->rdec;
        if (x->env <= 0) {
          x->env = 0;
          x->active = 0;
          break;
        }
      }
      uint32_t dinc = 0; /* FM: one deviation for all saws (they sit within 2 % of each other) */
      if (x->mdev) {
        int32_t m = sine(x->mph);
        x->mph += x->minc;
        int32_t dev = (int32_t)(((int64_t)m * x->mdev) >> 15); /* Q15 of the base */
        dinc = (uint32_t)(int32_t)(((int64_t)(int32_t)x->inc[0] * dev) >> 15);
      }
      int32_t l = 0, r = 0;
      for (int i = 0; i < x->uni; i++) {
        uint32_t inc = x->inc[i] + dinc;
        x->ph[i] += inc;
        int32_t s = saw(x->ph[i], inc);
        l += (s * x->gl[i]) >> 15;
        r += (s * x->gr[i]) >> 15;
      }
      if (x->sgain) {
        int32_t s = (sine(x->sph) * x->sgain) >> 15;
        x->sph += x->sinc;
        l += s;
        r += s;
      }
      int32_t e = x->env >> 9; /* Q15 */
      l = (l * e) >> 15;
      r = (r * e) >> 15;
      int32_t sd = (((l + r) >> 1) * x->send) >> 15;
      /* noise click: up in 1 ms, down by 30 ms, dry only */
      if (x->tpeak && x->tpos < 1323) {
        int32_t g = x->tpos < 44 ? x->tpeak * x->tpos / 44 : x->tpeak * (1323 - x->tpos) / 1279;
        int32_t z = ((int32_t)random32() >> 16) * g >> 15;
        l += z;
        r += z;
        x->tpos++;
      }
      out[2 * f] += l;
      out[2 * f + 1] += r;
      send_out[f] += sd;
    }
  }
}

static int32_t shape(int32_t x) { /* tanh(1.5 x) / tanh(1.5), x and result in Q15, by table */
  if (x >= 32767) return 32767;
  if (x <= -32767) return -32767;
  int32_t p = (x + 32768) >> 8, fr = (x + 32768) & 255;
  return TANH[p] + (((TANH[p + 1 > 256 ? 256 : p + 1] - TANH[p]) * fr) >> 8);
}

static int32_t dly_read(const int16_t *buf, int32_t t_q8) {
  uint32_t pos = dw - (uint32_t)(t_q8 >> 8);
  int32_t fr = t_q8 & 255;
  int32_t a = buf[pos & DMASK], b = buf[(pos - 1) & DMASK];
  return a + (((b - a) * fr) >> 8);
}

static int32_t clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

void audio(int32_t *out, uint32_t frames) {
  uint32_t t0 = micros();
  const clock_event_t *ev;
  int nev = clock_events(&ev), e = 0;
  const int16_t *k = S.k;
  static int32_t send_buf[256];
  int32_t drive = k[K_DRIVE];
  int32_t din = 32768 + drive * 983;                 /* 1 + 3 d, Q15 */
  int32_t dout = 3276800 / (100 + drive * 6 / 10);   /* 1 / (1 + 0.6 d), Q15 */
  int32_t fb = k[K_FB] * 328 + k[K_TRAIL] * 39;      /* feedback + trail x 0.12, Q15 */
  if (fb > 27853) fb = 27853;                        /* 0.85: no runaway */
  int32_t wet = k[K_WET] * 328;
  int dest = k[K_LFODEST];
  int32_t depth = k[K_LFODEPTH] * 328; /* Q15 */
  uint32_t lfo_inc = (uint32_t)k[K_LFORATE] * 974u * SUB;

  for (uint32_t at = 0; at < frames; at += SUB) {
    uint32_t n = frames - at < SUB ? frames - at : SUB;
    while (e < nev && ev[e].at < at + n) on_clock(ev[e++].what);
    for (int i = 0; i < EVQ; i++) {
      event_t *q = &EVQ_[i];
      if (!q->used) continue;
      if (q->wait < (int32_t)n) {
        start_note(q->lane, q->pitch, q->level, q->send, q->hold);
        q->used = 0;
      } else {
        q->wait -= (int32_t)n;
      }
    }
    for (uint32_t f = 0; f < n; f++) send_buf[f] = 0;
    int32_t *o = out + 2 * at;
    render(o, send_buf, n);

    /* LFO, once a slice */
    int32_t lfo = (sine(lfo_ph) * depth) >> 15; /* -depth..depth */
    lfo_ph += lfo_inc;
    int32_t amp_g = 24576, pan = 0, dmod = 0; /* 0.75: MASTER is on the hardware knob */
    if (dest == 0) amp_g = (int32_t)(24576 + ((lfo * 13107) >> 15) * 3 / 4); /* +-0.4 depth */
    if (dest == 1) pan = lfo;
    if (dest == 2) dmod = lfo * 882 / 32768 * 256;                         /* +-20 ms, Q8 frames */
    /* delay times glide (no zipper) */
    int32_t want_a = ms_frames(k[K_DLYA]) * 256 + dmod, want_b = ms_frames(k[K_DLYB]) * 256 + dmod;
    ta_q8 += (want_a - ta_q8) >> 4;
    tb_q8 += (want_b - tb_q8) >> 4;
    int32_t gl = pan > 0 ? 32768 - pan : 32768, gr = pan < 0 ? 32768 + pan : 32768;

    for (uint32_t f = 0; f < n; f++) {
      /* twin delay: each tap low-passed in its loop; A leans left, B right */
      int32_t ya = dly_read(dly_a, ta_q8), yb = dly_read(dly_b, tb_q8);
      lp_a += ((ya - lp_a) * 8817) >> 15;
      lp_b += ((yb - lp_b) * 6680) >> 15;
      dly_a[dw & DMASK] = (int16_t)clamp16(send_buf[f] + ((lp_a * fb) >> 15));
      dly_b[dw & DMASK] = (int16_t)clamp16(send_buf[f] + ((lp_b * fb) >> 15));
      dw++;
      int32_t wl = ((lp_a * 26214 + lp_b * 13107) >> 15) * wet >> 15;
      int32_t wr = ((lp_a * 13107 + lp_b * 26214) >> 15) * wet >> 15;
      int32_t l = o[2 * f] + wl, r = o[2 * f + 1] + wr;
      /* headroom for four lanes, then drive into the soft shaper */
      l = (int32_t)(((int64_t)l * 19661) >> 15);
      r = (int32_t)(((int64_t)r * 19661) >> 15);
      l = (shape((int32_t)(((int64_t)l * din) >> 15)) * dout) >> 15;
      r = (shape((int32_t)(((int64_t)r * din) >> 15)) * dout) >> 15;
      l = (((l * gl) >> 15) * amp_g) >> 15;
      r = (((r * gr) >> 15) * amp_g) >> 15;
      /* limiter: peaks above the ceiling are pulled down, released over ~190 ms */
      int32_t pk = l < 0 ? -l : l, pr = r < 0 ? -r : r;
      if (pr > pk) pk = pr;
      if (pk > lim_env) lim_env = pk;
      else lim_env -= (lim_env >> 13) + 1;
      if (lim_env < 26000) lim_env = 26000;
      int32_t g = (26000 * 32768) / lim_env; /* <= 32768: a ceiling 2 dB under full scale */
      o[2 * f] = (l * g) >> 15;
      o[2 * f + 1] = (r * g) >> 15;
    }
  }
  /* CPU: this block's time against its 256-frame budget */
  uint32_t used = micros() - t0, budget = frames * 22676u / 1000u; /* us */
  uint32_t pct = budget ? used * 100u / budget : 0;
  load_pct = (uint16_t)((load_pct * 7u + pct) / 8u);
}

/* ================================================================ the panel */

static int8_t white_of[KEY_COUNT], black_of[KEY_COUNT];
enum { BK_RND = 4, BK_SCENE_A = 5, BK_SCENE_D = 8, BK_CLR = 9, BK_INIT = 10 };
enum { PG_OSC, PG_CHR, PG_ENV, PG_LFO, PG_DLY, PG_GLO, PG_SYS, PAGES };
static const char *const PAGE_NAME[PAGES] = {"OSC", "CHR", "ENV", "LFO", "DLY", "GLO", "SYS"};
#define X_BPM 100
#define X_SYNC 101
#define X_UNI 102
#define X_OCT 103
static const uint8_t PAGE_K[PAGES][4] = {
  {K_DETUNE, K_DMIX, K_SUB, K_LEVEL}, {K_DRIVE, K_MODAMT, K_MODRAT, K_TRANS}, {K_ATK, K_HOLD, K_REL, K_WIDTH},
  {K_LFODEST, K_LFORATE, K_LFODEPTH, K_GRAVITY}, {K_DLYA, K_DLYB, K_FB, K_WET}, {X_BPM, K_SWING, K_ROOT, K_TRAIL},
  {X_SYNC, X_UNI, X_OCT, 255},
};
static const uint16_t SCENE_COL[4] = {RGB(255, 61, 127), RGB(122, 91, 196), RGB(255, 180, 84), RGB(92, 225, 230)};
static const char *const NOTE_NAME[12] = {"A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#"};
static const char *const DEST_NAME[3] = {"AMP", "PAN", "DLY"};

static uint8_t lane, page, playing;
static uint16_t held;
static uint8_t held_used; /* something was edited while steps were held: no toggle on release */
static uint8_t save_used;
static uint32_t init_since;
static char msg[28];
static uint32_t msg_until;

enum { D_HEAD = 1, D_TABS = 2, D_GRID = 4, D_LANES = 8, D_PAGES = 16, D_CTX = 32, D_PARAMS = 64, D_XY = 128 };
static uint8_t dirty = 0xFF;

#define C_BG RGB(10, 7, 16)
#define C_PANEL RGB(27, 18, 38)
#define C_LINE RGB(44, 31, 58)
#define C_DIM RGB(95, 82, 112)
#define C_SOFT RGB(154, 138, 168)
#define C_CREAM RGB(242, 233, 228)
#define C_PINK RGB(255, 61, 127)
#define C_AMBER RGB(255, 180, 84)
#define C_CYAN RGB(92, 225, 230)
#define C_VIOLET RGB(122, 91, 196)

static void say(const char *s) {
  int i = 0;
  for (; s[i] && i < 27; i++) msg[i] = s[i];
  msg[i] = 0;
  msg_until = millis() + 1400;
  dirty |= D_CTX;
}
static char *put(char *d, const char *s) {
  while (*s) *d++ = *s++;
  *d = 0;
  return d;
}
static char *put_int(char *d, int32_t v) {
  char b[12];
  format_int(b, v);
  return put(d, b);
}
static void center(int x, int w, int y, const char *s, uint16_t c, int font) {
  text(x + (w - text_width(s, font)) / 2, y, s, c, font);
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void fmt_k(int i, int v, char *val, const char **unit) {
  char *d = val;
  *unit = "";
  switch (KD[i].unit) {
    case U_PCT: put_int(d, v), *unit = "%"; break;
    case U_MS: put_int(d, v), *unit = "MS"; break;
    case U_X100:
      d = put_int(d, v / 100);
      *d++ = '.';
      *d++ = (char)('0' + v / 10 % 10);
      *d++ = (char)('0' + v % 10);
      *d = 0;
      *unit = "X";
      break;
    case U_HZ100:
      d = put_int(d, v / 100);
      *d++ = '.';
      *d++ = (char)('0' + v / 10 % 10);
      *d++ = (char)('0' + v % 10);
      *d = 0;
      *unit = "HZ";
      break;
    case U_DEST: put(d, DEST_NAME[clampi(v, 0, 2)]); break;
    case U_NOTE: put(d, NOTE_NAME[clampi(v - 45, 0, 11)]); break;
    default: break;
  }
}

/* ---- XY morph */
static void apply_morph(void) {
  int32_t x = S.xy[0], y = S.xy[1]; /* 0..100 */
  int32_t wa = (100 - x) * (100 - y), wb = x * (100 - y), wc = (100 - x) * y, wd = x * y; /* sum 10000 */
  for (int i = 0; i < MORPH; i++) {
    int32_t v = S.scene[0][i] * wa + S.scene[1][i] * wb + S.scene[2][i] * wc + S.scene[3][i] * wd;
    S.k[i] = (int16_t)clampi((v + 5000) / 10000, KD[i].min, KD[i].max);
  }
  dirty |= D_PARAMS | D_XY;
}

/* ---- screen */
static void draw_head(void) {
  char b[24], *d;
  rect(0, 0, SCREEN_W, 18, C_BG);
  text(4, 1, "SCENEDRIFT", C_CREAM, FONT_SMALL);
  d = put(b, "CPU ");
  d = put_int(d, load_pct);
  put(d, "%");
  text(100, 1, b, load_pct > 80 ? C_PINK : C_DIM, FONT_SMALL);
  d = put_int(b, S.bpm);
  put(d, " BPM");
  text(SCREEN_W - 4 - text_width(b, FONT_SMALL), 1, b, playing ? C_PINK : C_SOFT, FONT_SMALL);
}

static void draw_tabs(void) {
  for (int l = 0; l < LANES; l++) {
    int x = l * 60;
    const char *b = LANE[l].name;
    if (l == lane) {
      rect(x + 1, 20, 58, 18, C_PANEL);
      frame(x + 1, 20, 58, 18, C_VIOLET);
      rect(x + 5, 27, 5, 5, LANE[l].color);
      text(x + 13, 21, b, C_CREAM, FONT_SMALL);
    } else {
      rect(x + 1, 20, 58, 18, C_BG);
      rect(x + 5, 27, 5, 5, LANE[l].color);
      text(x + 13, 21, b, C_DIM, FONT_SMALL);
    }
  }
}

static int step_x(int s) { return 3 + (s >> 2) * 60 + (s & 3) * 14; }
static int has_locks(const step_t *st) {
  return st->pitch != NO_PITCH || st->level != NO_BYTE || st->send != NO_BYTE || st->hold != NO_HOLD;
}

static int8_t grid_ph = -1;
static void draw_grid(void) {
  int len = S.len[lane];
  int ph = playing ? play_pos[lane] : -1;
  uint16_t col = LANE[lane].color;
  grid_ph = (int8_t)ph;
  rect(0, 40, SCREEN_W, 34, C_BG);
  for (int s = 0; s < STEPS; s++) {
    const step_t *st = &S.step[lane][s];
    int x = step_x(s);
    uint16_t c = s >= len ? C_BG : st->on ? col : C_PANEL;
    if (s == ph) c = st->on ? C_CREAM : C_LINE;
    rect(x, 42, 12, 30, c);
    if (s < len && c == C_BG) frame(x, 42, 12, 30, C_LINE);
    if (s >= len) frame(x, 42, 12, 30, RGB(24, 17, 32));
    if (has_locks(st)) rect(x + 3, 66, 6, 3, st->locks_on ? C_AMBER : C_DIM);
    if (st->cond) rect(x + 3, 45, 6, 2, C_CYAN);
    if (held >> s & 1) frame(x - 1, 41, 14, 32, C_VIOLET);
  }
}

static void draw_lanes(void) { /* every lane at its own length: the polyrhythm, visible */
  rect(0, 76, SCREEN_W, 26, C_BG);
  for (int l = 0; l < LANES; l++) {
    int y = 77 + l * 6, ph = playing ? play_pos[l] : -1;
    if (l == lane) rect(0, y, 2, 4, LANE[l].color);
    for (int s = 0; s < S.len[l]; s++) {
      uint16_t c = S.step[l][s].on ? LANE[l].color : C_PANEL;
      if (s == ph) c = C_CREAM;
      rect(step_x(s), y, 12, 4, c);
    }
  }
}

static void draw_pages(void) {
  rect(0, 104, SCREEN_W, 18, C_BG);
  for (int i = 0; i < PAGES; i++) {
    int x = 1 + i * 34;
    if (i == page) frame(x, 104, 33, 18, C_PINK);
    center(x, 33, 105, PAGE_NAME[i], i == page ? C_CREAM : C_DIM, FONT_SMALL);
  }
}

static int first_held(void) {
  for (int s = 0; s < STEPS; s++)
    if (held >> s & 1) return s;
  return -1;
}
static int lane_key_held(void) {
  for (int k = 0; k < KEY_COUNT; k++)
    if (black_of[k] >= 0 && black_of[k] < LANES && key_held(k)) return black_of[k];
  return -1;
}

static void draw_ctx(void) {
  char b[32], *d;
  rect(0, 124, SCREEN_W, 18, C_BG);
  if (msg[0] && (int32_t)(msg_until - millis()) > 0) {
    center(0, SCREEN_W, 125, msg, C_CYAN, FONT_SMALL);
    return;
  }
  msg[0] = 0;
  int s = first_held(), lk = lane_key_held();
  if (lk >= 0) {
    d = put(b, LANE[lk].name);
    put(d, ": KNOB1 = LENGTH");
    center(0, SCREEN_W, 125, b, C_SOFT, FONT_SMALL);
  } else if (s >= 0) {
    const step_t *st = &S.step[lane][s];
    const uint8_t *c = CONDS[st->cond];
    d = put(b, "STEP ");
    d = put_int(d, s + 1);
    d = put(d, st->locks_on ? " LOCKS ON " : " LOCKS OFF ");
    if (c[0] == C_ALL) put(d, "ALL");
    if (c[0] == C_PROB) d = put_int(d, c[1]), put(d, "%");
    if (c[0] == C_RATIO) d = put_int(d, c[1]), d = put(d, ":"), put_int(d, c[2]);
    center(0, SCREEN_W, 125, b, st->locks_on ? C_AMBER : C_SOFT, FONT_SMALL);
  } else {
    center(0, SCREEN_W, 125, "HOLD A STEP TO LOCK", C_DIM, FONT_SMALL);
  }
}

static void draw_cell(int i, const char *label, const char *val, const char *unit, uint16_t c) {
  int x = 100 + (i & 1) * 70, y = 144 + (i >> 1) * 48;
  rect(x, y, 70, 48, C_BG);
  if (!label[0]) return;
  center(x, 70, y, label, C_SOFT, FONT_SMALL);
  char vu[20], *d = put(vu, val);
  if (text_width(val, FONT_BIG) <= 52 && text_width(unit, FONT_SMALL) <= 16) {
    int w = text_width(val, FONT_BIG) + 2 + text_width(unit, FONT_SMALL);
    int x0 = x + (70 - w) / 2;
    int x1 = text(x0, y + 15, val, c, FONT_BIG);
    text(x1 + 2, y + 30, unit, C_DIM, FONT_SMALL);
  } else {
    put(d, unit);
    center(x, 70, y + 24, vu, c, FONT_SMALL);
  }
}

static void draw_params(void) {
  char v[16];
  const char *u;
  int s = first_held(), lk = lane_key_held();
  if (lk >= 0) {
    put_int(v, S.len[lk]);
    draw_cell(0, "LENGTH", v, "", LANE[lk].color);
    for (int i = 1; i < 4; i++) draw_cell(i, "", "", "", 0);
    return;
  }
  if (s >= 0) {
    const step_t *st = &S.step[lane][s];
    static const char *const L[4] = {"PITCH", "LEVEL", "SEND", "HOLD"};
    int vals[4] = {st->pitch == NO_PITCH ? 0 : st->pitch, st->level == NO_BYTE ? S.k[K_LEVEL] : st->level,
                   st->send == NO_BYTE ? 70 : st->send, st->hold == NO_HOLD ? S.k[K_HOLD] : st->hold};
    int locked[4] = {st->pitch != NO_PITCH, st->level != NO_BYTE, st->send != NO_BYTE, st->hold != NO_HOLD};
    static const char *const UN[4] = {"ST", "%", "%", "MS"};
    for (int i = 0; i < 4; i++) {
      char *d = v;
      if (i == 0 && vals[0] > 0) *d++ = '+';
      put_int(d, vals[i]);
      draw_cell(i, L[i], v, UN[i], locked[i] ? (st->locks_on ? C_AMBER : C_DIM) : C_SOFT);
    }
    return;
  }
  for (int i = 0; i < 4; i++) {
    int id = PAGE_K[page][i];
    if (id == 255) {
      draw_cell(i, "", "", "", 0);
    } else if (id == X_BPM) {
      put_int(v, S.bpm);
      draw_cell(i, "TEMPO", v, "BPM", C_PINK);
    } else if (id == X_SYNC) {
      draw_cell(i, "SYNC", S.sync ? "EXT" : "INT", "", C_PINK);
    } else if (id == X_UNI) {
      put_int(v, S.unison);
      draw_cell(i, "UNISON", v, uni_cap < S.unison ? "CAP" : "SAW", uni_cap < S.unison ? C_AMBER : C_PINK);
    } else if (id == X_OCT) {
      char *d = v;
      if (S.octave > 2) *d++ = '+';
      put_int(d, S.octave - 2);
      draw_cell(i, "OCTAVE", v, "", C_PINK);
    } else {
      fmt_k(id, S.k[id], v, &u);
      draw_cell(i, KD[id].label, v, u, C_PINK);
    }
  }
}

/* ---- the XY square: corners, the puck, and the gravity field of particles */
#define XY_X 4
#define XY_Y 146
#define XY_S 88
typedef struct {
  int32_t x, y, vx, vy; /* Q8, inside the square */
  int16_t life;          /* 0..1000 */
  uint8_t lane;
  int16_t dx, dy;        /* where it was drawn (-1: not) */
} particle_t;
#define NPART 18
static particle_t P[NPART];

static void draw_xy_base(void) {
  rect(XY_X, XY_Y, XY_S, XY_S, C_PANEL);
  frame(XY_X - 1, XY_Y - 1, XY_S + 2, XY_S + 2, C_LINE);
  static const char *const CN[4] = {"A", "B", "C", "D"};
  for (int c = 0; c < 4; c++) {
    int x = XY_X + (c & 1 ? XY_S - 10 : 0), y = XY_Y + (c & 2 ? XY_S - 16 : 0);
    text(x + 1, y, CN[c], SCENE_COL[c], FONT_SMALL);
  }
  for (int i = 0; i < NPART; i++) P[i].dx = -1;
}
static void draw_puck(int on) {
  int px = XY_X + 3 + S.xy[0] * (XY_S - 6) / 100, py = XY_Y + 3 + S.xy[1] * (XY_S - 6) / 100;
  rect(px - 3, py - 3, 7, 7, on ? C_CREAM : C_PANEL);
}

static void spawn(int l) {
  int pick = 0;
  for (int i = 0; i < NPART; i++) {
    if (P[i].life <= 0) {
      pick = i;
      break;
    }
    if (P[i].life < P[pick].life) pick = i;
  }
  particle_t *p = &P[pick];
  if (p->dx >= 0) rect(p->dx, p->dy, 3, 3, C_PANEL);
  p->x = (8 + (int32_t)(random32() % (XY_S - 16))) << 8;
  p->y = 6 << 8;
  p->vx = (int32_t)(random32() % 128u) - 64;
  p->vy = 160 + (int32_t)(random32() % 128u);
  p->life = 1000;
  p->lane = (uint8_t)l;
  p->dx = -1;
}

static void step_particles(void) {
  int32_t g = S.k[K_GRAVITY], decay = 8 + (100 - S.k[K_TRAIL]) * 32 / 100;
  int32_t cx = (XY_S / 2) << 8, cy = (XY_S * 6 / 10) << 8;
  for (int i = 0; i < NPART; i++) {
    particle_t *p = &P[i];
    if (p->dx >= 0) rect(p->dx, p->dy, 3, 3, C_PANEL), p->dx = -1;
    if (p->life <= 0) continue;
    p->vx += ((cx - p->x) >> 8) * g / 300;
    p->vy += ((cy - p->y) >> 8) * g / 300 + 6;
    p->x += p->vx;
    p->y += p->vy;
    p->life = (int16_t)(p->life - decay);
    if (p->x < 0 || p->y < 0 || p->x > (XY_S - 3) << 8 || p->y > (XY_S - 3) << 8 || p->life <= 0) {
      p->life = 0;
      continue;
    }
    p->dx = (int16_t)(XY_X + (p->x >> 8));
    p->dy = (int16_t)(XY_Y + (p->y >> 8));
    uint16_t c = LANE[p->lane].color;
    if (p->life < 400) c = C_DIM;
    rect(p->dx, p->dy, 3, 3, c);
  }
  draw_puck(1);
}

/* ---- edits */
static int accel(int e, int32_t det) {
  static uint32_t last[ENC_COUNT];
  uint32_t now = millis(), dt = now - last[e];
  if (!det) return 0;
  last[e] = now;
  return det * (dt < 25 ? 4 : dt < 60 ? 2 : 1);
}

static void edit_k(int id, int d) {
  if (id == 255 || !d) return;
  if (id == X_BPM) {
    S.bpm = (uint8_t)clampi(S.bpm + d, 60, 200);
    clock_tempo((uint32_t)S.bpm * 100u);
    dirty |= D_HEAD;
  } else if (id == X_SYNC) {
    S.sync = d > 0;
    clock_follow(S.sync ? MIDI_ANY : 0);
  } else if (id == X_UNI) {
    S.unison = (uint8_t)clampi(S.unison + (d > 0 ? 2 : -2), 1, MAXUNI);
    if (S.unison == 2 || S.unison == 4 || S.unison == 6) S.unison++;
  } else if (id == X_OCT) {
    S.octave = (uint8_t)clampi(S.octave + (d > 0 ? 1 : -1), 0, 4);
  } else {
    S.k[id] = (int16_t)clampi(S.k[id] + d * KD[id].step, KD[id].min, KD[id].max);
  }
  dirty |= D_PARAMS;
}

static void edit_lock(int k, int d) {
  for (int s = 0; s < STEPS; s++) {
    if (!(held >> s & 1)) continue;
    step_t *st = &S.step[lane][s];
    if (k == 0) st->pitch = (int8_t)clampi((st->pitch == NO_PITCH ? 0 : st->pitch) + (d > 0 ? 1 : -1), -12, 12);
    if (k == 1) st->level = (uint8_t)clampi((st->level == NO_BYTE ? S.k[K_LEVEL] : st->level) + d, 0, 100);
    if (k == 2) st->send = (uint8_t)clampi((st->send == NO_BYTE ? 70 : st->send) + d, 0, 100);
    if (k == 3) st->hold = (uint16_t)clampi((st->hold == NO_HOLD ? S.k[K_HOLD] : st->hold) + d * 5, 0, 600);
    st->locks_on = 1; /* editing a lock turns the step's locks on, as the HTML's */
  }
  held_used = 1;
  dirty |= D_GRID | D_PARAMS | D_CTX;
}

/* rhythm-aware random: downbeats likely, off-beats less, other 16ths sparse; step 1 always (an anchor) */
static void randomize(int l) {
  int len = S.len[l];
  int density = len * (30 + (int)(random32() % 26u)) / 100;
  if (density < 1) density = 1;
  reset_lane(l);
  S.len[l] = (uint8_t)len;
  S.step[l][0].on = 1;
  for (int n = 1; n < density; n++) {
    uint32_t total = 0;
    uint8_t w[STEPS];
    for (int i = 1; i < len; i++) {
      w[i] = len % 4 == 0 ? (i % 4 == 0 ? 85 : i % 2 == 0 ? 50 : 18) : (i % 2 == 0 ? 45 : 20);
      if (S.step[l][i].on) w[i] = 0;
      total += w[i];
    }
    if (!total) break;
    int32_t r = (int32_t)(random32() % total);
    for (int i = 1; i < len; i++) {
      r -= w[i];
      if (r < 0 && w[i]) {
        S.step[l][i].on = 1;
        break;
      }
    }
  }
}

static void set_page(int p) {
  page = (uint8_t)((p + PAGES) % PAGES);
  dirty |= D_PAGES | D_PARAMS | D_CTX;
}

static void black_down(int bk) {
  if (bk < LANES) {
    lane = (uint8_t)bk;
    dirty = 0xFF & ~D_XY;
    return;
  }
  if (bk == BK_RND) {
    randomize(lane);
    say("RANDOM");
  } else if (bk >= BK_SCENE_A && bk <= BK_SCENE_D) {
    int c = bk - BK_SCENE_A;
    if (button_held(BTN_SAVE)) { /* store the sound in that corner */
      for (int i = 0; i < MORPH; i++) S.scene[c][i] = S.k[i];
      save_used = 1;
      static const char *const M[4] = {"STORED IN A", "STORED IN B", "STORED IN C", "STORED IN D"};
      say(M[c]);
    } else { /* jump there */
      draw_puck(0);
      S.xy[0] = (uint8_t)(c & 1 ? 100 : 0);
      S.xy[1] = (uint8_t)(c & 2 ? 100 : 0);
      apply_morph();
    }
  } else if (bk == BK_CLR) {
    if (held) {
      for (int s = 0; s < STEPS; s++) {
        if (!(held >> s & 1)) continue;
        step_t *st = &S.step[lane][s];
        st->pitch = NO_PITCH;
        st->level = st->send = NO_BYTE;
        st->hold = NO_HOLD;
        st->cond = 0;
        st->locks_on = 0;
      }
      held_used = 1;
      say("STEP CLEARED");
    } else {
      int len = S.len[lane];
      reset_lane(lane);
      S.len[lane] = (uint8_t)len;
      say("LANE CLEARED");
    }
  } else if (bk == BK_INIT) {
    init_since = millis() | 1;
    say("HOLD FOR INIT");
  }
  dirty |= D_GRID | D_LANES | D_PARAMS | D_CTX;
}

static void white_down(int s) {
  if (!held) held_used = 0;
  held |= (uint16_t)(1u << s);
  dirty |= D_GRID | D_CTX | D_PARAMS;
}
static void white_up(int s) {
  if (!(held >> s & 1)) return;
  held &= (uint16_t)~(1u << s);
  if (!held_used && s < S.len[lane]) S.step[lane][s].on ^= 1;
  dirty |= D_GRID | D_LANES | D_CTX | D_PARAMS;
}

void setup(void) {
  static const uint8_t IS_BLACK[12] = {0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0}; /* from F */
  int w = 0, b = 0;
  for (int k = 0; k < KEY_COUNT; k++) {
    white_of[k] = black_of[k] = -1;
    if (IS_BLACK[k % 12]) black_of[k] = (int8_t)b++;
    else white_of[k] = (int8_t)w++;
  }
  if (settings_load(&S, sizeof S) != 0 || S.magic != MAGIC) init_all();
  ta_q8 = ms_frames(S.k[K_DLYA]) * 256;
  tb_q8 = ms_frames(S.k[K_DLYB]) * 256;
  clear(C_BG);
  draw_xy_base();
  clock_tempo((uint32_t)S.bpm * 100u);
  clock_send(1);
  clock_follow(S.sync ? MIDI_ANY : 0);
  led_backlight(1);
  print("SCENEDRIFT ready");
}

void loop(void) {
  uint32_t now = millis();
  /* keys */
  for (int k = 0; k < KEY_COUNT; k++) {
    if (key_pressed(k)) {
      if (white_of[k] >= 0) white_down(white_of[k]);
      else black_down(black_of[k]);
    }
    if (key_released(k)) {
      if (white_of[k] >= 0) white_up(white_of[k]);
      else {
        if (black_of[k] == BK_INIT) init_since = 0;
        dirty |= D_CTX | D_PARAMS;
      }
    }
  }
  if (init_since && now - init_since > 1000) { /* INIT: the sparse start state */
    init_since = 0;
    if (playing) {
      playing = 0;
      clock_stop();
    }
    init_all();
    clock_tempo((uint32_t)S.bpm * 100u);
    clear(C_BG);
    draw_xy_base();
    dirty = 0xFF;
    say("INIT");
  }
  /* buttons */
  if (button_pressed(BTN_PLAY)) {
    playing = !playing;
    playing ? clock_start() : clock_stop();
    dirty |= D_HEAD | D_GRID | D_LANES;
  }
  if (button_pressed(BTN_SAVE)) save_used = 0;
  if (button_released(BTN_SAVE) && !save_used) {
    if (playing) say("STOP TO SAVE");
    else say(settings_save(&S, sizeof S) == 0 ? "SAVED" : "SAVE FAILED");
  }
  if (button_pressed(BTN_EDIT)) {
    if (held) { /* with steps held: their locks on / off */
      int on = !S.step[lane][first_held()].locks_on;
      for (int s = 0; s < STEPS; s++)
        if (held >> s & 1) S.step[lane][s].locks_on = (uint8_t)on;
      held_used = 1;
      dirty |= D_GRID | D_CTX | D_PARAMS;
    } else {
      set_page(PG_OSC);
    }
  }
  if (button_pressed(BTN_SEL)) set_page(PG_CHR);
  if (button_pressed(BTN_ENV)) set_page(PG_ENV);
  if (button_pressed(BTN_LFO)) set_page(PG_LFO);
  if (button_pressed(BTN_FX)) set_page(PG_DLY);
  if (button_pressed(BTN_GLO)) set_page(PG_GLO);
  if (button_pressed(BTN_ARP)) set_page(PG_SYS);
  if (button_pressed(BTN_HOME)) set_page(PG_OSC);
  if (button_pressed(BTN_OCT_DOWN)) S.octave = (uint8_t)clampi(S.octave - 1, 0, 4), dirty |= D_PARAMS, say("OCTAVE DOWN");
  if (button_pressed(BTN_OCT_UP)) S.octave = (uint8_t)clampi(S.octave + 1, 0, 4), dirty |= D_PARAMS, say("OCTAVE UP");

  /* encoders */
  int sel = encoder(ENC_SELECT);
  if (sel) {
    if (held) { /* trig condition of the held steps */
      for (int s = 0; s < STEPS; s++) {
        if (!(held >> s & 1)) continue;
        step_t *st = &S.step[lane][s];
        st->cond = (uint8_t)clampi(st->cond + (sel > 0 ? 1 : -1), 0, NCOND - 1);
        cond_count[lane][s] = 0;
      }
      held_used = 1;
      dirty |= D_GRID | D_CTX;
    } else {
      set_page(page + (sel > 0 ? 1 : -1));
    }
  }
  int ex = accel(ENC_ALGORITHM, encoder(ENC_ALGORITHM)), ey = accel(ENC_PRESETS, encoder(ENC_PRESETS));
  if (ex || ey) {
    draw_puck(0);
    S.xy[0] = (uint8_t)clampi(S.xy[0] + ex * 2, 0, 100);
    S.xy[1] = (uint8_t)clampi(S.xy[1] + ey * 2, 0, 100);
    apply_morph();
  }
  int lk = lane_key_held();
  for (int k = 0; k < 4; k++) {
    int d = accel(ENC_KNOB1 + k, encoder(ENC_KNOB1 + k));
    if (!d) continue;
    if (lk >= 0) {
      if (k == 0) {
        S.len[lk] = (uint8_t)clampi(S.len[lk] + (d > 0 ? 1 : -1), 1, STEPS);
        dirty |= D_TABS | D_GRID | D_LANES | D_PARAMS;
      }
    } else if (held) {
      edit_lock(k, d);
    } else {
      edit_k(PAGE_K[page][k], d);
    }
  }

  /* follow the music */
  if (S.sync && playing != playing_audio) {
    playing = playing_audio;
    dirty |= D_HEAD | D_GRID | D_LANES;
  }
  static uint32_t seen_tick = ~0u;
  if (tick_count != seen_tick) {
    seen_tick = tick_count;
    dirty |= D_LANES;
    if ((playing ? play_pos[lane] : -1) != grid_ph) dirty |= D_GRID;
  }
  static uint16_t seen_trig[LANES];
  for (int l = 0; l < LANES; l++) {
    if (trig_count[l] != seen_trig[l]) {
      seen_trig[l] = trig_count[l];
      spawn(l);
    }
  }
  /* CPU guard: thin the unison when the chip runs short, give it back when there is room */
  static uint32_t over_since, under_since, head_at;
  uint16_t load = load_pct;
  if (load > 85) {
    if (!over_since) over_since = now | 1;
    if (now - over_since > 200 && uni_cap > 3) {
      uni_cap = (uint8_t)(uni_cap - 2);
      over_since = 0;
      say(uni_cap == 5 ? "CPU: UNISON 5" : "CPU: UNISON 3");
      dirty |= D_PARAMS;
    }
  } else {
    over_since = 0;
  }
  if (load < 45 && uni_cap < MAXUNI) {
    if (!under_since) under_since = now | 1;
    if (now - under_since > 3000) {
      uni_cap = (uint8_t)(uni_cap + 2);
      under_since = 0;
      dirty |= D_PARAMS;
    }
  } else {
    under_since = 0;
  }
  if (now - head_at > 500) head_at = now, dirty |= D_HEAD;
  if (msg[0] && (int32_t)(msg_until - now) <= 0) dirty |= D_CTX;

  /* draw what changed; particles at ~12 frames a second */
  if (dirty & D_HEAD) draw_head();
  if (dirty & D_TABS) draw_tabs();
  if (dirty & D_GRID) draw_grid();
  if (dirty & D_LANES) draw_lanes();
  if (dirty & D_PAGES) draw_pages();
  if (dirty & D_CTX) draw_ctx();
  if (dirty & D_PARAMS) draw_params();
  static uint32_t part_at;
  if ((dirty & D_XY) || now - part_at > 80) {
    part_at = now;
    step_particles();
  }
  dirty = 0;

  /* lights */
  int ph = playing ? play_pos[lane] : -1;
  int corner = (S.xy[0] >= 50) | (S.xy[1] >= 50) << 1;
  for (int k = 0; k < KEY_COUNT; k++) {
    int on;
    if (white_of[k] >= 0) {
      int s = white_of[k];
      on = s < S.len[lane] && (S.step[lane][s].on ^ (s == ph));
    } else {
      int bk = black_of[k];
      on = bk == lane || (bk >= BK_SCENE_A && bk <= BK_SCENE_D && bk - BK_SCENE_A == corner);
    }
    led_key(k, on);
  }
  led_button(BTN_PLAY, playing);
  led_button(BTN_EDIT, page == PG_OSC);
  led_button(BTN_SEL, page == PG_CHR);
  led_button(BTN_ENV, page == PG_ENV);
  led_button(BTN_LFO, page == PG_LFO);
  led_button(BTN_FX, page == PG_DLY);
  led_button(BTN_GLO, page == PG_GLO);
  led_button(BTN_ARP, page == PG_SYS);
}
