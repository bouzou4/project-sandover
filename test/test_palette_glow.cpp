#include "game/graphics/opengl_renderer/background/PaletteGlow.h"
#include "game/graphics/opengl_renderer/background/Unbake.h"
#include "gtest/gtest.h"

// Expected values: `python -m sandover.unbake.palette_glow` (sandover/tools), the same rule in numpy.

namespace {
// Mountain Pass (ogre) fitted slots: day 0-1 with sun, night 2-3 without
constexpr float kA[4][3] = {{305.4f, 307.6f, 311.9f},
                            {360.0f, 354.2f, 359.7f},
                            {193.9f, 201.4f, 281.1f},
                            {188.8f, 222.2f, 273.3f}};
constexpr float kL[4][3] = {{1017.3f, 1023.9f, 1008.5f}, {500.6f, 527.6f, 518.5f}, {}, {}};

float glow_of(const float (*c)[3]) {
  return palette_glow_entry(c, kA, kL, 4);
}
}  // namespace

TEST(PaletteGlow, LightHeldAtNightIsGlow) {
  // ground next to a red crystal: the same colour in every slot (reference: 120.41)
  const float red[4][3] = {{208, 107, 107}, {208, 107, 107}, {208, 107, 107}, {208, 107, 107}};
  EXPECT_NEAR(glow_of(red), 120.4f, 1.0f);
  const float grey[4][3] = {{132, 122, 122}, {132, 122, 122}, {132, 122, 122}, {132, 122, 122}};
  EXPECT_NEAR(glow_of(grey), 108.3f, 1.0f);  // reference: 108.27
}

TEST(PaletteGlow, AmbientAndSunAreNotGlow) {
  float amb[4][3], lit[4][3];
  const float sun[4] = {0.08f, 0.05f, 0.f, 0.f};
  for (int s = 0; s < 4; s++) {
    for (int c = 0; c < 3; c++) {
      amb[s][c] = (float)(int)(0.3f * kA[s][c] + 0.5f);
      lit[s][c] = (float)(int)(0.3f * kA[s][c] + sun[s] * kL[s][c] + 0.5f);
    }
  }
  EXPECT_EQ(glow_of(amb), 0.f);
  EXPECT_EQ(glow_of(lit), 0.f);
}

TEST(PaletteGlow, SunlitEntryTheFitMissesIsNotGlow) {
  // village1 grass: lit by day, dark at dawn; the fit's global colours miss it by 31 u8 rms
  constexpr float A[7][3] = {{193.8f, 134.2f, 268.7f}, {205.7f, 204.4f, 239.6f},
                             {227.7f, 225.2f, 250.5f}, {203.6f, 202.8f, 239.4f},
                             {150.5f, 161.7f, 264.2f}, {133.6f, 158.3f, 244.8f},
                             {115.2f, 168.0f, 231.2f}};
  constexpr float L[7][3] = {{333.2f, 313.2f, 39.1f},  {336.2f, 326.0f, 285.7f},
                             {292.0f, 284.7f, 245.1f}, {342.2f, 328.5f, 281.5f},
                             {307.6f, 203.6f, 0.0f},   {42.3f, 52.6f, 117.0f},
                             {43.9f, 116.0f, 88.4f}};
  const float c[7][3] = {{84, 71, 46}, {184, 179, 86}, {253, 253, 126}, {207, 203, 96},
                         {87, 79, 43}, {66, 74, 60},   {62, 115, 61}};
  EXPECT_EQ(palette_glow_entry(c, A, L, 7), 0.f);
}

TEST(PaletteGlow, ClippedSamplesAreLeftOut) {
  // red clipped in every slot: the other channels decide, red counts from its dimmest slot
  const float red[4][3] = {{255, 90, 90}, {255, 90, 90}, {255, 90, 90}, {255, 90, 90}};
  EXPECT_NEAR(glow_of(red), 122.5f, 1.0f);  // reference: 122.54
  // white clipped everywhere says nothing about the slots
  const float white[4][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
  EXPECT_EQ(glow_of(white), 0.f);
}

TEST(PaletteGlow, NeedsTwoSlots) {
  const float c[1][3] = {{200, 100, 100}};
  EXPECT_EQ(palette_glow_entry(c, kA, kL, 1), 0.f);
}

TEST(PaletteGlow, SteadySunlitEntryWithSunInEverySlotIsNotGlow) {
  // village3 fitted slots (0-2 day, 3 night, all with sun): its mood rewrites the lights every
  // frame, so a sunlit entry is nearly constant over the slots; the per-slot sun still explains it
  constexpr float A[4][3] = {{113.1f, 102.9f, 151.0f},
                             {110.7f, 154.5f, 189.8f},
                             {100.5f, 109.2f, 148.9f},
                             {66.5f, 104.8f, 159.6f}};
  constexpr float L[4][3] = {{436.2f, 192.0f, 28.6f},
                             {451.9f, 157.7f, 2.3f},
                             {446.2f, 185.1f, 31.0f},
                             {478.0f, 191.1f, 21.1f}};
  const float sun[3] = {0.3f * A[0][0] + 0.15f * L[0][0], 0.3f * A[0][1] + 0.15f * L[0][1],
                        0.3f * A[0][2] + 0.15f * L[0][2]};
  const float tint[3] = {150, 120, 90};
  for (const float* c : {sun, tint}) {
    const float C[4][3] = {{c[0], c[1], c[2]}, {c[0], c[1], c[2]}, {c[0], c[1], c[2]},
                           {c[0], c[1], c[2]}};
    EXPECT_EQ(palette_glow_entry(C, A, L, 3), 0.f);  // day slots only
    EXPECT_EQ(palette_glow_entry(C, A, L, 4), 0.f);
  }
}

namespace {
// one palette quad (4 entries) of 8 slots; slot s of entry e set to rgb
void set_entry(tfrag3::PackedTimeOfDay* p, int e, int s, float r, float g, float b) {
  p->read(e, s, 0) = (u8)r;
  p->read(e, s, 1) = (u8)g;
  p->read(e, s, 2) = (u8)b;
}

UnbakeSidecar ogre_sidecar() {
  UnbakeSidecar sc;
  for (int s = 0; s < 4; s++) {
    sc.slots[s].amb = math::Vector3f(kA[s][0], kA[s][1], kA[s][2]);
    sc.slots[s].lgt = math::Vector3f(kL[s][0], kL[s][1], kL[s][2]);
  }
  sc.slot_mask = 0b111111;  // 4 and 5 are effect slots: fitted but neither day nor night
  sc.day_mask = 0b0011;
  sc.night_mask = 0b1100;
  return sc;
}
}  // namespace

TEST(PaletteGlow, ComputeUsesTheFittedDayAndNightSlots) {
  tfrag3::PackedTimeOfDay p;
  p.color_count = 4;
  p.data.assign(4 * 4 * 8, 0);
  for (int s = 0; s < 8; s++) {
    // entry 0: the red crystal ground in slots 0-3, garbage in the effect slots
    set_entry(&p, 0, s, s < 4 ? 208 : 20, s < 4 ? 107 : 240, s < 4 ? 107 : 20);
    // entry 1: ambient only
    set_entry(&p, 1, s, s < 4 ? 0.3f * kA[s][0] : 255, s < 4 ? 0.3f * kA[s][1] : 0,
              s < 4 ? 0.3f * kA[s][2] : 0);
  }
  const UnbakeSidecar sc = ogre_sidecar();
  std::vector<u8> out;
  ASSERT_TRUE(palette_glow_compute(p, sc, &out));
  ASSERT_EQ(out.size(), 4u);
  EXPECT_NEAR(out[0], 120, 1);  // as LightHeldAtNightIsGlow: slots 4-5 are left out
  EXPECT_EQ(out[1], 0);
  EXPECT_EQ(out[2], 0);  // all-zero entries
  EXPECT_EQ(out[3], 0);
}

TEST(PaletteGlow, ComputeNeedsTwoSlots) {
  tfrag3::PackedTimeOfDay p;
  p.color_count = 4;
  p.data.assign(4 * 4 * 8, 208);
  UnbakeSidecar sc = ogre_sidecar();
  sc.day_mask = 0b0001;
  sc.night_mask = 0;  // one usable slot
  std::vector<u8> out;
  EXPECT_FALSE(palette_glow_compute(p, sc, &out));
  sc.day_mask = 0b0011;
  sc.slot_mask = 0b0001;  // day slot 1 not fitted
  EXPECT_FALSE(palette_glow_compute(p, sc, &out));
}

TEST(PaletteGlow, ComputeNeedsASunlessSlot) {
  // village3: a sun in every fitted slot; a constant grey over the day slots alone would read
  // as glow (98 u8 by palette_glow_entry), so the level gets no split at all
  constexpr float A[4][3] = {{113.1f, 102.9f, 151.0f},
                             {110.7f, 154.5f, 189.8f},
                             {100.5f, 109.2f, 148.9f},
                             {66.5f, 104.8f, 159.6f}};
  constexpr float L[4][3] = {{436.2f, 192.0f, 28.6f},
                             {451.9f, 157.7f, 2.3f},
                             {446.2f, 185.1f, 31.0f},
                             {478.0f, 191.1f, 21.1f}};
  const float grey[3][3] = {{120, 120, 120}, {120, 120, 120}, {120, 120, 120}};
  EXPECT_GT(palette_glow_entry(grey, A, L, 3), 90.f);  // reference: 98.55
  UnbakeSidecar sc;
  for (int s = 0; s < 4; s++) {
    sc.slots[s].amb = math::Vector3f(A[s][0], A[s][1], A[s][2]);
    sc.slots[s].lgt = math::Vector3f(L[s][0], L[s][1], L[s][2]);
  }
  sc.slot_mask = 0b1111;
  sc.day_mask = 0b0111;
  sc.night_mask = 0;
  tfrag3::PackedTimeOfDay p;
  p.color_count = 4;
  p.data.assign(4 * 4 * 8, 120);
  std::vector<u8> out;
  EXPECT_FALSE(palette_glow_compute(p, sc, &out));  // day slots only
  sc.night_mask = 0b1000;
  EXPECT_FALSE(palette_glow_compute(p, sc, &out));  // all four
  // ogre (night slots without sun) keeps its split
  const UnbakeSidecar ogre = ogre_sidecar();
  p.data.assign(4 * 4 * 8, 0);
  for (int s = 0; s < 8; s++) {
    set_entry(&p, 0, s, 208, 107, 107);
  }
  ASSERT_TRUE(palette_glow_compute(p, ogre, &out));
  EXPECT_NEAR(out[0], 120, 1);  // reference: 120.41
}
