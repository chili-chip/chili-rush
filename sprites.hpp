#pragma once
// Sprite rectangles in assets/sprites.png (128x392 RGBA, transparent background).
// tools/make_sprites.py draws the sheet; keep both files in step.
#include "32blit.hpp"

namespace spr {
using blit::Rect;

// Player, 16x24 each, feet on the bottom row of the cell.
// An 8-frame run cycle: frames 0-3 on row 0, frames 4-7 on row 176.
const Rect run[8]   = {Rect(0, 0, 16, 24), Rect(16, 0, 16, 24), Rect(32, 0, 16, 24), Rect(48, 0, 16, 24),
                       Rect(0, 176, 16, 24), Rect(16, 176, 16, 24), Rect(32, 176, 16, 24), Rect(48, 176, 16, 24)};
const Rect jump     = Rect(64, 0, 16, 24);   // top of the jump
const Rect jump_up  = Rect(64, 176, 16, 24); // take-off, stretched upward
const Rect jump_down = Rect(80, 176, 16, 24); // falling, legs reaching down
const Rect land     = Rect(96, 176, 16, 24); // landing squash
const Rect slide    = Rect(80, 0, 16, 24);   // crouched, only the bottom ~12 rows used
const Rect slide2   = Rect(112, 176, 16, 24); // second slide frame (dust/cloth flutter), alternates with slide
const Rect dash[2]  = {Rect(96, 0, 16, 24), Rect(112, 0, 16, 24)}; // fire dash, flames
const Rect hurt     = Rect(0, 24, 16, 24);
// Alternates now and then: a dab jump (one-leg hop with a fist up, a leaning
// dab at the top, arms windmilling on the way down) and a knee slide.
const Rect jump_up_alt = Rect(0, 292, 16, 24);
const Rect jump_alt    = Rect(16, 292, 16, 24);
const Rect jump_down_alt = Rect(64, 292, 16, 24);
const Rect slide_alt[2] = {Rect(32, 292, 16, 24), Rect(48, 292, 16, 24)};

// Obstacles, drawn at their nearest (largest) size, bottom-centre anchored.
const Rect fence    = Rect(16, 24, 32, 16);  // LOW: jump over it
const Rect banner   = Rect(48, 24, 32, 32);  // HIGH: banner between two poles, slide under it
const Rect cactus   = Rect(80, 24, 24, 40);  // desert roadside decor
const Rect weed[2]  = {Rect(104, 24, 16, 16), Rect(104, 40, 16, 16)}; // rolling tumbleweed, LOW, moves lanes
const Rect cart     = Rect(0, 48, 32, 16);   // LOW in the city biome (food cart)
const Rect barrier  = Rect(32, 48, 32, 16);  // LOW in the volcano biome (lava rock)
const Rect vending  = Rect(0, 128, 24, 40);  // city roadside decor (glowing vending machine)
const Rect spire    = Rect(24, 128, 24, 40); // BLOCK in the volcano biome (obsidian spire with lava cracks)
const Rect banner_city = Rect(48, 128, 32, 32);  // HIGH in the city: neon sign on two poles, slide under
const Rect banner_lava = Rect(80, 128, 32, 32);  // HIGH in the volcano: stone arch, slide under

// BLOCK obstacles (change lane): parked cars facing the runner; the volcano keeps the spire.
const Rect car_desert = Rect(0, 200, 32, 28);  // dusty pickup truck
const Rect car_city   = Rect(32, 200, 32, 28); // neon street racer with underglow

// Bought looks (STYLE menu): each the same size and job as the obstacle it
// replaces, sharing its paint colours; the cars keep their lamps in place.
const Rect fence_alt   = Rect(0, 316, 32, 16);   // hay bales
const Rect cart_alt    = Rect(32, 316, 32, 16);  // roadworks
const Rect barrier_alt = Rect(64, 316, 32, 16);  // bone pile
const Rect weed_alt[2] = {Rect(96, 316, 16, 16), Rect(112, 316, 16, 16)}; // rolling tyre
const Rect banner_alt      = Rect(0, 332, 32, 32);   // ranch gate
const Rect banner_city_alt = Rect(32, 332, 32, 32);  // sky bridge
const Rect banner_lava_alt = Rect(64, 332, 32, 32);  // rib arch
const Rect spire_alt       = Rect(96, 332, 24, 40);  // crystals
const Rect car_desert_alt  = Rect(0, 364, 32, 28);   // camper van
const Rect car_city_alt    = Rect(32, 364, 32, 28);  // taxi

// Start-of-run camera move: the hero at twice the detail (32x48, drawn at
// half scale) turning from facing us to facing away (run[0] is the last
// view), and the shades it puts on (32x16, top row at the frame's row 17).
const Rect intro_front = Rect(0, 228, 32, 48);
const Rect intro_q1    = Rect(32, 228, 32, 48);  // three quarters, face to the right
const Rect intro_side  = Rect(64, 228, 32, 48);
const Rect intro_q3    = Rect(96, 228, 32, 48);  // three quarters from behind
const Rect shades_front = Rect(0, 276, 32, 16);
const Rect shades_q1    = Rect(32, 276, 32, 16);
const Rect shades_side  = Rect(64, 276, 32, 16);

// Pickups.
const Rect chili[2] = {Rect(0, 64, 8, 12), Rect(8, 64, 8, 12)};
const Rect milk     = Rect(16, 64, 12, 12);  // shield
const Rect sombrero = Rect(28, 64, 16, 12);  // magnet
const Rect golden   = Rect(44, 64, 8, 12);   // golden chili, fills heat
const Rect sparkle[2] = {Rect(52, 64, 8, 8), Rect(60, 64, 8, 8)};

// Roadside decor, bottom-centre anchored.
const Rect agave    = Rect(0, 80, 16, 24);   // desert
const Rect rock     = Rect(16, 80, 16, 12);  // desert
const Rect skull    = Rect(16, 92, 16, 12);  // desert
const Rect sign     = Rect(32, 80, 24, 24);  // all biomes: "PICANTE" sign on a post
const Rect lamp     = Rect(56, 80, 8, 32);   // city: neon street lamp
const Rect tower    = Rect(64, 80, 24, 32);  // city: building with lit windows
const Rect pillar   = Rect(88, 80, 16, 32);  // volcano: basalt pillar
const Rect vent     = Rect(104, 80, 16, 16); // volcano: smoking vent

// HUD icons, 8x8.
const Rect hud_chili  = Rect(0, 112, 8, 8);
const Rect hud_shield = Rect(8, 112, 8, 8);
const Rect hud_flame  = Rect(16, 112, 8, 8);
const Rect hud_magnet = Rect(24, 112, 8, 8);
const Rect hud_star   = Rect(32, 112, 8, 8);
}
