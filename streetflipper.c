// Copyright (C) 2026
// SPDX-License-Identifier: GPL-2.0-or-later
// StreetFlipper - a 2D fighting game for the Flipper Zero, in real grayscale
// on the 1-bit LCD (the flicker technique from our lcdgray experiments).
//
// Two kung-fu fighters on a scrolling Asian-street background. Best of three
// rounds. Health bar and a special bar that charges with hits.
//
// Controls:
//   Left / Right  walk left / right ("forward" = toward the opponent)
//   Up            jump          Down (hold)  crouch
//   Back (short)  attack: forward+Back = kick, Down+Back = low kick,
//                 in the air = jump kick, otherwise = punch
//   OK            special attack (when the special bar is full)
//   Back (hold)   quit
//
// Both fighters use the same drawing, mirrored. Player 2 has a darker suit and
// is run by a simple computer opponent.
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <math.h>
#include <string.h>
#include "streetflipper_art.h"
#include "vs_art.h"
#include "title_art.h"

// ============================ grayscale core ================================
// (the proven flicker technique: every gray tone is made by lighting a pixel in
//  some of P subframes shown faster than the slow LCD can follow)
#define PMAX   8 // highest supported grayscale depth (s8)
#define RATE   1000 // subframe rate in 1/10 Hz (100.0 Hz)
static int g_period = 4; // current depth s3..s8 (adjustable in the settings menu); s4 default

static const uint8_t bayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static uint8_t lut[16][PMAX + 1]; // lut[matrix][level] = bitmask of dark subframes

static void build_lut(void) {
    int P = g_period;
    for(int b = 0; b < 16; b++)
        for(int level = 0; level <= P; level++) {
            uint8_t mask = 0;
            for(int sf = 0; sf < P; sf++) {
                int t = (sf + b * P / 16) % P;
                if((t + 1) * level / P != t * level / P) mask |= (uint8_t)(1 << sf);
            }
            lut[b][level] = mask;
        }
}

// the picture as darkness per pixel: 0 = white, 255 = black
static uint8_t gray[64][128];

static int pixel_level(int g, int x, int y) {
    (void)x; (void)y;                 // no spatial dithering: nearest clean level
    int level = (g * g_period + 127) / 255;
    return level > g_period ? g_period : level;
}

#define PX_IDX(x, y) (((y) >> 3) * 128 + (x))
#define PX_BIT(y)    ((uint8_t)(1 << ((y) & 7)))

// build ONE subframe (sf) into out[1024], straight from gray[][].
// Done per-subframe so we only keep a single 1 KB buffer instead of PERIOD of them.
static void render_subframe(int sf, uint8_t* out) {
    memset(out, 0, 1024);
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 128; x++) {
            int g = gray[y][x];
            if(!g) continue;
            int level = pixel_level(g, x, y);
            if(!level) continue;
            uint8_t bits = lut[bayer4[y & 3][x & 3]][level];
            if(bits & (1 << sf)) out[PX_IDX(x, y)] |= PX_BIT(y);
        }
}

#define RAM_START 0x20000000u
#define RAM_END   0x20040000u
static uint8_t* find_screen_buffer(Canvas* canvas) {
    uintptr_t c = (uintptr_t)canvas;
    if(c < RAM_START || c + 256 * 4 > RAM_END || (c & 3)) return NULL;
    const uint32_t* words = (const uint32_t*)canvas;
    for(int i = 0; i < 256; i++) {
        uint32_t w = words[i];
        if(w < RAM_START || w + 1024 > RAM_END) continue;
        const uint8_t* buf = (const uint8_t*)(uintptr_t)w;
        canvas_clear(canvas);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_dot(canvas, 5, 0);
        canvas_draw_dot(canvas, 3, 9);
        bool ok = buf[5] == 0x01 && buf[128 + 3] == 0x02 && buf[4] == 0 && buf[6] == 0 &&
                  buf[128 + 2] == 0 && buf[128 + 4] == 0;
        canvas_clear(canvas);
        if(ok && buf[5] == 0 && buf[128 + 3] == 0) return (uint8_t*)buf;
    }
    return NULL;
}

static void draw_slow(Canvas* canvas, const uint8_t* picture) {
    static uint8_t xbm[1024];
    memset(xbm, 0, sizeof(xbm));
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 128; x++)
            if(picture[PX_IDX(x, y)] & PX_BIT(y)) xbm[y * 16 + (x >> 3)] |= (uint8_t)(1 << (x & 7));
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_xbm(canvas, 0, 0, 128, 64, xbm);
}

// ============================= drawing helpers ==============================
static void px(int x, int y, uint8_t tone) {
    if(x >= 0 && x < 128 && y >= 0 && y < 64) gray[y][x] = tone;
}

// a thick line segment (capsule) from (x0,y0) to (x1,y1), radius r
static void cap(int x0, int y0, int x1, int y1, int r, uint8_t tone) {
    int minx = (x0 < x1 ? x0 : x1) - r, maxx = (x0 > x1 ? x0 : x1) + r;
    int miny = (y0 < y1 ? y0 : y1) - r, maxy = (y0 > y1 ? y0 : y1) + r;
    if(minx < 0) minx = 0;
    if(miny < 0) miny = 0;
    if(maxx > 127) maxx = 127;
    if(maxy > 63) maxy = 63;
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float len2 = dx * dx + dy * dy;
    for(int y = miny; y <= maxy; y++)
        for(int x = minx; x <= maxx; x++) {
            float t = len2 > 0 ? ((x - x0) * dx + (y - y0) * dy) / len2 : 0.0f;
            if(t < 0) t = 0;
            if(t > 1) t = 1;
            float cx = x0 + t * dx, cy = y0 + t * dy;
            float d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if(d2 <= (float)(r * r) + 0.5f) gray[y][x] = tone;
        }
}

static void disc(int cx, int cy, int r, uint8_t tone) {
    cap(cx, cy, cx, cy, r, tone);
}

static void rect(int x0, int y0, int x1, int y1, uint8_t tone) {
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 > 127) x1 = 127;
    if(y1 > 63) y1 = 63;
    for(int y = y0; y <= y1; y++)
        for(int x = x0; x <= x1; x++) gray[y][x] = tone;
}

static void frame_rect(int x0, int y0, int x1, int y1, uint8_t tone) {
    for(int x = x0; x <= x1; x++) {
        px(x, y0, tone);
        px(x, y1, tone);
    }
    for(int y = y0; y <= y1; y++) {
        px(x0, y, tone);
        px(x1, y, tone);
    }
}

// =============================== background =================================
// The background comes from streetflipper_art.h (bg_data), sampled on the fly.
#define WORLD_W BG_W
#define GROUND  60 // y of the floor line (feet stand here)
// Background brightness band: the darkest a background pixel may get (0=white,
// 255=black). Kept well below 255 so there is NO pure black in the scene and
// the figures (which have a true-black outline) clearly stand out in front.
#define BG_MAXDARK 235
static int bg_px(int wx, int y) {            // darkness 0..255 of a background pixel
    if(wx < 0) wx = 0;
    if(wx >= BG_W) wx = BG_W - 1;
    int idx = y * BG_W + wx; uint8_t b = bg_data[idx >> 2];
    int v = (b >> ((idx & 3) * 2)) & 3;      // 2 bits/pixel, level 0..3
    if(v > 2) v = 2;                         // cap at the 2nd-darkest step: no pure black bg
    return v * BG_MAXDARK / 3;               // 0..2 -> 0..(2/3*BG_MAXDARK), dark-grey max
}

static uint32_t hash2(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}



// =============================== fighters ===================================
#define GROUND_Y   GROUND // feet baseline
#define FIGHT_MINX 12
#define FIGHT_MAXX (WORLD_W - 12)

typedef enum {
    A_IDLE,
    A_WALK,
    A_CROUCH,
    A_JUMP,
    A_PUNCH,
    A_KICK,
    A_LOWKICK,
    A_JUMPKICK,
    A_BLOCK,
    A_HIT,
    A_SPECIAL,
    A_KO,
    A_WIN,
} Action;

typedef struct {
    float x; // world x of the feet
    float yoff; // height above the ground (0 = standing), for jumps
    float vy; // vertical speed
    int dir; // +1 faces right, -1 faces left
    Action act;
    float t; // time spent in the current action
    float walk; // walk animation phase
    int hp; // 0..100
    int sp; // special charge 0..100
    bool attacked; // this attack has already landed its hit
    bool is_ai;
    uint8_t suit; // body tone
    // ai
    float think; // time until the next ai decision
    int ai_move;
    // ko/win animation
    float ko_vx; // backward slide speed while being knocked out
    float cel_t; // little timer for the winner's repeated arm-raise hops
    float kbx; // knockback slide speed when hit (smooth push-back, not teleport)
} Fighter;

// special-attack projectile (travels along the ground; can be jumped over)
typedef struct {
    bool on;
    float x, y, vx;
    int dmg, dir;
    const Fighter* owner;
} Shot;
static Shot shots[2];

static Fighter f1, f2;

typedef struct {
    int move; // -1 left, 0, +1 right
    bool jump, crouch, block;
    bool punch, kick, lowkick, jumpkick, special;
} Intent;

#define SPEED      34.0f // walk speed px/s
#define JUMP_V     -82.0f // higher jump
#define GRAVITY    180.0f
#define SP_MAX     100

static bool grounded(const Fighter* f) {
    return f->yoff <= 0.01f;
}
static bool busy(const Fighter* f) {
    return f->act == A_PUNCH || f->act == A_KICK || f->act == A_LOWKICK || f->act == A_JUMPKICK ||
           f->act == A_HIT || f->act == A_SPECIAL || f->act == A_KO || f->act == A_WIN;
}

static float attack_dur(Action a) {
    switch(a) {
    case A_PUNCH: return 0.34f;
    case A_KICK: return 0.40f;
    case A_LOWKICK: return 0.38f;
    case A_JUMPKICK: return 0.45f;
    case A_SPECIAL: return 0.60f;
    default: return 0.3f;
    }
}

// the reach and damage of each attack
static float attack_reach(Action a) {
    switch(a) {
    case A_PUNCH: return 22.0f;
    case A_KICK: return 27.0f;
    case A_LOWKICK: return 26.0f;
    case A_JUMPKICK: return 24.0f;
    case A_SPECIAL: return 34.0f;
    default: return 0.0f;
    }
}
static int attack_dmg(Action a) {
    switch(a) {
    case A_PUNCH: return 6;
    case A_KICK: return 9;
    case A_LOWKICK: return 7;
    case A_JUMPKICK: return 10;
    case A_SPECIAL: return 30;
    default: return 0;
    }
}

static void start_action(Fighter* f, Action a) {
    f->act = a;
    f->t = 0.0f;
    f->attacked = false;
}

// apply one fighter's intent (what the player or the ai wants to do)
static void apply_intent(Fighter* f, const Intent* in, float dt) {
    // vertical physics
    if(!grounded(f) || f->vy < 0.0f) {
        f->vy += GRAVITY * dt;
        f->yoff -= f->vy * dt; // yoff grows upward
        if(f->yoff <= 0.0f) {
            f->yoff = 0.0f;
            f->vy = 0.0f;
            if(f->act == A_JUMP || f->act == A_JUMPKICK) start_action(f, A_IDLE);
        }
    }

    // knockback: fast push-back that eases out (smooth slide, not a teleport)
    if(f->kbx != 0.0f) {
        f->x += f->kbx * dt;
        f->kbx -= f->kbx * 12.0f * dt;
        if(f->kbx > -1.0f && f->kbx < 1.0f) f->kbx = 0.0f;
        if(f->x < FIGHT_MINX) f->x = FIGHT_MINX;
        if(f->x > FIGHT_MAXX) f->x = FIGHT_MAXX;
    }

    // finish timed actions
    if(busy(f)) {
        f->t += dt;
        float dur = (f->act == A_HIT) ? 0.3f : attack_dur(f->act);
        if(f->act == A_KO || f->act == A_WIN) return; // these end only on round reset
        if(f->t >= dur && grounded(f)) start_action(f, A_IDLE);
        if(f->act == A_JUMPKICK && f->t >= attack_dur(A_JUMPKICK)) {
            // jump kick becomes a plain jump after its active frames
            f->act = A_JUMP;
        }
        // during an attack you keep your motion but can't start another move
        return;
    }

    // start new attacks
    if(in->special && f->sp >= SP_MAX) {
        f->sp = 0;
        start_action(f, A_SPECIAL);
        return;
    }
    if(in->jumpkick && !grounded(f)) {
        start_action(f, A_JUMPKICK);
        return;
    }
    if(in->lowkick && grounded(f)) {
        start_action(f, A_LOWKICK);
        return;
    }
    if(in->kick && grounded(f)) {
        start_action(f, A_KICK);
        return;
    }
    if(in->punch && grounded(f)) {
        start_action(f, A_PUNCH);
        return;
    }
    // jump
    if(in->jump && grounded(f)) {
        f->vy = JUMP_V;
        f->yoff = 0.01f;
        start_action(f, A_JUMP);
    }
    // block / crouch / walk (only while grounded)
    if(grounded(f)) {
        if(in->block)
            start_action(f, A_BLOCK);
        else if(in->crouch)
            start_action(f, A_CROUCH);
        else if(in->move != 0)
            f->act = A_WALK;
        else if(f->act != A_JUMP)
            f->act = A_IDLE;
    }
    // horizontal movement (allowed while walking, blocking is slower, in air drift)
    float sp = SPEED;
    if(f->act == A_BLOCK || in->crouch) sp = SPEED * 0.4f;
    if(!grounded(f)) sp = SPEED * 0.7f;
    if(in->move != 0) {
        f->x += in->move * sp * dt;
        if(grounded(f) && !in->crouch && !in->block && f->act != A_JUMP) f->act = A_WALK;
        f->walk += dt * 6.0f;
    }
    if(f->x < FIGHT_MINX) f->x = FIGHT_MINX;
    if(f->x > FIGHT_MAXX) f->x = FIGHT_MAXX;
}

// where an attack's hitting point is (world x), for range checks
static bool in_active_frames(const Fighter* f) {
    float t = f->t, dur = attack_dur(f->act);
    return t > dur * 0.25f && t < dur * 0.7f;
}

// resolve hits: if `a` is attacking and within reach of `b`, deal damage
static void resolve_hit(Fighter* a, Fighter* b) {
    // special is now a ranged projectile (handled separately), not a melee hit
    bool attacking = (a->act == A_PUNCH || a->act == A_KICK || a->act == A_LOWKICK ||
                      a->act == A_JUMPKICK);
    if(!attacking || a->attacked || !in_active_frames(a)) return;
    float gap = fabsf(a->x - b->x);
    float reach = attack_reach(a->act);
    if(gap > reach || gap < 4.0f) return;
    if((a->x < b->x && a->dir < 0) || (a->x > b->x && a->dir > 0)) return; // facing away
    if(b->act == A_KO || b->act == A_WIN) return;
    a->attacked = true;

    // blocking: holding away from the attacker, standing, not mid-attack
    bool blocking = (b->act == A_BLOCK);
    int dmg = attack_dmg(a->act);
    if(blocking) {
        dmg = dmg / 5 + 1; // chip damage
        b->sp += 3;
    } else {
        start_action(b, A_HIT);
        b->sp += 6;
    }
    b->hp -= dmg;
    if(b->hp < 0) b->hp = 0;
    a->sp += blocking ? 4 : 12;
    if(a->sp > SP_MAX) a->sp = SP_MAX;
    if(b->sp > SP_MAX) b->sp = SP_MAX;
    // knockback as a fast easing slide (set a velocity, physics slides it ~kb px)
    float kb = blocking ? 2.0f : 6.0f;
    b->kbx = (b->x > a->x ? 1.0f : -1.0f) * kb * 12.0f;
}

// keep the two fighters from overlapping
static void separate(void) {
    float gap = f2.x - f1.x;
    float minf = 13.0f;
    if(f1.x <= f2.x) {
        if(gap < minf) {
            float push = (minf - gap) / 2;
            f1.x -= push;
            f2.x += push;
        }
    } else {
        float g2 = f1.x - f2.x;
        if(g2 < minf) {
            float push = (minf - g2) / 2;
            f2.x -= push;
            f1.x += push;
        }
    }
    if(f1.x < FIGHT_MINX) f1.x = FIGHT_MINX;
    if(f2.x > FIGHT_MAXX) f2.x = FIGHT_MAXX;
    if(f1.x > FIGHT_MAXX) f1.x = FIGHT_MAXX;
    if(f2.x < FIGHT_MINX) f2.x = FIGHT_MINX;
}

// ------------------------------ drawing a fighter ---------------------------
// ------------------------------ drawing the fighters ------------------------
// pick the sprite pose for the current action (with simple animation frames)
static int pose_for(const Fighter* f) {
    switch(f->act) {
    // two clearly switching step poses: stride (WALK) <-> passing (IDLE)
    case A_WALK: return ((int)(f->walk * 2.5f) & 1) ? SP_WALK : SP_IDLE;
    case A_CROUCH: return SP_CROUCH;
    case A_JUMP: return SP_JUMP;
    case A_PUNCH: return SP_PUNCH;
    case A_KICK: return SP_KICK;
    case A_LOWKICK: return SP_LOWKICK;
    case A_JUMPKICK: return SP_JUMPKICK;
    case A_BLOCK: return SP_BLOCK;
    case A_HIT: return SP_HIT;
    case A_SPECIAL: return SP_SPECIAL;
    case A_KO: return SP_KO;
    // victory: arms up at the top of each little hop, arms down between hops,
    // so the winner keeps throwing the hands into the air again and again
    case A_WIN: return (f->yoff > 3.0f) ? SP_WIN : SP_IDLE2;
    default: return (((int)(f->t * 2.0f)) & 1) ? SP_IDLE2 : SP_IDLE; // idle breathing
    }
}

// draw a sprite rotated by `ang` radians about its feet anchor (used for the
// knock-out "falling backwards" tween). Inverse-mapped so there are no holes.
static void blit_sprite_rot(const Sprite* s, int fx, int fy, int mirror, float ang, float scale) {
    float ca = cosf(ang), sa = sinf(ang);
    int R = (int)((s->w + s->h) * scale); // generous bounding radius
    for(int SY = fy - R; SY <= fy + R; SY++) {
        if(SY < 0 || SY > 63) continue;
        for(int SX = fx - R; SX <= fx + R; SX++) {
            if(SX < 0 || SX > 127) continue;
            float lx = (float)(SX - fx) / scale, ly = (float)(SY - fy) / scale;
            // rotate screen offset back by -ang into sprite-local space
            float rx = lx * ca + ly * sa;
            float ry = -lx * sa + ly * ca;
            int localX = (int)lroundf(rx);
            int yy = (int)lroundf(ry) + (s->h - 1);
            int xx = mirror ? (s->ax - localX) : (s->ax + localX);
            if(xx < 0 || xx >= s->w || yy < 0 || yy >= s->h) continue;
            int idx = yy * s->w + xx; uint8_t b = s->data[idx >> 1];
            int v = (idx & 1) ? (b & 0xF) : (b >> 4);
            if(!v) continue;
            gray[SY][SX] = (v - 1) * 255 / 4;
        }
    }
}

// draw a rectangular region of a sprite, scaled to fill a destination box
// (nearest-neighbour). Used for the big faces on the VS intro screen.
static void blit_region_scaled(const Sprite* s, int sx0, int sy0, int sw, int sh, int dx0,
                               int dy0, int dw, int dh, int mirror) {
    for(int dy = 0; dy < dh; dy++) {
        int Y = dy0 + dy;
        if(Y < 0 || Y > 63) continue;
        int sv = sy0 + dy * sh / dh;
        for(int dx = 0; dx < dw; dx++) {
            int X = dx0 + dx;
            if(X < 0 || X > 127) continue;
            int su = mirror ? (sx0 + sw - 1 - dx * sw / dw) : (sx0 + dx * sw / dw);
            if(su < 0 || su >= s->w || sv < 0 || sv >= s->h) continue;
            int idx = sv * s->w + su; uint8_t b = s->data[idx >> 1];
            int v = (idx & 1) ? (b & 0xF) : (b >> 4);
            if(!v) continue;
            gray[Y][X] = (v - 1) * 255 / 4;
        }
    }
}

// soft shadow ellipse on the ground under the feet (stays on the ground in jumps)
static void draw_shadow(int cx, int gy, int half) {
    for(int yy = -2; yy <= 2; yy++)
        for(int xx = -half; xx <= half; xx++) {
            float dxn = (float)xx / half, dyn = (float)yy / 2.4f;
            if(dxn * dxn + dyn * dyn <= 1.0f) {
                int X = cx + xx, Y = gy + yy;
                if(X < 0 || X > 127 || Y < 0 || Y > 63) continue;
                int d = gray[Y][X] + (int)(110 * (1 - 0.45f * (dxn * dxn + dyn * dyn)));
                gray[Y][X] = d > 255 ? 255 : d;
            }
        }
}

#define KO_FALL_T 0.45f // seconds for the loser to topple over

// fighters are stored at native resolution and drawn 1:1 (crisp, max detail)
#define FS_N 1
#define FS_D 1
#define FSCALE (1.0f)

// draw a sprite scaled by FS, feet-anchored at (fx,fy)
static void blit_sprite_big(const Sprite* s, int fx, int fy, int mirror) {
    int dw = s->w * FS_N / FS_D, dh = s->h * FS_N / FS_D;
    int axs = s->ax * FS_N / FS_D;
    int dx = mirror ? fx - (dw - 1 - axs) : fx - axs;
    int dy = fy - dh + 1;
    blit_region_scaled(s, 0, 0, s->w, s->h, dx, dy, dw, dh, mirror);
}

static void draw_fighter(const Fighter* f, float cam) {
    const Sprite* set = (f->suit == 0) ? fighter0 : fighter1;
    int mirror = f->dir < 0 ? 1 : 0;
    int fx = (int)(f->x - cam);
    int fy = GROUND - (int)f->yoff;

    // little up-down bob on every other step so walking reads as real stepping
    if(f->act == A_WALK && (((int)(f->walk * 2.5f)) & 1)) fy -= 1;

    int posew = set[pose_for(f)].w * FS_N / FS_D;
    int half = posew / 2 - 1; if(half < 4) half = 4;
    int shrink = (int)(f->yoff / 3);         // smaller shadow when higher up
    if(half - shrink > 3) draw_shadow(fx, GROUND - 1, half - shrink);

    // knock-out: topple backwards (rotated HIT sprite), then switch to the
    // dedicated lying KO sprite, placed so it lines up with the fall (no jump).
    if(f->act == A_KO) {
        if(f->t < KO_FALL_T) {
            const Sprite* hit = &set[SP_HIT];
            float ang = (f->t / KO_FALL_T) * 1.5708f; // 0 -> 90 degrees
            blit_sprite_rot(hit, fx, fy, mirror, mirror ? ang : -ang, FSCALE);
        } else {
            const Sprite* ko = &set[SP_KO];
            int back = -f->dir * ((ko->w / 2 - ko->ax + 2) * FS_N / FS_D);
            blit_sprite_big(ko, fx + back, fy, mirror);
        }
        return;
    }

    const Sprite* s = &set[pose_for(f)];
    blit_sprite_big(s, fx, fy, mirror);      // stored small, drawn 1.3x; mirror to face left
}

// =============================== special fx =================================
static int flash_timer; // frames of full-screen flash left
static float shake; // screen shake amount

// a muzzle/impact flash star at (cx,cy)
static void draw_flash(int cx, int cy, int r) {
    for(int a = 0; a < 8; a++) {
        float ang = a * 0.785f;
        int ex = cx + (int)(cosf(ang) * r), ey = cy + (int)(sinf(ang) * r);
        cap(cx, cy, ex, ey, 1, 0);
    }
    disc(cx, cy, r / 2, 0);
    disc(cx, cy, r / 2 + 1, 10);
}

// ================================= HUD ======================================
static int round_no; // 1..3
static int wins1, wins2;
static float round_time; // seconds left

// inverted bar: FULL = white inside, damage/charge-missing eats away as BLACK
// from the inner end (toward the centre of the screen).
static void draw_bar(int x0, int y0, int w, int h, int val, int maxv, bool depleteFromRight) {
    rect(x0, y0, x0 + w, y0 + h, 0);          // white interior (full)
    frame_rect(x0, y0, x0 + w, y0 + h, 255);  // black border
    int miss = (w - 2) * (maxv - val) / maxv; // black (missing) width
    if(miss < 0) miss = 0;
    if(miss > 0) {
        if(depleteFromRight)
            rect(x0 + 1 + (w - 2 - miss), y0 + 1, x0 + w - 1, y0 + h - 1, 255); // black on right
        else
            rect(x0 + 1, y0 + 1, x0 + miss, y0 + h - 1, 255);                   // black on left
    }
}

static void draw_hud(void) {
    // health bars (deplete toward the centre of the screen)
    draw_bar(2, 1, 54, 4, f1.hp, 100, true);
    draw_bar(72, 1, 54, 4, f2.hp, 100, false);
    // special bars (thinner, below)
    draw_bar(2, 7, 40, 2, f1.sp, SP_MAX, true);
    draw_bar(86, 7, 40, 2, f2.sp, SP_MAX, false);
    // round pips in the middle (won rounds filled)
    for(int i = 0; i < 2; i++) {
        int x = 59 + i * 5;
        frame_rect(x, 1, x + 3, 4, 255);
        if(i < wins1) rect(x + 1, 2, x + 2, 3, 255);
        int xr = 65 - i * 5;
        frame_rect(xr, 1, xr + 3, 4, 255);
        if(i < wins2) rect(xr + 1, 2, xr + 2, 3, 255);
    }
}

// =============================== game logic =================================
typedef enum {
    GS_TITLE,    // start screen (press OK to play)
    GS_VS,       // short "face vs face" intro before the match
    GS_ANNOUNCE, // "ROUND 1" / "FIGHT!"
    GS_FIGHT,
    GS_ROUND_END,
    GS_MATCH_END,
    GS_SETTINGS, // long-OK: volume + grayscale depth (s3..s8)
    GS_INFO      // controls help (several pages)
} GameState;
static GameState gstate;
static GameState prev_state; // state to return to after the settings menu
static float state_t; // time in the current state
static const char* banner; // big text to show, or NULL
static int match_winner; // 0 none, 1 or 2

// menus
static int title_sel;     // start-screen selection (0=START, 1=INFO)
static int info_page;     // which help page is shown
#define INFO_PAGES 3
static int menu_sel;      // selected row in the settings menu (0=SOUND,1=MUSIC,2=GRAYSCALE)
static int vol_sfx = 10;  // hit / action sound volume (0..10)
static int vol_music = 7; // background music volume (0..10)

static void reset_positions(void) {
    f1.x = WORLD_W / 2 - 46;
    f2.x = WORLD_W / 2 + 46;
    f1.yoff = f2.yoff = 0;
    f1.vy = f2.vy = 0;
    f1.dir = 1;
    f2.dir = -1;
    f1.kbx = f2.kbx = 0;
    shots[0].on = shots[1].on = false;
    start_action(&f1, A_IDLE);
    start_action(&f2, A_IDLE);
}

static void start_round(void) {
    f1.hp = f2.hp = 100;
    reset_positions();
    round_time = 60.0f;
    gstate = GS_ANNOUNCE;
    state_t = 0;
    banner = round_no == 1 ? "ROUND 1" : (round_no == 2 ? "ROUND 2" : "FINAL ROUND");
}

static void start_match(void) {
    wins1 = wins2 = 0;
    round_no = 1;
    f1.sp = f2.sp = 0;
    f1.ko_vx = f2.ko_vx = 0;
    f1.cel_t = f2.cel_t = 0;
    match_winner = 0;
    start_round();
}

static void goto_title(void) {
    gstate = GS_TITLE;
    state_t = 0;
    menu_sel = 0;
    title_sel = 0; // START always marked first
    // pose the two fighters for the title/VS backdrop
    f1 = (Fighter){.suit = 0, .is_ai = false};
    f2 = (Fighter){.suit = 1, .is_ai = true};
    f1.dir = 1;
    f2.dir = -1;
}

static void face_each_other(Fighter* a, Fighter* b) {
    if(grounded(a) && !busy(a)) a->dir = (b->x >= a->x) ? 1 : -1;
}

// simple computer opponent: fills an intent for f2 based on the distance to f1
static Intent ai_intent(Fighter* me, Fighter* foe, float dt) {
    Intent in = {0};
    me->think -= dt;
    float gap = fabsf(foe->x - me->x);
    int toward = (foe->x > me->x) ? 1 : -1;

    // block if the opponent is attacking and close
    bool foe_attacks = (foe->act == A_PUNCH || foe->act == A_KICK || foe->act == A_LOWKICK ||
                        foe->act == A_JUMPKICK || foe->act == A_SPECIAL);
    // sometimes block when the opponent attacks from close
    if(foe_attacks && gap < 26 && (hash2((int)(state_t * 40), me->hp) % 2 == 0)) {
        in.block = true;
        in.move = -toward; // step back while blocking
        return in;
    }
    if(me->think > 0) {
        in.move = me->ai_move; // keep doing what we decided (slow reactions)
        return in;
    }
    // think again only every ~0.55..1.25 s -> calm, not constant attacking
    me->think = 0.55f + (float)(hash2((int)(state_t * 30), me->sp) % 70) / 100.0f;

    if(me->sp >= SP_MAX && gap < 34 && (hash2((int)state_t, 7) % 3 == 0)) {
        in.special = true;
        me->ai_move = 0;
        return in;
    }
    // ~1 in 4 decisions: deliberately back away for a bit so it doesn't just
    // cling to the player the whole time
    if(gap < 46 && (hash2((int)(state_t * 17), me->hp + 3) % 4 == 0)) {
        me->ai_move = -toward;
        in.move = me->ai_move;
        return in;
    }
    if(gap > 38) {
        me->ai_move = toward; // far away: close the distance
    } else if(gap > 20) {
        int r = hash2((int)(state_t * 20), me->hp) % 10;
        me->ai_move = 0;
        if(r < 2)
            in.kick = true; // ~20% attack from mid range
        else if(r < 6)
            me->ai_move = toward; // mostly just approach
        // else: wait a moment
    } else {
        int r = hash2((int)(state_t * 25), me->sp) % 10;
        me->ai_move = 0;
        if(r < 2)
            in.punch = true;
        else if(r < 4)
            in.kick = true; // ~40% attack up close
        else if(r < 6)
            me->ai_move = -toward; // back off to keep spacing
        // else: wait / guard
    }
    in.move = me->ai_move;
    return in;
}

// =============================== input ======================================
typedef struct {
    volatile bool held_up, held_down, held_left, held_right;
    volatile bool do_jump, do_attack, do_special, quit;
    // single-press edges, used by the menu screens
    volatile bool do_up, do_down, do_left, do_right, do_ok, do_settings;
} Input;
static Input input;

static void input_cb(const void* value, void* ctx) {
    const InputEvent* e = value;
    Input* s = ctx;
    if(e->key == InputKeyOk) {
        if(e->type == InputTypeShort) {
            s->do_special = true;
            s->do_ok = true;
        }
        if(e->type == InputTypeLong) s->do_settings = true; // long OK opens settings
        return;
    }
    if(e->key == InputKeyBack) {
        if(e->type == InputTypeShort) s->do_attack = true;
        if(e->type == InputTypeLong) s->quit = true;
        return;
    }
    if(e->type == InputTypePress || e->type == InputTypeRelease) {
        bool d = e->type == InputTypePress;
        if(e->key == InputKeyUp) {
            s->held_up = d;
            if(d) {
                s->do_jump = true;
                s->do_up = true;
            }
        }
        if(e->key == InputKeyDown) {
            s->held_down = d;
            if(d) s->do_down = true;
        }
        if(e->key == InputKeyLeft) {
            s->held_left = d;
            if(d) s->do_left = true;
        }
        if(e->key == InputKeyRight) {
            s->held_right = d;
            if(d) s->do_right = true;
        }
    }
    // auto-repeat for left/right so holding changes a volume slider smoothly
    if(e->type == InputTypeRepeat) {
        if(e->key == InputKeyLeft) s->do_left = true;
        if(e->key == InputKeyRight) s->do_right = true;
    }
}

// turn the human input into an intent for f1
static Intent human_intent(Fighter* f) {
    Intent in = {0};
    in.move = (input.held_right ? 1 : 0) - (input.held_left ? 1 : 0);
    in.crouch = input.held_down && grounded(f);
    // blocking = holding away from the opponent, standing, not crouching
    int away = (f->dir > 0) ? -1 : 1;
    if(grounded(f) && !input.held_down && in.move == away) in.block = true;
    if(input.do_jump) in.jump = true;
    if(input.do_special) in.special = true;
    if(input.do_attack) {
        int toward = f->dir;
        if(!grounded(f))
            in.jumpkick = true;
        else if(input.held_down)
            in.lowkick = true;
        else if(in.move == toward)
            in.kick = true;
        else
            in.punch = true;
    }
    input.do_jump = input.do_attack = input.do_special = false;
    return in;
}

// spawn a projectile on the active frame of a special attack (once per special)
static void fire_specials(void) {
    for(int k = 0; k < 2; k++) {
        Fighter* f = k ? &f2 : &f1;
        if(f->act == A_SPECIAL && in_active_frames(f) && !f->attacked) {
            f->attacked = true;
            for(int i = 0; i < 2; i++)
                if(!shots[i].on) {
                    shots[i] = (Shot){.on = true,
                                      .x = f->x + f->dir * 14.0f,
                                      .y = GROUND - 18.0f,
                                      .vx = f->dir * 150.0f,
                                      .dmg = 30,
                                      .dir = f->dir,
                                      .owner = f};
                    break;
                }
        }
    }
}

// move projectiles; they hit a grounded opponent but pass by if it jumps over them
static void update_shots(float dt) {
    for(int i = 0; i < 2; i++) {
        Shot* s = &shots[i];
        if(!s->on) continue;
        s->x += s->vx * dt;
        if(s->x < 2.0f || s->x > WORLD_W - 2.0f) {
            s->on = false;
            continue;
        }
        Fighter* tgt = (s->owner == &f1) ? &f2 : &f1;
        if(tgt->act == A_KO || tgt->act == A_WIN) continue;
        if(fabsf(s->x - tgt->x) < 12.0f) {
            // jump OR duck to dodge; otherwise it hits
            if(tgt->yoff < 14.0f && tgt->act != A_CROUCH) {
                bool blocking = (tgt->act == A_BLOCK);
                int dmg = blocking ? 7 : s->dmg;
                if(!blocking) start_action(tgt, A_HIT);
                tgt->hp -= dmg;
                if(tgt->hp < 0) tgt->hp = 0;
                tgt->sp += 6;
                if(tgt->sp > SP_MAX) tgt->sp = SP_MAX;
                tgt->kbx = (float)s->dir * (blocking ? 6.0f : 26.0f) * 12.0f;
                flash_timer = 1;
                s->on = false;
            }
            // if the opponent is in the air, the shot passes and keeps going
        }
    }
}

// winner keeps hopping with arms up; loser slides/settles after the fall
static void animate_celebration(float dt) {
    Intent none = {0};
    for(int k = 0; k < 2; k++) {
        Fighter* f = k ? &f2 : &f1;
        if(f->act == A_WIN) {
            f->cel_t += dt;
            if(grounded(f) && f->cel_t > 0.55f) { // throw the hands up again
                f->vy = -55.0f;
                f->yoff = 0.01f;
                f->cel_t = 0;
            }
        } else if(f->act == A_KO) {
            f->x += f->ko_vx * dt;
            float fr = grounded(f) ? 6.0f : 1.5f; // more drag once on the floor
            f->ko_vx -= f->ko_vx * fr * dt;
            if(f->ko_vx > -0.5f && f->ko_vx < 0.5f) f->ko_vx = 0;
            if(f->x < FIGHT_MINX) f->x = FIGHT_MINX;
            if(f->x > FIGHT_MAXX) f->x = FIGHT_MAXX;
        }
    }
    apply_intent(&f1, &none, dt);
    apply_intent(&f2, &none, dt);
}

// =============================== main update ================================
static void update_game(float dt) {
    state_t += dt;
    switch(gstate) {
    case GS_TITLE:
        if(input.do_up || input.do_down) title_sel ^= 1; // toggle START / INFO
        if(input.do_settings) {                          // long OK -> settings
            prev_state = GS_TITLE;
            gstate = GS_SETTINGS;
            menu_sel = 0;
            state_t = 0;
        } else if(input.do_ok) {
            if(title_sel == 0) {
                gstate = GS_VS; // START
            } else {
                gstate = GS_INFO; // INFO
                info_page = 0;
            }
            state_t = 0;
        }
        input.do_up = input.do_down = input.do_left = input.do_right = false;
        input.do_ok = input.do_special = input.do_jump = input.do_attack = input.do_settings =
            false;
        break;
    case GS_INFO:
        if(input.do_right && info_page < INFO_PAGES - 1) info_page++;
        if(input.do_left && info_page > 0) info_page--;
        if(input.do_ok || input.do_attack) { // OK or Back returns to the title
            gstate = GS_TITLE;
            state_t = 0;
        }
        input.do_up = input.do_down = input.do_left = input.do_right = false;
        input.do_ok = input.do_special = input.do_jump = input.do_attack = input.do_settings =
            false;
        break;
    case GS_SETTINGS:
        if(input.do_up) menu_sel = (menu_sel + 2) % 3;
        if(input.do_down) menu_sel = (menu_sel + 1) % 3;
        if(menu_sel == 0) { // SOUND
            if(input.do_left && vol_sfx > 0) vol_sfx--;
            if(input.do_right && vol_sfx < 10) vol_sfx++;
        } else if(menu_sel == 1) { // MUSIC
            if(input.do_left && vol_music > 0) vol_music--;
            if(input.do_right && vol_music < 10) vol_music++;
        } else { // GRAYSCALE depth s3..s8
            if(input.do_left && g_period > 3) {
                g_period--;
                build_lut();
            }
            if(input.do_right && g_period < PMAX) {
                g_period++;
                build_lut();
            }
        }
        // long OK or Back closes the settings menu
        if(input.do_settings || input.do_attack) {
            gstate = prev_state;
            state_t = 0;
        }
        input.do_up = input.do_down = input.do_left = input.do_right = false;
        input.do_ok = input.do_special = input.do_jump = input.do_attack = input.do_settings =
            false;
        break;
    case GS_VS:
        // short face-off; OK skips it
        if(state_t > 2.8f || input.do_ok) {
            input.do_ok = input.do_special = false;
            start_match(); // -> GS_ANNOUNCE, round 1
        }
        break;
    case GS_ANNOUNCE:
        if(state_t > 1.2f) {
            gstate = GS_FIGHT;
            state_t = 0;
            banner = "FIGHT!";
        }
        // idle breathing only
        face_each_other(&f1, &f2);
        face_each_other(&f2, &f1);
        break;
    case GS_FIGHT: {
        if(input.do_settings) { // long OK pauses into settings (to test s3..s8 live)
            input.do_settings = input.do_special = input.do_ok = false;
            prev_state = GS_FIGHT;
            gstate = GS_SETTINGS;
            menu_sel = 2; // jump straight to GRAYSCALE
            state_t = 0;
            break;
        }
        if(state_t > 0.6f) banner = NULL; // clear "FIGHT!"
        round_time -= dt;
        face_each_other(&f1, &f2);
        face_each_other(&f2, &f1);
        Intent i1 = human_intent(&f1);
        Intent i2 = f2.is_ai ? ai_intent(&f2, &f1, dt) : (Intent){0};
        apply_intent(&f1, &i1, dt);
        apply_intent(&f2, &i2, dt);
        separate();
        resolve_hit(&f1, &f2);
        resolve_hit(&f2, &f1);
        fire_specials(); // spawn special projectiles
        update_shots(dt); // move them / resolve ranged hits
        // special attack: flash at the moment it is launched
        if((f1.act == A_SPECIAL || f2.act == A_SPECIAL) &&
           (in_active_frames(f1.act == A_SPECIAL ? &f1 : &f2)))
            flash_timer = 1;
        // round end?
        bool ko = (f1.hp <= 0 || f2.hp <= 0 || round_time <= 0);
        if(ko) {
            int winner;
            if(f1.hp == f2.hp)
                winner = 0;
            else
                winner = (f1.hp > f2.hp) ? 1 : 2;
            Fighter* won = NULL;
            Fighter* lost = NULL;
            if(winner == 1) {
                wins1++;
                won = &f1;
                lost = &f2;
            } else if(winner == 2) {
                wins2++;
                won = &f2;
                lost = &f1;
            }
            if(won) {
                start_action(won, A_WIN);
                won->cel_t = 0;
                start_action(lost, A_KO);
                lost->vy = -40.0f;             // small upward pop as it topples (vy<0 = up)
                lost->yoff = 0.01f;
                lost->ko_vx = -lost->dir * SPEED * 1.1f; // slide backwards, away from winner
            }
            banner = winner ? "K.O." : "TIME";
            gstate = GS_ROUND_END;
            state_t = 0;
        }
    } break;
    case GS_ROUND_END:
        animate_celebration(dt);
        if(state_t > 3.0f) {
            if(wins1 >= 2 || wins2 >= 2) {
                match_winner = wins1 >= 2 ? 1 : 2;
                banner = match_winner == 1 ? "YOU WIN" : "YOU LOSE";
                gstate = GS_MATCH_END;
                state_t = 0;
            } else {
                round_no++;
                start_round();
            }
        }
        break;
    case GS_MATCH_END:
        animate_celebration(dt);
        if(state_t > 4.0f || input.do_ok) { // OK returns to the start menu
            input.do_ok = input.do_special = false;
            goto_title();
        }
        break;
    }
    if(flash_timer > 0) flash_timer--;
}

// start screen: the full title artwork (logo + burst baked in)
static void render_title(void) {
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 128; x++) {
            int idx = y * 128 + x; uint8_t b = title_bg_d[idx >> 2];
            int v = (b >> ((idx & 3) * 2)) & 3;    // 2 bits/pixel, level 0..3
            gray[y][x] = (uint8_t)(v * 255 / 3);
        }
}

// VS intro: black background with the big VS growing in the middle.
// (Face portraits were dropped to free RAM for more detailed fighters.)
static void render_vs(void) {
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 128; x++) gray[y][x] = 255; // solid black

    float g = state_t / 1.0f;
    if(g > 1.0f) g = 1.0f;
    float scl = 0.40f + 0.55f * g;                 // 0.40x -> 0.95x, then steady
    int gw = (int)(vs_glyph.w * scl);
    int gh = (int)(vs_glyph.h * scl);
    if(gw > 2 && gh > 2)
        blit_region_scaled(&vs_glyph, 0, 0, vs_glyph.w, vs_glyph.h, 64 - gw / 2, 32 - gh / 2, gw,
                           gh, 0);
}

// settings screen: a left-to-right gray ramp so the chosen s-depth's look is
// visible live behind the menu text.
static void render_settings(void) {
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 128; x++) gray[y][x] = (uint8_t)(x * 255 / 127);
}

// INFO/help screen: a dark panel with a demo sprite on the left (text overlaid)
static void render_info(void) {
    // LIGHT panel: the figure's white outline disappears into the bright background,
    // so the character reads cleanly by its black outline (text drawn black on top).
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 128; x++) gray[y][x] = 25;
    int pose = (info_page == 0) ? SP_WALK : (info_page == 1) ? SP_KICK : SP_SPECIAL;
    const Sprite* s = &fighter0[pose];
    int th = 50, tw = s->w * th / s->h;      // demo figure ~50 px tall
    blit_region_scaled(s, 0, 0, s->w, s->h, 6, 10, tw, th, 0);
}

// draw the whole scene into gray[][]
static void render_scene(void) {
    if(gstate == GS_TITLE) {
        render_title();
        return;
    }
    if(gstate == GS_VS) {
        render_vs();
        return;
    }
    if(gstate == GS_INFO) {
        render_info();
        return;
    }
    if(gstate == GS_SETTINGS) {
        render_settings();
        return;
    }
    // camera follows the PLAYER only (not the midpoint), so the background
    // scrolls when the player walks but stays put when the CPU moves.
    float cam = f1.x - 64.0f;
    if(cam < 0) cam = 0;
    if(cam > WORLD_W - 128) cam = WORLD_W - 128;
    int icam = (int)cam;
    if(shake > 0.5f) icam += (int)shake * ((hash2((int)(state_t * 60), 1) & 1) ? 1 : -1);
    if(icam < 0) icam = 0;
    if(icam > WORLD_W - 128) icam = WORLD_W - 128;

    // background slice
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 128; x++) gray[y][x] = (uint8_t)bg_px(icam + x, y);

    // fighters: draw the farther one first so the nearer overlaps
    if(f1.x <= f2.x) {
        draw_fighter(&f2, icam);
        draw_fighter(&f1, icam);
    } else {
        draw_fighter(&f1, icam);
        draw_fighter(&f2, icam);
    }

    // special-attack flash at the attacker's fist (the launch)
    if(f1.act == A_SPECIAL && in_active_frames(&f1))
        draw_flash((int)(f1.x - icam) + f1.dir * 20, GROUND_Y - 20, 8);
    if(f2.act == A_SPECIAL && in_active_frames(&f2))
        draw_flash((int)(f2.x - icam) + f2.dir * 20, GROUND_Y - 20, 8);

    // flying special projectiles (energy orbs)
    for(int i = 0; i < 2; i++)
        if(shots[i].on) {
            int sx = (int)(shots[i].x - icam), sy = (int)shots[i].y;
            disc(sx, sy, 4, 0);            // bright core
            disc(sx, sy, 5, 10);           // soft edge
            draw_flash(sx, sy, 6);         // spark halo
        }

    draw_hud();

    // full-screen flash on a special hit
    if(flash_timer > 0)
        for(int y = 10; y < 64; y++)
            for(int x = 0; x < 128; x++) gray[y][x] = (uint8_t)(gray[y][x] / 3);
}

// =============================== sound ======================================
static bool speaker_ok;
static bool snd_playing;
static uint32_t snd_stop_at;
static uint32_t g_cpu_us;

static void sfx(float freq, float vol, uint32_t ms) {
    if(!speaker_ok) return;
    float v = vol * (float)vol_sfx / 10.0f;   // scale by the menu's SOUND setting
    if(v < 0.01f) return;
    furi_hal_speaker_start(freq, v);
    snd_playing = true;
    snd_stop_at = DWT->CYCCNT + g_cpu_us * (ms * 1000U);
}
static void sfx_tick(void) {
    if(snd_playing && (int32_t)(DWT->CYCCNT - snd_stop_at) >= 0) {
        furi_hal_speaker_stop();
        snd_playing = false;
    }
}

// --- looping background music (same single piezo channel; sfx take priority) ---
// 0 = short rest. An original, laid-back smooth-jazz loop (~22 s): a ii-V-I
// progression in C with arpeggios, walking-bass notes and chromatic passing tones.
static const uint16_t mus_freq[] = {
    // Cmaj7
    262, 0,   330, 0,   392, 0,   494, 0,   523, 0,   494, 392, 330, 0,   0,   0,
    // Am7
    220, 0,   330, 0,   440, 0,   523, 0,   659, 0,   523, 440, 392, 0,   0,   0,
    // Dm7
    294, 0,   349, 0,   440, 0,   523, 0,   587, 0,   523, 440, 349, 0,   0,   0,
    // G7
    196, 0,   247, 0,   392, 0,   494, 0,   587, 0,   494, 392, 349, 330, 0,   0,
    // Cmaj7 (variation)
    262, 0,   392, 330, 523, 0,   494, 0,   440, 392, 330, 0,   262, 0,   0,   0,
    // Em7
    330, 0,   392, 0,   494, 0,   440, 0,   392, 0,   330, 294, 330, 0,   0,   0,
    // Dm7 - G7
    294, 349, 440, 0,   392, 0,   494, 587, 494, 440, 392, 349, 294, 0,   0,   0,
    // Cmaj7 turnaround
    262, 330, 392, 494, 523, 0,   392, 0,   330, 0,   262, 0,   0,   0,   0,   0,
};
#define MUS_STEPS ((int)(sizeof(mus_freq) / sizeof(mus_freq[0])))
#define MUS_STEP_MS 170 // relaxed, smooth tempo
static int mus_idx;
static uint32_t mus_next;    // CYCCNT when the next note begins
static float mus_sounding;   // freq music currently drives on the speaker (0 = silent)

static void music_tick(void) {
    if(!speaker_ok) return;
    if(vol_music <= 0) {                       // music muted in the menu
        if(mus_sounding > 0 && !snd_playing) furi_hal_speaker_stop();
        mus_sounding = 0;
        return;
    }
    if((int32_t)(DWT->CYCCNT - mus_next) >= 0) { // time for the next note
        mus_idx = (mus_idx + 1) % MUS_STEPS;
        mus_next = DWT->CYCCNT + g_cpu_us * (MUS_STEP_MS * 1000U);
        mus_sounding = -1.0f;                  // force the new note to be applied
    }
    if(snd_playing) {                          // a sound effect is using the speaker
        mus_sounding = 0.0f;
        return;
    }
    float want = (float)mus_freq[mus_idx];
    if(want != mus_sounding) {
        if(want <= 0.0f)
            furi_hal_speaker_stop();
        else
            furi_hal_speaker_start(want, 0.28f * (float)vol_music / 10.0f);
        mus_sounding = want;
    }
}

// ============================ on-screen text ================================
// bold WHITE text with a BLACK outline, so it is readable over any background
static void text_outline(Canvas* c, int x, int y, Align ax, const char* str) {
    canvas_set_color(c, ColorBlack);
    for(int dy = -1; dy <= 1; dy++)
        for(int dx = -1; dx <= 1; dx++) {
            if(!dx && !dy) continue;
            canvas_draw_str_aligned(c, x + dx, y + dy, ax, AlignCenter, str);
        }
    canvas_set_color(c, ColorWhite);
    canvas_draw_str_aligned(c, x, y, ax, AlignCenter, str);
}

// a 0..10 volume bar for the menu
static void draw_vol_bar(Canvas* c, int x, int y, int val) {
    canvas_set_color(c, ColorWhite);
    canvas_draw_box(c, x - 1, y - 1, 46, 7);  // white backing so it reads on any shade
    canvas_set_color(c, ColorBlack);
    canvas_draw_frame(c, x, y, 44, 5);
    int w = 42 * val / 10;
    if(w > 0) canvas_draw_box(c, x + 1, y + 1, w, 3);
}

// --- Flipper button icons on the canvas (white key, black symbol) ---
enum { IC_UP, IC_DOWN, IC_LEFT, IC_RIGHT, IC_OK, IC_BACK };
static void draw_btn(Canvas* c, int x, int y, int which) {
    if(which <= IC_RIGHT) { // square direction key with a filled triangle
        canvas_set_color(c, ColorWhite);
        canvas_draw_box(c, x, y, 13, 13);
        canvas_set_color(c, ColorBlack);
        canvas_draw_frame(c, x, y, 13, 13);
        for(int r = 0; r < 7; r++) {
            if(which == IC_UP) canvas_draw_box(c, x + 6 - r, y + 3 + r, 2 * r + 1, 1);
            else if(which == IC_DOWN) canvas_draw_box(c, x + 6 - r, y + 9 - r, 2 * r + 1, 1);
            else if(which == IC_LEFT) canvas_draw_box(c, x + 3 + r, y + 6 - r, 1, 2 * r + 1);
            else canvas_draw_box(c, x + 9 - r, y + 6 - r, 1, 2 * r + 1);
        }
    } else { // round button: OK (dot) or BACK (return arrow)
        canvas_set_color(c, ColorWhite);
        canvas_draw_disc(c, x + 6, y + 6, 6);
        canvas_set_color(c, ColorBlack);
        canvas_draw_circle(c, x + 6, y + 6, 6);
        if(which == IC_OK) {
            canvas_draw_disc(c, x + 6, y + 6, 1); // centre dot
        } else {
            canvas_draw_line(c, x + 3, y + 7, x + 9, y + 7); // shaft
            canvas_draw_line(c, x + 9, y + 7, x + 9, y + 4); // hook up
            canvas_draw_line(c, x + 2, y + 7, x + 5, y + 4); // arrowhead
            canvas_draw_line(c, x + 2, y + 7, x + 5, y + 10);
        }
    }
}

// one INFO row: up to two button icons, then a word
static void info_row(Canvas* c, int y, int i1, int i2, const char* word) {
    int x = 48;
    if(i1 >= 0) {
        draw_btn(c, x, y, i1);
        x += 15;
    }
    if(i2 >= 0) {
        draw_btn(c, x, y, i2);
        x += 15;
    }
    canvas_set_font(c, FontSecondary);
    canvas_set_color(c, ColorBlack); // black text on the light INFO panel
    canvas_draw_str_aligned(c, x + 2, y + 6, AlignLeft, AlignCenter, word);
}

// everything drawn on top of the flickered picture, per game state
static void draw_overlay(Canvas* c) {
    if(gstate == GS_TITLE) {
        // logo is in render_title; selectable START / INFO with a ">" marker (right side)
        canvas_set_font(c, FontPrimary);
        text_outline(c, 125, 48, AlignRight, title_sel == 0 ? "> START" : "START");
        text_outline(c, 125, 59, AlignRight, title_sel == 1 ? "> INFO" : "INFO");
        return;
    }
    if(gstate == GS_VS) {
        // faces + growing VS are drawn in render_vs; nothing to overlay
        return;
    }
    if(gstate == GS_INFO) {
        // sprite is drawn in render_info; black text/icons on the light panel
        canvas_set_color(c, ColorBlack);
        canvas_set_font(c, FontSecondary);
        char pg[6];
        pg[0] = (char)('1' + info_page);
        pg[1] = '/';
        pg[2] = (char)('0' + INFO_PAGES);
        pg[3] = 0;
        canvas_draw_str_aligned(c, 4, 6, AlignLeft, AlignCenter, pg);
        canvas_set_font(c, FontPrimary);
        const char* titles[3] = {"MOVE", "ATTACK", "SPECIAL"};
        canvas_set_color(c, ColorBlack);
        canvas_draw_str_aligned(c, 92, 6, AlignCenter, AlignCenter, titles[info_page]);
        if(info_page == 0) {
            info_row(c, 15, IC_LEFT, IC_RIGHT, "gehen");
            info_row(c, 31, IC_UP, -1, "springen");
            info_row(c, 46, IC_DOWN, -1, "ducken");
        } else if(info_page == 1) {
            info_row(c, 13, IC_BACK, -1, "schlag");
            info_row(c, 25, IC_RIGHT, IC_BACK, "kick");
            info_row(c, 37, IC_DOWN, IC_BACK, "tief");
            info_row(c, 49, IC_UP, IC_BACK, "sprung");
        } else {
            info_row(c, 14, IC_OK, -1, "spezial");
            info_row(c, 30, IC_OK, -1, "lang=setup");
            info_row(c, 46, IC_DOWN, IC_UP, "weicht");
        }
        // page arrows at the bottom corners (clear of the content words)
        canvas_set_font(c, FontSecondary);
        canvas_set_color(c, ColorBlack);
        if(info_page > 0) canvas_draw_str_aligned(c, 4, 60, AlignLeft, AlignCenter, "<");
        if(info_page < INFO_PAGES - 1)
            canvas_draw_str_aligned(c, 124, 60, AlignRight, AlignCenter, ">");
        return;
    }
    if(gstate == GS_SETTINGS) {
        char v[4];
        canvas_set_font(c, FontPrimary);
        text_outline(c, 64, 7, AlignCenter, "SETTINGS");
        canvas_set_font(c, FontSecondary);
        const char* labels[3] = {"SOUND", "MUSIC", "GRAY"};
        for(int i = 0; i < 3; i++) {
            int y = 22 + i * 12;
            if(i == menu_sel) text_outline(c, 5, y, AlignCenter, ">");
            text_outline(c, 12, y, AlignLeft, labels[i]);
        }
        draw_vol_bar(c, 60, 20, vol_sfx);
        draw_vol_bar(c, 60, 32, vol_music);
        v[0] = 's';
        v[1] = (char)('0' + g_period);
        v[2] = 0;
        text_outline(c, 118, 46, AlignRight, v);
        text_outline(c, 64, 61, AlignCenter, "BACK = close");
        return;
    }
    // in-fight: solid-black player labels in the HUD
    if(gstate == GS_ANNOUNCE || gstate == GS_FIGHT || gstate == GS_ROUND_END ||
       gstate == GS_MATCH_END) {
        canvas_set_font(c, FontSecondary);
        canvas_set_color(c, ColorBlack);
        canvas_draw_str_aligned(c, 2, 14, AlignLeft, AlignCenter, "1P");
        canvas_draw_str_aligned(c, 126, 14, AlignRight, AlignCenter, "CPU");
    }
    if(banner) {
        canvas_set_font(c, FontPrimary);
        text_outline(c, 64, 30, AlignCenter, banner);
    }
}

// =============================== app loop ===================================
int32_t streetflipper_app(void* p) {
    UNUSED(p);
    FuriPubSub* in = furi_record_open(RECORD_INPUT_EVENTS);
    FuriPubSubSubscription* sub = furi_pubsub_subscribe(in, input_cb, &input);
    Gui* gui = furi_record_open(RECORD_GUI);
    Canvas* canvas = gui_direct_draw_acquire(gui);
    uint8_t* screen = find_screen_buffer(canvas);
    g_cpu_us = furi_hal_cortex_instructions_per_microsecond();
    speaker_ok = furi_hal_speaker_acquire(100);

    build_lut();
    goto_title();      // open on the start menu (sets up f1/f2)
    mus_next = DWT->CYCCNT;

    static uint8_t frame[1024]; // one subframe buffer, reused (saves 3 KB RAM)
    int prev_hp1 = 100, prev_hp2 = 100;

    uint32_t next = DWT->CYCCNT;

    while(!input.quit) {
        float dt = (float)g_period / ((float)RATE / 10.0f); // seconds per picture (depends on s)
        // --- one game step + new picture (at the start of a gray cycle) ---
        update_game(dt);
        // hit / action sounds (louder base volume)
        if(f1.hp < prev_hp1 || f2.hp < prev_hp2) {
            bool special = (f1.act == A_SPECIAL || f2.act == A_SPECIAL);
            sfx(special ? 70.0f : 160.0f, special ? 0.9f : 0.6f, special ? 100 : 40);
        }
        prev_hp1 = f1.hp;
        prev_hp2 = f2.hp;
        render_scene();

        // --- send the subframes (g_period of them), each one exactly on time ---
        for(int sf = 0; sf < g_period && !input.quit; sf++) {
            next += g_cpu_us * (10000000U / (uint32_t)RATE);
            int32_t wait = (int32_t)(next - DWT->CYCCNT) / (int32_t)g_cpu_us;
            if(wait < -50000) {
                next = DWT->CYCCNT;
                wait = 0;
            }
            if(wait > 1500)
                furi_delay_ms((uint32_t)(wait - 500) / 1000);
            else
                furi_delay_tick(1);
            while((int32_t)(next - DWT->CYCCNT) > 0) furi_thread_yield();

            render_subframe(sf, frame);
            if(screen)
                memcpy(screen, frame, 1024);
            else
                draw_slow(canvas, frame);
            draw_overlay(canvas);
            canvas_commit(canvas);
            sfx_tick();
            music_tick();
        }
    }

    if(speaker_ok) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
    gui_direct_draw_release(gui);
    furi_record_close(RECORD_GUI);
    furi_pubsub_unsubscribe(in, sub);
    furi_record_close(RECORD_INPUT_EVENTS);
    return 0;
}
