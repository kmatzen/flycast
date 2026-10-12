// Differential/performance test for the production Metal tile backend.
// clang++ -std=c++20 -O3 -fobjc-arc -Icore -Icore/deps/nowide/include \
//   -framework Foundation -framework Metal \
//   tests/manual/MetalTileRasterizerPrototype.mm \
//   core/rend/soft/HybridTileRenderer.mm -o /tmp/metal-tile-prototype

#include "rend/soft/HybridTileRenderer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {
using Triangle = rend::soft::hybrid::Triangle;
constexpr u32 Width = 640, Height = 480, TextureSize = 64;
constexpr s32 SubpixelScale = 16, SampleOffset = 8;

u32 channel(u32 color, u32 shift) { return (color >> shift) & 0xff; }

u32 packColor(float r, float g, float b, float a) {
  auto byte = [](float value) {
    return static_cast<u32>(std::clamp(std::lround(value), 0l, 255l));
  };
  return byte(r) | byte(g) << 8 | byte(b) << 16 | byte(a) << 24;
}

s64 edge(s32 ax, s32 ay, s32 bx, s32 by, s32 x, s32 y) {
  return static_cast<s64>(bx - ax) * (y - ay) -
         static_cast<s64>(by - ay) * (x - ax);
}

bool topLeft(s32 ax, s32 ay, s32 bx, s32 by) {
  const s64 dx = static_cast<s64>(bx) - ax;
  const s64 dy = static_cast<s64>(by) - ay;
  return dy < 0 || (dy == 0 && dx > 0);
}

u32 factor(u32 mode, u32 source, u32 destination, bool sourceTerm, u32 shift) {
  const u32 other =
      sourceTerm ? channel(destination, shift) : channel(source, shift);
  switch (mode & 7) {
  case 0:
    return 0;
  case 1:
    return 255;
  case 2:
    return other;
  case 3:
    return 255 - other;
  case 4:
    return channel(source, 24);
  case 5:
    return 255 - channel(source, 24);
  case 6:
    return channel(destination, 24);
  default:
    return 255 - channel(destination, 24);
  }
}

u32 blend(u32 source, u32 destination, u32 sourceFactor,
          u32 destinationFactor) {
  u32 result = 0;
  for (u32 shift = 0; shift < 32; shift += 8) {
    const u32 sf = factor(sourceFactor, source, destination, true, shift);
    const u32 df = factor(destinationFactor, source, destination, false, shift);
    result |= std::min(255u, (channel(source, shift) * sf +
                              channel(destination, shift) * df + 127) /
                                 255)
              << shift;
  }
  return result;
}

bool depthTest(u32 mode, float source, float destination) {
  switch (mode & 7) {
  case 0:
    return false;
  case 1:
    return source < destination;
  case 2:
    return source == destination;
  case 3:
    return source <= destination;
  case 4:
    return source > destination;
  case 5:
    return source != destination;
  case 6:
    return source >= destination;
  default:
    return true;
  }
}

u32 sampleBilinear(const std::vector<u32> &texture, float u, float v) {
  u -= std::floor(u);
  v -= std::floor(v);
  const float txf = u * TextureSize - .5f;
  const float tyf = v * TextureSize - .5f;
  const s32 x0 = static_cast<s32>(std::floor(txf));
  const s32 y0 = static_cast<s32>(std::floor(tyf));
  const u32 xs[2]{static_cast<u32>(x0) & (TextureSize - 1),
                  static_cast<u32>(x0 + 1) & (TextureSize - 1)};
  const u32 ys[2]{static_cast<u32>(y0) & (TextureSize - 1),
                  static_cast<u32>(y0 + 1) & (TextureSize - 1)};
  const u32 samples[4]{texture[ys[0] * TextureSize + xs[0]],
                       texture[ys[0] * TextureSize + xs[1]],
                       texture[ys[1] * TextureSize + xs[0]],
                       texture[ys[1] * TextureSize + xs[1]]};
  const float tx = txf - x0, ty = tyf - y0;
  float out[4];
  for (u32 component = 0; component < 4; ++component) {
    const u32 shift = component * 8;
    const float top = channel(samples[0], shift) +
                      (static_cast<float>(channel(samples[1], shift)) -
                       channel(samples[0], shift)) *
                          tx;
    const float bottom = channel(samples[2], shift) +
                         (static_cast<float>(channel(samples[3], shift)) -
                          channel(samples[2], shift)) *
                             tx;
    out[component] = top + (bottom - top) * ty;
  }
  return packColor(out[0], out[1], out[2], out[3]);
}

u32 modulate(u32 base, u32 texel) {
  float out[4];
  for (u32 component = 0; component < 4; ++component) {
    const u32 shift = component * 8;
    out[component] = channel(base, shift) * channel(texel, shift) / 255.f;
  }
  return packColor(out[0], out[1], out[2], out[3]);
}

Triangle testTriangle(u32 color, float z, u32 state) {
  Triangle triangle{{0, 32, 0}, {0, 0, 32}, {z, z, z}, {},    {},
                    {color, color, color},
                    0,             0,          0,         state, 0, 0,
                    0,             0,          0};
  if ((state & rend::soft::hybrid::Textured) != 0)
    triangle.mipLevelCount = 1;
  triangle.area1Index = std::numeric_limits<u32>::max();
  return triangle;
}

void rasterizeReference(const std::vector<Triangle> &triangles,
                        const std::vector<u32> &texture,
                        std::vector<u32> &colors, std::vector<float> &depths) {
  std::fill(colors.begin(), colors.end(), 0xff000000u);
  std::fill(depths.begin(), depths.end(), 0.f);
  for (const Triangle &t : triangles) {
    const s64 area = edge(t.x[0], t.y[0], t.x[1], t.y[1], t.x[2], t.y[2]);
    const bool tl[3]{topLeft(t.x[1], t.y[1], t.x[2], t.y[2]),
                     topLeft(t.x[2], t.y[2], t.x[0], t.y[0]),
                     topLeft(t.x[0], t.y[0], t.x[1], t.y[1])};
    for (s32 y = t.minY; y <= t.maxY; ++y)
      for (s32 x = t.minX; x <= t.maxX; ++x) {
        const s32 sx = x * SubpixelScale + SampleOffset,
                  sy = y * SubpixelScale + SampleOffset;
        const s64 w[3]{edge(t.x[1], t.y[1], t.x[2], t.y[2], sx, sy),
                       edge(t.x[2], t.y[2], t.x[0], t.y[0], sx, sy),
                       edge(t.x[0], t.y[0], t.x[1], t.y[1], sx, sy)};
        if (w[0] < 0 || (w[0] == 0 && !tl[0]) || w[1] < 0 ||
            (w[1] == 0 && !tl[1]) || w[2] < 0 || (w[2] == 0 && !tl[2]))
          continue;
        const float s[3]{static_cast<float>(w[0]) / static_cast<float>(area),
                         static_cast<float>(w[1]) / static_cast<float>(area),
                         static_cast<float>(w[2]) / static_cast<float>(area)};
        const float z = t.z[0] * s[0] + t.z[1] * s[1] + t.z[2] * s[2];
        const size_t offset = static_cast<size_t>(y) * Width + x;
        if (!depthTest(t.state, z, depths[offset]))
          continue;
        const float iz = z == 0.f ? 0.f : 1.f / z;
        auto interpolate = [&](u32 shift) {
          return (channel(t.color[0], shift) * t.z[0] * s[0] +
                  channel(t.color[1], shift) * t.z[1] * s[1] +
                  channel(t.color[2], shift) * t.z[2] * s[2]) *
                 iz;
        };
        u32 source = packColor(interpolate(0), interpolate(8), interpolate(16),
                               interpolate(24));
        const float u = (t.u[0] * t.z[0] * s[0] + t.u[1] * t.z[1] * s[1] +
                         t.u[2] * t.z[2] * s[2]) *
                        iz;
        const float v = (t.v[0] * t.z[0] * s[0] + t.v[1] * t.z[1] * s[1] +
                         t.v[2] * t.z[2] * s[2]) *
                        iz;
        source = modulate(source, sampleBilinear(texture, u, v));
        if ((t.state & rend::soft::hybrid::DepthWrite) != 0)
          depths[offset] = z;
        colors[offset] = blend(source, colors[offset], (t.state >> 16) & 7,
                               (t.state >> 19) & 7);
      }
  }
}
} // namespace

int main() {
  std::mt19937 random(0x505652u);
  std::uniform_real_distribution<float> randomDepth(.01f, 1.f);
  std::vector<u32> texture(TextureSize * TextureSize);
  for (u32 &texel : texture)
    texel = 0xff000000u | (random() & 0x00ffffffu);
  std::vector<Triangle> triangles;
  for (u32 i = 0; i < 72; ++i) {
    const s32 inset = i % 5;
    const bool translucent = i >= 26;
    const u32 alpha = translucent ? 32 + random() % 192 : 255;
    auto color = [&] { return alpha << 24 | (random() & 0x00ffffffu); };
    triangles.push_back(
        {{inset * 16, (static_cast<s32>(Width) - 1 - inset) * 16, inset * 16},
         {inset * 16, inset * 16, (static_cast<s32>(Height) - 1 - inset) * 16},
         {randomDepth(random), randomDepth(random), randomDepth(random)},
         {0.f, 4.f, 0.f},
         {0.f, 0.f, 4.f},
         {color(), color(), color()},
         0,
         TextureSize,
         TextureSize,
         rend::soft::hybrid::makeState(6, !translucent, true, true, true, 3,
                                       translucent ? 4 : 1,
                                       translucent ? 5 : 0),
         0,
         inset,
         inset,
         static_cast<s32>(Width) - 1 - inset,
         static_cast<s32>(Height) - 1 - inset});
    triangles.back().mipLevelCount = 1;
    triangles.back().area1Index = std::numeric_limits<u32>::max();
  }
  std::vector<u32> expectedColors(Width * Height), actualColors;
  std::vector<float> expectedDepths(Width * Height), actualDepths;
  const auto cpuStart = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i)
    rasterizeReference(triangles, texture, expectedColors, expectedDepths);
  const auto cpuEnd = std::chrono::steady_clock::now();
  const rend::soft::hybrid::Frame frame{Width, Height, &triangles, &texture};
  if (!rend::soft::hybrid::render(frame, actualColors, actualDepths))
    return 2;
  rend::soft::hybrid::Timing timing;
  if (!rend::soft::hybrid::render(frame, actualColors, actualDepths, &timing))
    return 2;
  size_t colorMismatches = 0, depthMismatches = 0;
  for (size_t i = 0; i < expectedColors.size(); ++i) {
    colorMismatches += actualColors[i] != expectedColors[i];
    depthMismatches +=
        std::memcmp(&actualDepths[i], &expectedDepths[i], sizeof(float)) != 0;
  }

  const u32 source = 0xc0a06020u, destination = 0x80402010u;
  for (u32 sf = 0; sf < 8; ++sf)
    for (u32 df = 0; df < 8; ++df) {
      std::vector<Triangle> commands{
          testTriangle(destination, .5f,
                       rend::soft::hybrid::makeState(7, true, false, false,
                                                     true, 0, 1, 0)),
          testTriangle(source, .75f,
                       rend::soft::hybrid::makeState(7, false, false, false,
                                                     true, 0, sf, df))};
      const rend::soft::hybrid::Frame test{1, 1, &commands, &texture};
      if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
          actualColors[0] != blend(source, destination, sf, df))
        return 1;
    }
  for (u32 mode = 0; mode < 8; ++mode)
    for (float z : {.25f, .5f, .75f}) {
      std::vector<Triangle> commands{
          testTriangle(destination, .5f,
                       rend::soft::hybrid::makeState(7, true, false, false,
                                                     true, 0, 1, 0)),
          testTriangle(source, z,
                       rend::soft::hybrid::makeState(mode, false, false, false,
                                                     true, 0, 1, 0))};
      const rend::soft::hybrid::Frame test{1, 1, &commands, &texture};
      if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
          actualColors[0] != (depthTest(mode, z, .5f) ? source : destination))
        return 1;
    }

  {
    std::vector<Triangle> commands{testTriangle(
        source, .25f,
        rend::soft::hybrid::makeState(4, true, false, false, true, 0, 1, 0))};
    const std::vector<u32> initialColors{destination};
    const std::vector<float> initialDepths{.5f};
    rend::soft::hybrid::Frame test{1, 1, &commands, &texture, &initialColors,
                                   &initialDepths};
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != destination || actualDepths[0] != .5f)
      return 1;
  }

  {
    std::vector<Triangle> commands{testTriangle(
        source, .75f,
        rend::soft::hybrid::makeState(7, true, false, false, true, 0, 1, 0))};
    const std::vector<u32> initialColors{destination};
    const std::vector<float> initialDepths{.5f};
    const std::vector<u8> disabledList{0};
    const std::vector<u8> clearDepth{1};
    rend::soft::hybrid::Frame test{1, 1, &commands, &texture, &initialColors,
                                   &initialDepths};
    test.depthClearMask = &clearDepth;
    test.opaqueMask = &disabledList;
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != destination || actualDepths[0] != 0.f)
      return 1;
  }

  {
    const u32 colors[]{0x80602010u, 0x804080c0u, 0x80c04020u};
    const float depths[]{.8f, .2f, .5f};
    std::vector<Triangle> commands;
    for (u32 i = 0; i < 3; ++i) {
      Triangle command = testTriangle(
          colors[i], depths[i],
          rend::soft::hybrid::makeState(6, false, false, false, true, 0, 4,
                                        5) |
              rend::soft::hybrid::Autosort);
      commands.push_back(command);
    }
    u32 expected = 0xff000000u;
    expected = blend(colors[1], expected, 4, 5);
    expected = blend(colors[2], expected, 4, 5);
    expected = blend(colors[0], expected, 4, 5);
    const rend::soft::hybrid::Frame test{1, 1, &commands, &texture};
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != expected || actualDepths[0] != 0.f)
      return 1;
  }

  {
    std::array<u32, 128> fogTable{};
    reinterpret_cast<u8 *>(fogTable.data())[0] = 10;
    reinterpret_cast<u8 *>(fogTable.data())[1] = 20;
    const u32 input = 0xff204060u;
    const u32 fogColor = 0xffe0c0a0u;
    std::vector<Triangle> commands{testTriangle(
        input, 1.f,
        rend::soft::hybrid::makeState(7, false, false, false, true, 0, 1, 0) |
            rend::soft::hybrid::LookupFog)};
    rend::soft::hybrid::Frame test{1, 1, &commands, &texture};
    test.fogTable = fogTable.data();
    test.fogColor = fogColor;
    test.fogDensity = 1.f;
    const float fog = 20.f / 255.f;
    const u32 expected = packColor(
        channel(input, 0) + (channel(fogColor, 0) - channel(input, 0)) * fog,
        channel(input, 8) + (channel(fogColor, 8) - channel(input, 8)) * fog,
        channel(input, 16) + (channel(fogColor, 16) - channel(input, 16)) * fog,
        channel(input, 24));
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != expected)
      return 1;
  }

  {
    Triangle command = testTriangle(
        0xff3264c8u, 1.f,
        rend::soft::hybrid::makeState(7, false, false, false, true, 0, 1, 0) |
            rend::soft::hybrid::IntensityShadow);
    command.shadeScale = 128;
    std::vector<Triangle> commands{command};
    const std::vector<u8> modifierMask{1};
    rend::soft::hybrid::Frame test{1, 1, &commands, &texture};
    test.modifierMask = &modifierMask;
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != 0xff193264u)
      return 1;
  }

  {
    Triangle area0 = testTriangle(
        0xff0000ffu, 1.f,
        rend::soft::hybrid::makeState(7, false, false, false, true, 0, 1, 0));
    Triangle area1 = testTriangle(
        0xff00ff00u, 1.f,
        rend::soft::hybrid::makeState(7, false, false, false, true, 0, 1, 0) |
            rend::soft::hybrid::Area1Only);
    area0.area1Index = 1;
    std::vector<Triangle> commands{area0, area1};
    std::vector<u8> modifierMask{1};
    rend::soft::hybrid::Frame test{1, 1, &commands, &texture};
    test.modifierMask = &modifierMask;
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != 0xff00ff00u)
      return 1;
    modifierMask[0] = 0;
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != 0xff0000ffu)
      return 1;
  }

  {
    const std::vector<u32> superTexture{
        0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu};
    Triangle command = testTriangle(
        0xffffffffu, 1.f,
        rend::soft::hybrid::makeState(7, false, true, false, true, 0, 1, 0) |
            rend::soft::hybrid::TextureSuperSample);
    command.textureWidth = command.textureHeight = 2;
    command.u[0] = command.v[0] = -.25f;
    command.u[1] = 1.75f;
    command.v[1] = -.25f;
    command.u[2] = -.25f;
    command.v[2] = 1.75f;
    command.superSampleOffsetX = command.superSampleOffsetY = 8;
    std::vector<Triangle> commands{command};
    const rend::soft::hybrid::Frame test{1, 1, &commands, &superTexture};
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != 0xff808080u)
      return 1;
  }

  {
    const std::vector<u32> whiteTexture{0xffffffffu};
    Triangle command = testTriangle(
        0xff000010u, 1.f,
        rend::soft::hybrid::makeState(7, false, true, false, true, 3, 1, 0));
    command.textureWidth = command.textureHeight = 1;
    command.offsetColor[0] = command.offsetColor[1] = command.offsetColor[2] =
        0x80000020u;
    command.effects = rend::soft::hybrid::UseOffsetColor |
                      rend::soft::hybrid::VertexFog;
    std::vector<Triangle> commands{command};
    rend::soft::hybrid::Frame test{1, 1, &commands, &whiteTexture};
    test.fogVertexColor = 0xff00ff00u;
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != 0xff008018u)
      return 1;
  }

  {
    const std::vector<u32> bumpTexture{0x88000000u};
    Triangle command = testTriangle(
        0xffffffffu, 1.f,
        rend::soft::hybrid::makeState(7, false, true, false, true, 0, 1, 0));
    command.textureWidth = command.textureHeight = 1;
    command.offsetColor[0] = command.offsetColor[1] = command.offsetColor[2] =
        0x000000ffu;
    command.effects = rend::soft::hybrid::UseOffsetColor |
                      rend::soft::hybrid::BumpMap;
    std::vector<Triangle> commands{command};
    const rend::soft::hybrid::Frame test{1, 1, &commands, &bumpTexture};
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != 0xb4ffffffu)
      return 1;
  }

  const std::vector<u32> atlas{0xff0000ffu, 0xff0000ffu, 0xff0000ffu,
                               0xff0000ffu, 0xff00ff00u, 0xff00ff00u,
                               0xff00ff00u, 0xff00ff00u};
  for (u32 index = 0; index < 2; ++index) {
    Triangle command = testTriangle(
        0xffffffffu, .5f,
        rend::soft::hybrid::makeState(7, false, true, false, true, 0, 1, 0));
    command.textureOffset = index * 4;
    command.textureWidth = command.textureHeight = 2;
    std::vector<Triangle> commands{command};
    const rend::soft::hybrid::Frame test{1, 1, &commands, &atlas};
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != (index == 0 ? 0xff0000ffu : 0xff00ff00u))
      return 1;
  }

  {
    const std::vector<u32> mipAtlas(21);
    std::vector<u32> levels = mipAtlas;
    std::fill(levels.begin(), levels.begin() + 16, 0xff0000ffu);
    std::fill(levels.begin() + 16, levels.begin() + 20, 0xff00ff00u);
    levels[20] = 0xffff0000u;
    Triangle command = testTriangle(
        0xffffffffu, .5f,
        rend::soft::hybrid::makeState(7, false, true, false, true, 0, 1, 0));
    command.textureWidth = command.textureHeight = 4;
    command.mipOffsets[0] = 0;
    command.mipOffsets[1] = 16;
    command.mipOffsets[2] = 20;
    command.mipLevelCount = 3;
    command.mipNumerator = .75f;
    std::vector<Triangle> commands{command};
    const rend::soft::hybrid::Frame test{1, 1, &commands, &levels};
    if (!rend::soft::hybrid::render(test, actualColors, actualDepths) ||
        actualColors[0] != 0xffff0000u)
      return 1;

    for (u32 pass = 0; pass < 2; ++pass) {
      command.state |= rend::soft::hybrid::Trilinear;
      if (pass != 0)
        command.state |= rend::soft::hybrid::TrilinearPassB;
      command.mipNumerator = .5625f;
      commands = {command};
      const rend::soft::hybrid::Frame trilinear{1, 1, &commands, &levels};
      const u32 expected = pass == 0 ? 0xbf00bf00u : 0x40400000u;
      if (!rend::soft::hybrid::render(trilinear, actualColors, actualDepths) ||
          actualColors[0] != expected)
        return 1;
    }
  }

  std::vector<Triangle> invalid = triangles;
  invalid[0].x[2] = invalid[0].x[1];
  invalid[0].y[2] = invalid[0].y[1];
  const rend::soft::hybrid::Frame invalidFrame{Width, Height, &invalid,
                                               &texture};
  actualColors = {0x12345678u};
  actualDepths = {.25f};
  if (rend::soft::hybrid::render(invalidFrame, actualColors, actualDepths) ||
      actualColors != std::vector<u32>{0x12345678u} ||
      actualDepths != std::vector<float>{.25f})
    return 2;

  const double cpuMs =
      std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count() /
      20.;
  std::printf(
      "triangles=%zu cpu_ms=%.3f encode_ms=%.3f gpu_ms=%.3f readback_ms=%.3f "
      "color_mismatches=%zu depth_bit_mismatches=%zu\n",
      triangles.size(), cpuMs, timing.encodeMilliseconds,
      timing.gpuMilliseconds, timing.readbackMilliseconds, colorMismatches,
      depthMismatches);
  return colorMismatches == 0 && depthMismatches == 0 ? 0 : 1;
}
