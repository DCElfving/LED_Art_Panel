#ifndef EFFECT2D_H
#define EFFECT2D_H

// ===========================================================================
// Effect2D.h — generative 2D animations for the 32x32 matrix
//
// Four effects (Fire, Game of Life, Rain on Pavement, Aurora) render into the
// shared ws2812fx buffer, which myCustomShow() pushes to the panel. Each
// effect returns its desired frame delay in ms. current_2d_effect_id selects
// the active one; set2DEffect() switches and applies a sensible default speed.
// ===========================================================================

#include "MinimalFX.h"   // Lightweight WS2812FX stand-in

extern WS2812FX ws2812fx;

// ===========================================================================
// Math helpers
// ===========================================================================

// Map 2D coordinates to a 1D buffer index.
uint16_t XY(uint8_t x, uint8_t y) {
  if (x >= PANEL_RES_X || y >= PANEL_RES_Y) return 0;
  return y * PANEL_RES_X + x;
}

// Fast 8-bit math
uint8_t scale8(uint8_t i, uint8_t scale) {
  return ((uint16_t)i * (uint16_t)(scale + 1)) >> 8;
}

// Saturating subtraction / addition
uint8_t qsub8(uint8_t i, uint8_t j) {
  return (i > j) ? (i - j) : 0;
}

uint8_t qadd8(uint8_t i, uint8_t j) {
  uint16_t s = (uint16_t)i + (uint16_t)j;
  return (s > 255) ? 255 : (uint8_t)s;
}

// Simple pseudo-random noise generator (Xorshift)
uint8_t noise2d(uint16_t x, uint16_t y) {
  uint32_t h = (x + y * 57);
  h ^= h << 13;
  h ^= h >> 17;
  h ^= h << 5;
  return (uint8_t)(h);
}

// Interpolated noise
uint8_t inoise8(uint16_t x, uint16_t y) {
    uint16_t X = x >> 8;
    uint16_t Y = y >> 8;
    uint16_t x_frac = x & 0xFF;
    uint16_t y_frac = y & 0xFF;

    uint8_t n00 = noise2d(X, Y);
    uint8_t n10 = noise2d(X + 1, Y);
    uint8_t n01 = noise2d(X, Y + 1);
    uint8_t n11 = noise2d(X + 1, Y + 1);

    uint8_t nx0 = n00 + (((n10 - n00) * x_frac) >> 8);
    uint8_t nx1 = n01 + (((n11 - n01) * x_frac) >> 8);

    return nx0 + (((nx1 - nx0) * y_frac) >> 8);
}

// ===========================================================================
// Per-effect persistent state
// ===========================================================================

static byte    heat[PANEL_RES_X][PANEL_RES_Y];  // Fire: heat map
static uint8_t gol_grid[32][32];                // Game of Life: current cells
static uint8_t gol_next[32][32];                // Game of Life: next generation

// ===========================================================================
// Effect implementations
// ===========================================================================

// 1. Fire — cozy campfire, taller/hotter toward the center.
uint16_t effect_fire(void) {
  uint16_t speed = ws2812fx.getSpeed();
  static uint16_t firePhase = 0;
  firePhase += 1 + (speed >> 6);
  // Cozy campfire tuning: slower motion, gentler cooling, moderate sparks
  int cooling = 34 + ((speed * 28) >> 8);     // ~34..61
  int sparking = 72 + ((speed * 32) >> 8);    // ~72..103

  for(int x = 0; x < PANEL_RES_X; x++) {
      int distCenter = abs(x - (PANEL_RES_X / 2));
      int core = 255 - (distCenter * 10); // center hotter/taller than sides
      if (core < 64) core = 64;
      uint8_t wobble = noise2d((uint16_t)(x * 17), firePhase) >> 2; // 0..63
      int columnProfile = core + (int)wobble - 24;
      if (columnProfile < 48) columnProfile = 48;
      if (columnProfile > 255) columnProfile = 255;

      for( int i = 0; i < PANEL_RES_Y; i++) {
        // Pseudo-random cooldown
        int cooldown = (int)random(0, ((cooling * 10) / PANEL_RES_Y) + 2);
        if(cooldown > heat[x][i]) heat[x][i] = 0;
        else heat[x][i] -= cooldown;
      }

      for( int k = PANEL_RES_Y - 1; k >= 2; k--) {
        heat[x][k] = (heat[x][k - 1] + heat[x][k - 2] + heat[x][k - 2]) / 3;
      }

      int localSpark = (sparking * columnProfile) >> 8;
      if( random(255) < localSpark ) {
        int y = random(4);
        int h = heat[x][y] + random(110, 190);
        heat[x][y] = (h > 255 ? 255 : h);
      }

      for( int j = 0; j < PANEL_RES_Y; j++) {
         // Subtle stochastic sway per-column/per-row (avoid synchronized bending)
         uint8_t n = noise2d((uint16_t)(x * 47 + j * 9 + (firePhase >> 2)), (uint16_t)(j * 29 + firePhase));
         int16_t sway = 0;
         if (n > 220) sway = 1;
         else if (n < 34) sway = -1;
         if (j > 17) sway *= 2; // just a touch more motion near the top
         int sx = x + sway;
         if (sx < 0) sx = 0;
         if (sx >= PANEL_RES_X) sx = PANEL_RES_X - 1;

         byte temperature = heat[sx][j];
         // Warm campfire palette (deep red -> amber -> soft warm white)
         uint8_t r = 0, g = 0, b = 0;
         if (temperature < 90) {
            uint8_t t = (uint8_t)((temperature * 255U) / 90U);
            r = t >> 1;
            g = t >> 4;
         } else if (temperature < 180) {
            uint8_t t = (uint8_t)(((uint16_t)(temperature - 90) * 255U) / 90U);
            r = 200;
            g = (50 + (t >> 1));
            b = t >> 5;
         } else {
            uint8_t t = (uint8_t)(((uint16_t)(temperature - 180) * 255U) / 75U);
            if (t > 255) t = 255;
            r = 255;
            g = (180 + (t >> 2));
            b = (40 + (t >> 3));
         }
         // Keep flames lower: strong fade in upper rows
         uint8_t hfade = 255;
         int startFade = 15 + (columnProfile >> 5); // center fades later
         if (j > startFade) {
            uint16_t over = (uint16_t)(j - startFade) * (10U + (distCenter >> 1));
            if (over > 255) over = 255;
            hfade = 255 - (uint8_t)over;
         }
         r = scale8(scale8(r, hfade), columnProfile);
         g = scale8(scale8(g, hfade), columnProfile);
         b = scale8(scale8(b, hfade), columnProfile);
         uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
         ws2812fx.setPixelColor(XY(x, (PANEL_RES_Y-1) - j), color);
      }
  }
  // Slower/cozier frame cadence
  return 125 - ((uint32_t)speed * 75) / 255; // ~125ms .. 50ms
}


// 2. Game Of Life
uint16_t effect_game_of_life(void) {
    static bool init = false;
    static uint16_t phase = 0;
    uint16_t speed = ws2812fx.getSpeed();
    uint16_t delay = 420 - (((uint32_t)speed * 300) >> 8);
    phase += 2 + (speed >> 6);

    if (!init || random(1000) == 0) {
        for(int x=0; x<PANEL_RES_X; x++)
            for(int y=0; y<PANEL_RES_Y; y++)
                gol_grid[x][y] = random(2);
        init = true;
    }

    // Evolve — store neighbor count in gol_next upper bits (bit 0 = alive)
    for (int x = 0; x < PANEL_RES_X; x++) {
        for (int y = 0; y < PANEL_RES_Y; y++) {
            uint8_t nb = 0;
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++) {
                    if (dx == 0 && dy == 0) continue;
                    if (gol_grid[(x+dx+PANEL_RES_X)%PANEL_RES_X][(y+dy+PANEL_RES_Y)%PANEL_RES_Y]) nb++;
                }
            uint8_t alive = (gol_grid[x][y]) ? (nb == 2 || nb == 3) : (nb == 3);
            gol_next[x][y] = alive | (nb << 1); // pack: bit0=state, bits1-4=neighbors
        }
    }

    // Render
    for (int x = 0; x < PANEL_RES_X; x++) {
        for (int y = 0; y < PANEL_RES_Y; y++) {
            uint8_t packed = gol_next[x][y];
            gol_grid[x][y] = packed & 1;
            // Aquatic background: single noise, direct teal
            uint8_t n = inoise8((uint16_t)(x * 130 + (phase << 1)), (uint16_t)(y * 120 + phase));
            uint8_t bgBri = 14 + (n >> 3);
            if (bgBri > 80) bgBri = 80;
            uint8_t bgR = scale8(12, bgBri);
            uint8_t bgG = scale8(180, bgBri);
            uint8_t bgB = scale8(160, bgBri);

            if (packed & 1) {
                 uint8_t nb = packed >> 1;
                 uint8_t cellBri = 150 + nb * 18;
                 ws2812fx.setPixelColor(XY(x,y),
                    qadd8(bgR, scale8(230, cellBri)),
                    qadd8(bgG, scale8(222, cellBri)),
                    qadd8(bgB, scale8(210, cellBri)));
            } else {
                 ws2812fx.setPixelColor(XY(x,y), bgR, bgG, bgB);
            }
        }
    }
    return delay;
}

// 3. Rain On Pavement: top-down puddle ripples, no visible falling drops
uint16_t effect_rain_pavement(void) {
    static bool init = false;
    static uint16_t t = 0;
    static uint8_t rx[10];
    static uint8_t ry[10];
    static uint8_t rr_q4[10];
    static uint8_t rv_q4[10];
    static uint8_t rl[10];
    uint16_t speed = ws2812fx.getSpeed();
    t += 1 + (speed >> 8);

    if (!init) {
      for (uint8_t i = 0; i < 10; i++) {
        rl[i] = 0;
        rr_q4[i] = 0;
        rv_q4[i] = 8 + random(9); // 0.5..1.0 px/frame
      }
      init = true;
    }

    // Spawn sparse random impacts; higher speed = more frequent raindrops.
    uint8_t spawnChance = 10 + (speed >> 5); // 10..17 (out of 255)
    if ((uint8_t)random(256) < spawnChance) {
      uint8_t slot = 0;
      uint8_t weakest = 255;
      for (uint8_t i = 0; i < 10; i++) {
        if (rl[i] == 0) { slot = i; weakest = 0; break; }
        if (rl[i] < weakest) { weakest = rl[i]; slot = i; }
      }
      rx[slot] = (uint8_t)random(PANEL_RES_X);
      ry[slot] = (uint8_t)random(PANEL_RES_Y);
      rr_q4[slot] = 0;
      rv_q4[slot] = 9 + (uint8_t)random(12);   // different ripple sizes/speeds
      rl[slot] = 120 + (uint8_t)random(90);    // lifetime
    }

    // Advance ripple radii and decay
    for (uint8_t i = 0; i < 10; i++) {
      if (rl[i] == 0) continue;
      rr_q4[i] += rv_q4[i];
      if (rl[i] > 0) rl[i]--;
    }

    // Puddle background: soft dark grays with slow wet-texture drift
    for (int y = 0; y < PANEL_RES_Y; y++) {
      for (int x = 0; x < PANEL_RES_X; x++) {
        uint8_t n1 = noise2d((uint16_t)(x * 15 + (t >> 2)), (uint16_t)(y * 17 + (t >> 3)));
        uint8_t n2 = noise2d((uint16_t)(x * 29 + (t >> 1)), (uint16_t)(y * 11 + (t >> 2)));
        uint8_t n = (n1 + n2) >> 1;
        uint8_t base = 11 + (n >> 5); // 11..18
        uint8_t r = base;
        uint8_t g = base;
        uint8_t b = base + 1;

        // Add expanding ripple rings from active impacts.
        uint16_t ripple = 0;
        for (uint8_t i = 0; i < 10; i++) {
          if (rl[i] == 0) continue;
          int16_t dx = (int16_t)x - rx[i];
          int16_t dy = (int16_t)y - ry[i];
          // True Euclidean distance for round ripples
          uint8_t dist = (uint8_t)sqrtf((float)(dx * dx + dy * dy));
          uint8_t rad = rr_q4[i] >> 4;
          uint8_t d = (dist > rad) ? (dist - rad) : (rad - dist);
          uint8_t ring = qsub8(70, d * 18); // narrow ring profile
          // Fading amplitude over ripple lifetime
          ring = scale8(ring, rl[i]);
          ripple += ring;
        }
        if (ripple > 120) ripple = 120;

        uint8_t lift = (uint8_t)ripple;
        ws2812fx.setPixelColor(XY(x, y), qadd8(r, lift), qadd8(g, lift), qadd8(b, lift));
      }
    }
    return 46;
}

// 4. Aurora: sweeping vertical curtain ribbons across the full grid
uint16_t effect_aurora(void) {
    static uint32_t phase = 0;
    uint16_t speed = ws2812fx.getSpeed();
    phase += 1 + (speed >> 5);

    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // Two noise layers: curtain structure + organic drift
            uint8_t n1 = inoise8(
                (uint16_t)(x * 180 + (phase >> 1)),
                (uint16_t)(y * 50 + (phase >> 2))
            );
            uint8_t n2 = inoise8(
                (uint16_t)(x * 70 + 3000 + phase),
                (uint16_t)(y * 30 + (phase >> 1))
            );

            // Combine for ribbon shape
            uint16_t combined = ((uint16_t)n1 * 2 + (uint16_t)n2) / 3;

            // Sharpen into ribbon bands with soft edges
            uint8_t ribbon;
            if (combined > 155) ribbon = 255;
            else if (combined > 105) ribbon = (uint8_t)(((uint16_t)(combined - 105) * 255) / 50);
            else if (combined > 85) ribbon = (uint8_t)(((uint16_t)(combined - 85) * 60) / 20);
            else ribbon = 0;

            // Subtle vertical intensity variation — brighter toward top
            uint8_t vFade = 200 + (uint8_t)(((uint16_t)(PANEL_RES_Y - 1 - y) * 55) / PANEL_RES_Y);
            uint8_t bri = scale8(ribbon, vFade);

            if (bri < 5) {
                // Dark sky with sparse star twinkle
                uint8_t star = noise2d((uint16_t)(x * 37 + 99), (uint16_t)(y * 53 + 77));
                if (star > 250) {
                    uint8_t sv = 25 + (star & 0x07) * 4;
                    ws2812fx.setPixelColor(XY(x, y), sv, sv, sv);
                } else {
                    ws2812fx.setPixelColor(XY(x, y), 1, 2, 4);
                }
                continue;
            }

            // Color driven by n1 — varies independently of brightness
            uint8_t r, g, b;
            if (n1 < 120) {
                // Green — dominant aurora color
                r = scale8(8, bri);
                g = scale8(200 + (n1 >> 3), bri);
                b = scale8(30 + (n1 >> 2), bri);
            } else if (n1 < 180) {
                // Cyan-teal transition
                uint8_t t = (uint8_t)(((uint16_t)(n1 - 120) * 255) / 60);
                r = scale8(8 + (t >> 3), bri);
                g = scale8(180 - (t >> 2), bri);
                b = scale8(80 + (t >> 1), bri);
            } else {
                // Pink-magenta highlights
                uint8_t t = (uint8_t)(((uint16_t)(n1 - 180) * 255) / 75);
                r = scale8(120 + (t >> 1), bri);
                g = scale8(20 + (t >> 3), bri);
                b = scale8(90 + (t >> 2), bri);
            }
            ws2812fx.setPixelColor(XY(x, y), r, g, b);
        }
    }
    return 50 - ((uint32_t)speed * 22) / 255; // ~50ms..28ms
}

// ===========================================================================
// Effect selection & setup
// ===========================================================================

volatile uint8_t current_2d_effect_id = 0;

uint16_t effect_2d_wrapper(void) {
  switch (current_2d_effect_id) {
    case 1: return effect_fire();
    case 2: return effect_game_of_life();
    case 3: return effect_rain_pavement();
    case 4: return effect_aurora();
    default: return effect_fire();
  }
}

// Initialize Custom Effects
void setupCustomEffects() {
   ws2812fx.setCustomMode(0, F("2D Matrix Effects"), effect_2d_wrapper);
}

#ifndef FX_MODE_CUSTOM
  #define FX_MODE_CUSTOM 57
#endif

uint8_t defaultSpeedFor2DEffect(uint8_t id) {
  switch (id) {
    case 1: return 255; // Fire
    case 2: return 237; // Game of Life
    case 3: return 52;  // Rain
    case 4: return 128; // Aurora
    default: return 128;
  }
}

void set2DEffect(uint8_t id) {
  if (id > 4 || id < 1) id = 1; // Check bounds (1-4)
  current_2d_effect_id = id;
  ws2812fx.setSpeed(defaultSpeedFor2DEffect(id));
  ws2812fx.setMode(FX_MODE_CUSTOM + 0); // Always slot 0
  ws2812fx.trigger();
}

#endif
