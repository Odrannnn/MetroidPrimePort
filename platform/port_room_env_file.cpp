// The .roomenv file: parsing, and picking the probe for a point. See port_room_env.h.
#include "port_room_env.h"
#include "port_hash.h"
#include "port_strings.h"
#include "port_bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace PortRoomEnv {
namespace {

constexpr uint32_t kMagic = 0x5645504D; // 'MPEV'
constexpr uint32_t kVersion = 27;
constexpr uint32_t kMaxGrades = 64;
// A fog record up to its link count (inclusive).
constexpr size_t kFogBytes = 368;
// A fog region record up to its link count (inclusive).
constexpr size_t kFogRegionBytes = 136;
constexpr uint32_t kMaxFogRegions = 1024;
// A fog transition record up to its spline's size (inclusive).
constexpr size_t kFogTransitionBytes = 44;
constexpr uint32_t kMaxGradeLinks = 256;
constexpr size_t kHeaderSize = 32;
constexpr size_t kProbeSizeV1 = 100;
constexpr size_t kProbeSize = 112;
constexpr size_t kCubeHeaderSize = 16;
constexpr uint32_t kMaxProbes = 4096;
constexpr uint32_t kMaxCubeSize = 1024;
constexpr uint32_t kMaxLightmapSize = 8192;
constexpr size_t kGridHeaderSize = 60;
constexpr size_t kPointSize = 24;
constexpr uint32_t kMaxGrids = 64;
constexpr uint32_t kMaxGridSize = 1024;

using port::ReadLE32;
using port::ReadLEFloat;

float GetHalf(const uint8_t* p) {
  const uint32_t h = uint32_t(p[0]) | (uint32_t(p[1]) << 8);
  const int exponent = int((h >> 10) & 0x1F);
  const int mantissa = int(h & 0x3FF);
  // An infinity is clamped; a NaN or a negative is no light.
  if ((h & 0x8000) != 0 || (exponent == 31 && mantissa != 0)) {
    return 0.f;
  }
  if (exponent == 31) {
    return 65504.f;
  }
  return exponent == 0 ? std::ldexp(float(mantissa), -24) : std::ldexp(float(mantissa | 0x400), exponent - 25);
}

// A LUT's id: 0 for the identity (each texel its own coordinate, give or take the rounding
// of 32 steps onto 255), else a hash of its texels.
uint32_t GradeLutId(const uint8_t* lut) {
  constexpr uint32_t n = kGradeLutSize;
  bool identity = true;
  uint32_t hash = port::kFnv32Basis;
  for (uint32_t i = 0; i < n * n * n; ++i) {
    const uint32_t coord[3] = {i % n, i / n % n, i / (n * n)};
    for (int c = 0; c < 4; ++c) {
      const uint8_t v = lut[i * 4 + c];
      hash = port::Fnv1a32Step(hash, v);
      if (c < 3 && std::fabs(float(v) - float(coord[c]) * (255.f / float(n - 1))) > 4.f) {
        identity = false;
      }
    }
  }
  return identity ? 0u : (hash != 0 ? hash : 1u);
}

using port::HexDigit;

constexpr uint32_t kMaxTiles = 1u << 16;
constexpr size_t kTileHeaderSize = 12 * 4 + 6 * 4 + 8;
constexpr size_t kTileRecordSize = 44;

// Reads a version 21 tile set at `at`, checking every record against the data it points into.
bool ParseTiles(const std::vector<uint8_t>& data, size_t& at, PortRoomEnvTiles::TileSet& set, std::string& error) {
  using namespace PortRoomEnvTiles;
  if (data.size() - at < kTileHeaderSize) {
    error = "cut short";
    return false;
  }
  const uint8_t* p = data.data() + at;
  for (int i = 0; i < 12; ++i) {
    set.worldToPoint[i] = ReadLEFloat(p + i * 4);
    if (!std::isfinite(set.worldToPoint[i])) {
      error = "a tile set is not finite";
      return false;
    }
  }
  for (int i = 0; i < 6; ++i) {
    set.box[i] = int32_t(ReadLE32(p + 48 + i * 4));
  }
  // A box of lit points: ordered, and no wider than 4096 points (a room spans a few hundred).
  for (int a = 0; a < 3; ++a) {
    const int64_t lo = set.box[a], hi = set.box[a + 3];
    if (hi < lo || hi - lo > 4096 || lo < -(int64_t(1) << 24) || hi > (int64_t(1) << 24)) {
      error = "bad tile box";
      return false;
    }
  }
  const uint32_t count = ReadLE32(p + 72);
  const uint32_t bytes = ReadLE32(p + 76);
  at += kTileHeaderSize;
  if (count > kMaxTiles || (data.size() - at) / kTileRecordSize < count) {
    error = "cut short";
    return false;
  }
  const uint8_t* records = data.data() + at;
  at += size_t(count) * kTileRecordSize;
  if (data.size() - at < bytes) {
    error = "cut short";
    return false;
  }
  set.tiles.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* r = records + size_t(i) * kTileRecordSize;
    Tile& tile = set.tiles[i];
    tile.key = {int32_t(ReadLE32(r)), int32_t(ReadLE32(r + 4)), int32_t(ReadLE32(r + 8)), int32_t(ReadLE32(r + 12))};
    for (int a = 0; a < 3; ++a) {
      tile.lo[a] = int32_t(ReadLE32(r + 16 + a * 4));
      tile.size[a] = int32_t(ReadLE32(r + 28 + a * 4));
    }
    tile.offset = ReadLE32(r + 40);
    if (tile.key.layer < 0 || tile.key.layer >= kLayers || (i != 0 && !(set.tiles[i - 1].key < tile.key))) {
      error = "bad tile key";
      return false;
    }
    const int32_t extent[3] = {TileWidth(tile.key.layer), 64, 16};
    const bool empty = tile.size[0] == 0 && tile.size[1] == 0 && tile.size[2] == 0;
    if (!empty) {
      for (int a = 0; a < 3; ++a) {
        if (tile.size[a] <= 0 || tile.lo[a] < 0 || tile.lo[a] > extent[a] - tile.size[a]) {
          error = "bad tile crop";
          return false;
        }
      }
      const size_t tileBytes = size_t(tile.size[0]) * size_t(tile.size[1]) * size_t(tile.size[2]) *
                               size_t(TileBytes(tile.key.layer));
      if (tile.offset > bytes || bytes - tile.offset < tileBytes) {
        error = "bad tile data";
        return false;
      }
    }
  }
  set.data.assign(data.begin() + ptrdiff_t(at), data.begin() + ptrdiff_t(at + bytes));
  at += bytes;
  return true;
}

} // namespace

bool ParseTileSet(const std::vector<uint8_t>& data, size_t& at, PortRoomEnvTiles::TileSet& set, std::string& error) {
  return ParseTiles(data, at, set, error);
}

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  return port::ParseHexFileName(fileName, ".roomenv", id);
}

size_t CubeBytes(uint32_t size, uint32_t mipCount) {
  size_t bytes = 0;
  for (uint32_t mip = 0; mip < mipCount; ++mip) {
    const size_t edge = size >> mip > 0 ? size >> mip : 1;
    const size_t blocks = (edge + 3) / 4;
    bytes += blocks * blocks * 16 * 6;
  }
  return bytes;
}

bool ValidBrdfLut(const std::vector<uint8_t>& data, std::string& error) {
  if (data.size() != kBrdfLutSize) {
    error = "brdf.lut is " + std::to_string(data.size()) + " bytes, expected " + std::to_string(kBrdfLutSize);
    return false;
  }
  return true;
}

bool Parse(std::vector<uint8_t>&& data, File& out, std::string& error) {
  out = {};
  if (data.size() < kHeaderSize || ReadLE32(data.data()) != kMagic) {
    error = "not a room environment";
    return false;
  }
  const uint32_t version = ReadLE32(data.data() + 4);
  if (version == 0 || version > kVersion) {
    error = "unknown version " + std::to_string(ReadLE32(data.data() + 4));
    return false;
  }
  out.version = version;
  for (int i = 0; i < 4; ++i) {
    out.tonemap[i] = ReadLEFloat(data.data() + 8 + i * 4);
  }
  const uint32_t probes = ReadLE32(data.data() + 24);
  const uint32_t cubes = ReadLE32(data.data() + 28);
  const size_t probeSize = version >= 8 ? kProbeSize : kProbeSizeV1;
  if (probes > kMaxProbes || cubes > kMaxProbes || data.size() - kHeaderSize < size_t(probes) * probeSize) {
    error = "cut short";
    return false;
  }
  size_t at = kHeaderSize;
  out.probes.resize(probes);
  for (Probe& probe : out.probes) {
    const uint8_t* p = data.data() + at;
    for (int i = 0; i < 12; ++i) {
      probe.worldToBox[i] = ReadLEFloat(p + i * 4);
    }
    for (int i = 0; i < 9; ++i) {
      probe.worldToCube[i] = ReadLEFloat(p + 48 + i * 4);
    }
    probe.layer = version >= 9 ? int32_t(ReadLE32(p + 84)) : -1;
    probe.cube = ReadLE32(p + 88);
    probe.scale = ReadLEFloat(p + 92);
    if (version >= 8) {
      probe.padding = ReadLEFloat(p + 96);
      probe.priority = int32_t(ReadLE32(p + 100));
      probe.intensityMin = ReadLEFloat(p + 104);
      probe.intensityMax = ReadLEFloat(p + 108);
      if (!std::isfinite(probe.padding)) {
        probe.padding = 1.f;
      }
      if (!std::isfinite(probe.intensityMin) || !std::isfinite(probe.intensityMax)) {
        probe.intensityMin = 0.f;
        probe.intensityMax = 1.f;
      }
    }
    if (!std::isfinite(probe.scale)) {
      probe.scale = 1.f;
    }
    SetExtents(probe);
    if (probe.cube >= cubes && probe.cube != kProbeNoCube) {
      error = "a probe names a cube the file does not have";
      return false;
    }
    for (int i = 0; i < 21; ++i) {
      if (!std::isfinite(i < 12 ? probe.worldToBox[i] : probe.worldToCube[i - 12])) {
        error = "a probe is not finite";
        return false;
      }
    }
    at += probeSize;
  }
  out.cubes.resize(cubes);
  for (Cube& cube : out.cubes) {
    if (data.size() - at < kCubeHeaderSize) {
      error = "cut short";
      return false;
    }
    const uint8_t* p = data.data() + at;
    cube.size = ReadLE32(p);
    cube.mipCount = ReadLE32(p + 4);
    cube.isSigned = ReadLE32(p + 8) != 0;
    cube.length = ReadLE32(p + 12);
    cube.offset = at + kCubeHeaderSize;
    if (cube.size == 0 || cube.size > kMaxCubeSize || (cube.size & (cube.size - 1)) != 0 || cube.mipCount == 0 ||
        cube.mipCount > 11 || (cube.size >> (cube.mipCount - 1)) == 0) {
      error = "bad cube size";
      return false;
    }
    if (cube.length != CubeBytes(cube.size, cube.mipCount) || data.size() - cube.offset < cube.length) {
      error = "cut short";
      return false;
    }
    at = cube.offset + cube.length;
  }
  if (version >= 21) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t hasTiles = ReadLE32(data.data() + at);
    at += 4;
    if (hasTiles > 1) {
      error = "bad tile flag";
      return false;
    }
    if (hasTiles != 0) {
      auto set = std::make_shared<PortRoomEnvTiles::TileSet>();
      if (!ParseTiles(data, at, *set, error)) {
        return false;
      }
      out.tiles = std::move(set);
    }
  } else if (version >= 2) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t grids = ReadLE32(data.data() + at);
    at += 4;
    if (grids > kMaxGrids) {
      error = "too many grids";
      return false;
    }
    out.grids.resize(grids);
    for (Grid& grid : out.grids) {
      if (data.size() - at < kGridHeaderSize) {
        error = "cut short";
        return false;
      }
      const uint8_t* p = data.data() + at;
      for (int i = 0; i < 12; ++i) {
        grid.worldToGrid[i] = ReadLEFloat(p + i * 4);
        if (!std::isfinite(grid.worldToGrid[i])) {
          error = "a grid is not finite";
          return false;
        }
      }
      size_t points = 1;
      for (int i = 0; i < 3; ++i) {
        grid.size[i] = ReadLE32(p + 48 + i * 4);
        if (grid.size[i] == 0 || grid.size[i] > kMaxGridSize) {
          error = "bad grid size";
          return false;
        }
        points *= grid.size[i];
      }
      grid.offset = at + kGridHeaderSize;
      if ((data.size() - grid.offset) / kPointSize < points) {
        error = "cut short";
        return false;
      }
      double sum = 0.0;
      size_t filled = 0;
      // A sample of the points is enough for an average, and a big room has millions.
      const size_t step = points / 32768 + 1;
      for (size_t i = 0; i < points; i += step) {
        const uint8_t* point = data.data() + grid.offset + i * kPointSize;
        const double luminance = 0.2126 * GetHalf(point) + 0.7152 * GetHalf(point + 2) + 0.0722 * GetHalf(point + 4);
        if (luminance > 0.0) {
          sum += std::log(luminance);
          ++filled;
        }
      }
      // The geometric mean: a room's points span five decades, and the arithmetic mean is
      // only its few brightest.
      grid.average = filled != 0 ? float(std::exp(sum / double(filled))) : 0.f;
      at = grid.offset + points * kPointSize;
    }
  }
  if (version >= 3) {
    if (data.size() - at < 8) {
      error = "cut short";
      return false;
    }
    for (int i = 0; i < 2; ++i) {
      out.exposure[i] = ReadLEFloat(data.data() + at + i * 4);
    }
    if (!std::isfinite(out.exposure[0]) || !std::isfinite(out.exposure[1]) || out.exposure[1] < out.exposure[0]) {
      out.exposure[0] = out.exposure[1] = 0.f;
    }
    at += 8;
  }
  if (version >= 4) {
    if (data.size() - at < 8) {
      error = "cut short";
      return false;
    }
    out.exposureBias = ReadLEFloat(data.data() + at);
    out.contrast = ReadLEFloat(data.data() + at + 4);
    if (!std::isfinite(out.exposureBias)) {
      out.exposureBias = 0.f;
    }
    if (!(out.contrast >= 0.f && out.contrast <= 1.f)) {
      out.contrast = 0.f;
    }
    at += 8;
  }
  if (version >= 5) {
    if (data.size() - at < 8) {
      error = "cut short";
      return false;
    }
    out.bloomThreshold = ReadLEFloat(data.data() + at);
    const uint32_t tints = ReadLE32(data.data() + at + 4);
    at += 8;
    if (tints > 16 || (data.size() - at) / 16 < tints) {
      error = "cut short";
      return false;
    }
    out.bloomTints.resize(size_t(tints) * 4);
    for (uint32_t i = 0; i < tints * 4; ++i) {
      out.bloomTints[i] = ReadLEFloat(data.data() + at + i * 4);
      if (!std::isfinite(out.bloomTints[i])) {
        out.bloomTints[i] = 0.f;
      }
    }
    at += size_t(tints) * 16;
    if (!std::isfinite(out.bloomThreshold)) {
      out.bloomThreshold = 0.9f;
    }
  }
  if (version >= 6) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t grades = ReadLE32(data.data() + at);
    at += 4;
    if (grades > kMaxGrades) {
      error = "too many grades";
      return false;
    }
    out.grades.resize(grades);
    for (Grade& grade : out.grades) {
      if ((data.size() - at) < 12 + kGradeLutBytes) {
        error = "cut short";
        return false;
      }
      const uint8_t* p = data.data() + at;
      grade.layer = int32_t(ReadLE32(p));
      grade.fadeIn = ReadLEFloat(p + 4);
      grade.fadeOut = ReadLEFloat(p + 8);
      if (!(grade.fadeIn >= 0.f && grade.fadeIn < 600.f)) {
        grade.fadeIn = 0.f;
      }
      if (!(grade.fadeOut >= 0.f && grade.fadeOut < 600.f)) {
        grade.fadeOut = 0.f;
      }
      at += 12;
      if (version >= 10) {
        if (data.size() - at < 12) {
          error = "cut short";
          return false;
        }
        const uint8_t* q = data.data() + at;
        grade.on = q[0] != 0;
        grade.master = q[1] != 0;
        grade.priority = int32_t(ReadLE32(q + 4));
        const uint32_t links = ReadLE32(q + 8);
        at += 12;
        if (links > kMaxGradeLinks || (data.size() - at) / 8 < links) {
          error = "cut short";
          return false;
        }
        grade.links.resize(links);
        for (GradeLink& link : grade.links) {
          q = data.data() + at;
          link.sender = ReadLE32(q);
          link.state = q[4];
          link.action = q[5];
          at += 8;
        }
      }
      if (data.size() - at < kGradeLutBytes) {
        error = "cut short";
        return false;
      }
      grade.offset = at;
      grade.id = GradeLutId(data.data() + grade.offset);
      at = grade.offset + kGradeLutBytes;
    }
  }
  if (version >= 7) {
    if (data.size() - at < 8) {
      error = "cut short";
      return false;
    }
    out.exposureSigma = ReadLEFloat(data.data() + at);
    out.staticLerp = ReadLEFloat(data.data() + at + 4);
    if (!(out.exposureSigma >= 0.f && out.exposureSigma < 10000.f)) {
      out.exposureSigma = 32.f;
    }
    if (!(out.staticLerp >= 0.f && out.staticLerp <= 1.f)) {
      out.staticLerp = 0.5f;
    }
    at += 8;
  }
  if (version >= 11) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t lights = ReadLE32(data.data() + at);
    at += 4;
    if (lights > kMaxGrades) {
      error = "too many backlights";
      return false;
    }
    out.backlights.resize(lights);
    for (BacklightHint& light : out.backlights) {
      if (data.size() - at < 32) {
        error = "cut short";
        return false;
      }
      const uint8_t* p = data.data() + at;
      light.layer = int32_t(ReadLE32(p));
      light.fadeIn = ReadLEFloat(p + 4);
      light.fadeOut = ReadLEFloat(p + 8);
      light.on = p[12] != 0;
      light.master = p[13] != 0;
      light.priority = int32_t(ReadLE32(p + 16));
      light.top = ReadLEFloat(p + 20);
      light.back = ReadLEFloat(p + 24);
      const uint32_t links = ReadLE32(p + 28);
      at += 32;
      if (!(light.fadeIn >= 0.f && light.fadeIn < 600.f)) {
        light.fadeIn = 0.f;
      }
      if (!(light.fadeOut >= 0.f && light.fadeOut < 600.f)) {
        light.fadeOut = 0.f;
      }
      if (!std::isfinite(light.top)) {
        light.top = 1.f;
      }
      if (!std::isfinite(light.back)) {
        light.back = 1.f;
      }
      if (links > kMaxGradeLinks || (data.size() - at) / 8 < links) {
        error = "cut short";
        return false;
      }
      light.links.resize(links);
      for (GradeLink& link : light.links) {
        const uint8_t* q = data.data() + at;
        link.sender = ReadLE32(q);
        link.state = q[4];
        link.action = q[5];
        at += 8;
      }
    }
  }
  if (version >= 12) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t fogs = ReadLE32(data.data() + at);
    at += 4;
    if (fogs > kMaxGrades) {
      error = "too many fogs";
      return false;
    }
    out.fogs.resize(fogs);
    for (FogHint& fog : out.fogs) {
      if (data.size() - at < kFogBytes) {
        error = "cut short";
        return false;
      }
      const uint8_t* p = data.data() + at;
      fog.layer = int32_t(ReadLE32(p));
      fog.fadeIn = ReadLEFloat(p + 4);
      fog.fadeOut = ReadLEFloat(p + 8);
      fog.on = p[12] != 0;
      fog.master = p[13] != 0;
      fog.priority = int32_t(ReadLE32(p + 16));
      float* scalars[10] = {&fog.range,    &fog.scatter,    &fog.absorb,       &fog.m1z,         &fog.decay,
                            &fog.attenSlope, &fog.attenBias, &fog.noiseFreq, &fog.noiseStrength, &fog.lightCap};
      for (int i = 0; i < 10; ++i) {
        *scalars[i] = ReadLEFloat(p + 20 + 4 * i);
      }
      for (int i = 0; i < 3; ++i) {
        fog.wind[i] = ReadLEFloat(p + 60 + 4 * i);
      }
      fog.useScriptWind = p[72] != 0;
      fog.noProbe = p[73] != 0;
      for (int i = 0; i < 4; ++i) {
        fog.colorB[i] = ReadLEFloat(p + 76 + 4 * i);
        fog.colorA[i] = ReadLEFloat(p + 92 + 4 * i);
      }
      for (int i = 0; i < 64; ++i) {
        fog.lut[i] = ReadLEFloat(p + 108 + 4 * i);
      }
      const uint32_t links = ReadLE32(p + 364);
      at += kFogBytes;
      if (!(fog.fadeIn >= 0.f && fog.fadeIn < 600.f)) {
        fog.fadeIn = 0.f;
      }
      if (!(fog.fadeOut >= 0.f && fog.fadeOut < 600.f)) {
        fog.fadeOut = 0.f;
      }
      // Anything that is not a number would poison the blend; the fog then has no density.
      bool finite = true;
      for (const float* v : scalars) {
        finite = finite && std::isfinite(*v);
      }
      for (int i = 0; i < 3; ++i) {
        finite = finite && std::isfinite(fog.wind[i]);
      }
      for (int i = 0; i < 4; ++i) {
        finite = finite && std::isfinite(fog.colorA[i]) && std::isfinite(fog.colorB[i]);
      }
      for (int i = 0; i < 64; ++i) {
        finite = finite && std::isfinite(fog.lut[i]);
      }
      if (!finite || !(fog.range > 0.f)) {
        fog.decay = 0.f;
        fog.range = std::isfinite(fog.range) && fog.range > 0.f ? fog.range : 250.f;
        for (int i = 0; i < 3; ++i) {
          fog.wind[i] = std::isfinite(fog.wind[i]) ? fog.wind[i] : 0.f;
        }
        if (!finite) {
          for (float* v : scalars) {
            *v = std::isfinite(*v) ? *v : 0.f;
          }
          for (int i = 0; i < 4; ++i) {
            fog.colorA[i] = std::isfinite(fog.colorA[i]) ? fog.colorA[i] : 0.f;
            fog.colorB[i] = std::isfinite(fog.colorB[i]) ? fog.colorB[i] : 0.f;
          }
          for (float& v : fog.lut) {
            v = std::isfinite(v) ? v : 0.f;
          }
        }
      }
      if (links > kMaxGradeLinks || (data.size() - at) / 8 < links) {
        error = "cut short";
        return false;
      }
      fog.links.resize(links);
      for (GradeLink& link : fog.links) {
        const uint8_t* q = data.data() + at;
        link.sender = ReadLE32(q);
        link.state = q[4];
        link.action = q[5];
        at += 8;
      }
      fog.linearFade = version < 13;
      if (version >= 13) {
        for (int k = 0; k < 2; ++k) {
          if (data.size() - at < 4) {
            error = "cut short";
            return false;
          }
          const uint32_t size = ReadLE32(data.data() + at);
          at += 4;
          const size_t padded = (size_t(size) + 3) & ~size_t(3);
          if (data.size() - at < padded) {
            error = "cut short";
            return false;
          }
          if (size != 0) {
            PortMayaSpline& spline = k == 0 ? fog.fadeInSpline : fog.fadeOutSpline;
            if (!spline.Load(data.data() + at, size)) {
              error = "bad fog fade spline";
              return false;
            }
            (k == 0 ? fog.hasFadeInSpline : fog.hasFadeOutSpline) = true;
          }
          at += padded;
        }
      }
    }
  }
  if (version >= 14) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t regions = ReadLE32(data.data() + at);
    at += 4;
    if (regions > kMaxFogRegions) {
      error = "too many fog regions";
      return false;
    }
    out.regions.resize(regions);
    for (FogRegion& region : out.regions) {
      if (data.size() - at < kFogRegionBytes) {
        error = "cut short";
        return false;
      }
      const uint8_t* p = data.data() + at;
      region.layer = int32_t(ReadLE32(p));
      region.on = p[4] != 0;
      region.fluid = p[5] <= 2 ? p[5] : 0;
      region.hasColor = p[6] != 0;
      region.hasCap = p[7] != 0;
      for (int i = 0; i < 12; ++i) {
        region.m[i] = ReadLEFloat(p + 8 + 4 * i);
      }
      for (int i = 0; i < 3; ++i) {
        region.edgeScale[i] = ReadLEFloat(p + 56 + 4 * i);
        region.edgeBias[i] = ReadLEFloat(p + 72 + 4 * i);
      }
      region.mult = ReadLEFloat(p + 68);
      region.cap = ReadLEFloat(p + 84);
      for (int i = 0; i < 4; ++i) {
        region.color[i] = ReadLEFloat(p + 88 + 4 * i);
      }
      region.density = ReadLEFloat(p + 104);
      for (int i = 0; i < 6; ++i) {
        region.box[i] = ReadLEFloat(p + 108 + 4 * i);
      }
      const uint32_t links = ReadLE32(p + 132);
      at += kFogRegionBytes;
      // A region with anything that is not a number never shows.
      bool finite = std::isfinite(region.mult) && std::isfinite(region.cap) && std::isfinite(region.density);
      for (const float v : region.m) {
        finite = finite && std::isfinite(v);
      }
      for (int i = 0; i < 3; ++i) {
        finite = finite && std::isfinite(region.edgeScale[i]) && std::isfinite(region.edgeBias[i]);
      }
      for (const float v : region.color) {
        finite = finite && std::isfinite(v);
      }
      for (const float v : region.box) {
        finite = finite && std::isfinite(v);
      }
      if (!finite) {
        region = FogRegion{};
        region.box[0] = region.box[1] = region.box[2] = 1.f; // an empty box: never in view
      }
      if (links > kMaxGradeLinks || (data.size() - at) / 8 < links) {
        error = "cut short";
        return false;
      }
      region.links.resize(links);
      for (GradeLink& link : region.links) {
        const uint8_t* q = data.data() + at;
        link.sender = ReadLE32(q);
        link.state = q[4];
        link.action = q[5];
        at += 8;
      }
      if (version >= 15) {
        if (data.size() - at < 12) {
          error = "cut short";
          return false;
        }
        const float distance = ReadLEFloat(data.data() + at);
        const float transmittance = ReadLEFloat(data.data() + at + 4);
        region.subtract = data[at + 8] != 0;
        at += 12;
        if (std::isfinite(distance) && std::isfinite(transmittance)) {
          region.distance = distance;
          region.transmittance = transmittance;
        }
      }
    }
  }
  if (version >= 15) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t transitions = ReadLE32(data.data() + at);
    at += 4;
    if (transitions > kMaxFogRegions) {
      error = "too many fog transitions";
      return false;
    }
    out.transitions.resize(transitions);
    for (FogTransition& t : out.transitions) {
      if (data.size() - at < kFogTransitionBytes) {
        error = "cut short";
        return false;
      }
      const uint8_t* p = data.data() + at;
      t.region = ReadLE32(p);
      t.layer = int32_t(ReadLE32(p + 4));
      t.on = p[8] != 0;
      t.autoStart = p[9] != 0;
      t.loop = p[10] != 0;
      t.select = p[11] & 0xf;
      t.distance = ReadLEFloat(p + 12);
      t.transmittance = ReadLEFloat(p + 16);
      for (int i = 0; i < 4; ++i) {
        t.color[i] = ReadLEFloat(p + 20 + 4 * i);
      }
      t.cap = ReadLEFloat(p + 36);
      const uint32_t size = ReadLE32(p + 40);
      at += kFogTransitionBytes;
      bool finite = std::isfinite(t.distance) && std::isfinite(t.transmittance) && std::isfinite(t.cap);
      for (const float v : t.color) {
        finite = finite && std::isfinite(v);
      }
      // One that would poison its region, or names none, changes nothing.
      if (!finite || t.region >= out.regions.size()) {
        t.select = 0;
        t.region = kNoFogRegion;
      }
      const size_t padded = (size_t(size) + 3) & ~size_t(3);
      if (data.size() - at < padded) {
        error = "cut short";
        return false;
      }
      if (size != 0 && !t.phase.Load(data.data() + at, size)) {
        error = "bad fog transition spline";
        return false;
      }
      at += padded;
      if (data.size() - at < 4) {
        error = "cut short";
        return false;
      }
      const uint32_t links = ReadLE32(data.data() + at);
      at += 4;
      if (links > kMaxGradeLinks || (data.size() - at) / 8 < links) {
        error = "cut short";
        return false;
      }
      t.links.resize(links);
      for (GradeLink& link : t.links) {
        const uint8_t* q = data.data() + at;
        link.sender = ReadLE32(q);
        link.state = q[4];
        link.action = q[5];
        at += 8;
      }
      if (out.regions.empty()) {
        t.links.clear();
        t.autoStart = false;
      }
    }
  }
  if (version >= 16) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t suns = ReadLE32(data.data() + at);
    at += 4;
    const size_t size = version >= 24 ? 40 : version >= 17 ? 36 : 32;
    if ((data.size() - at) / size < suns) {
      error = "cut short";
      return false;
    }
    out.suns.resize(suns);
    for (SunLight& s : out.suns) {
      const uint8_t* q = data.data() + at;
      s.layer = int32_t(ReadLE32(q));
      s.on = q[4] != 0;
      for (int i = 0; i < 3; ++i) {
        s.toSun[i] = ReadLEFloat(q + 8 + 4 * i);
        s.color[i] = ReadLEFloat(q + 20 + 4 * i);
      }
      if (version >= 17) {
        s.group = ReadLE32(q + 32);
      }
      s.casts = version >= 24 && q[36] != 0;
      at += size;
    }
  }
  if (version >= 18) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    Lightmap& lm = out.lightmap;
    lm = Lightmap{};
    lm.width = ReadLE32(data.data() + at);
    at += 4;
    if (lm.width != 0) {
      if (data.size() - at < 16) {
        error = "cut short";
        return false;
      }
      const uint8_t* p = data.data() + at;
      lm.height = ReadLE32(p);
      lm.layers = ReadLE32(p + 4);
      lm.isSigned = ReadLE32(p + 8) != 0;
      lm.length = ReadLE32(p + 12);
      lm.offset = at + 16;
      at = lm.offset;
      // Whole 4x4 blocks of 16 bytes, every layer the same size.
      if (lm.width > kMaxLightmapSize || lm.height == 0 || lm.height > kMaxLightmapSize || lm.layers == 0 ||
          lm.layers > 16 ||
          lm.length != size_t(lm.layers) * ((size_t(lm.width) + 3) / 4) * ((size_t(lm.height) + 3) / 4) * 16 ||
          data.size() - at < lm.length) {
        error = "bad lightmap";
        return false;
      }
      at += lm.length;
    }
  }
  if (version >= 19) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t lights = ReadLE32(data.data() + at);
    at += 4;
    // The fixed part, the link count, the animation block and the spline sizes.
    constexpr size_t kSize = 64;
    constexpr size_t kAnimBytes = 36;
    constexpr size_t kLeast = kSize + 4 + kAnimBytes + 4 * kLightSplines;
    if ((data.size() - at) / kLeast < lights) {
      error = "cut short";
      return false;
    }
    out.lights.resize(lights);
    for (PointLight& l : out.lights) {
      if (data.size() - at < kLeast) {
        error = "cut short";
        return false;
      }
      const uint8_t* q = data.data() + at;
      l.layer = int32_t(ReadLE32(q));
      l.on = q[4] != 0;
      l.spot = q[5] != 0;
      l.falloff = q[6];
      for (int i = 0; i < 3; ++i) {
        l.pos[i] = ReadLEFloat(q + 8 + 4 * i);
        l.toLight[i] = ReadLEFloat(q + 20 + 4 * i);
        l.color[i] = ReadLEFloat(q + 32 + 4 * i);
      }
      for (int i = 0; i < 2; ++i) {
        l.nearFar[i] = ReadLEFloat(q + 44 + 4 * i);
        l.cone[i] = ReadLEFloat(q + 52 + 4 * i);
      }
      l.group = ReadLE32(q + 60);
      at += kSize;
      const uint32_t links = ReadLE32(data.data() + at);
      at += 4;
      if (links > kMaxGradeLinks || (data.size() - at) / 8 < links) {
        error = "cut short";
        return false;
      }
      l.links.resize(links);
      for (GradeLink& link : l.links) {
        const uint8_t* k = data.data() + at;
        link.sender = ReadLE32(k);
        link.state = k[4];
        link.action = k[5];
        at += 8;
      }
      if (data.size() - at < kAnimBytes) {
        error = "cut short";
        return false;
      }
      const uint8_t* a = data.data() + at;
      l.animated = a[0] != 0;
      l.playing = a[1] != 0;
      l.loop = a[2] != 0;
      l.offAtStart = a[3] != 0;
      l.animFalloff = a[4];
      l.length = ReadLEFloat(a + 8);
      l.scale[0] = ReadLEFloat(a + 12);
      l.scale[1] = ReadLEFloat(a + 16);
      l.intensity = ReadLEFloat(a + 20);
      for (int i = 0; i < 3; ++i) {
        l.rgb[i] = ReadLEFloat(a + 24 + 4 * i);
      }
      at += kAnimBytes;
      if (!std::isfinite(l.length) || l.length < 0.f) {
        l.length = 0.f;
      }
      for (int i = 0; i < kLightSplines; ++i) {
        if (data.size() - at < 4) {
          error = "cut short";
          return false;
        }
        const uint32_t size = ReadLE32(data.data() + at);
        at += 4;
        const size_t padded = (size_t(size) + 3) & ~size_t(3);
        if (data.size() - at < padded) {
          error = "cut short";
          return false;
        }
        if (size != 0) {
          if (!l.splines[i].Load(data.data() + at, size)) {
            error = "bad light spline";
            return false;
          }
          l.hasSpline[i] = true;
        }
        at += padded;
      }
    }
  }
  if (version >= 20) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t hints = ReadLE32(data.data() + at);
    at += 4;
    const size_t kHintBytes = version >= 27 ? 28 : 24;
    if ((data.size() - at) / kHintBytes < hints) {
      error = "cut short";
      return false;
    }
    for (uint32_t i = 0; i < hints; ++i, at += kHintBytes) {
      const uint8_t* q = data.data() + at;
      FluidHint h;
      h.mode = q[0];
      if (version >= 27) {
        h.on = q[3] != 0;
        h.priority = int32_t(ReadLE32(q + 24));
      }
      if (version >= 25 && q[1] <= 2) {
        h.role = q[1];
        h.pair = q[2];
      }
      h.exposure[0] = ReadLEFloat(q + 4);
      h.exposure[1] = ReadLEFloat(q + 8);
      h.bias = ReadLEFloat(q + 12);
      h.sigma = ReadLEFloat(q + 16);
      h.staticLerp = ReadLEFloat(q + 20);
      if (h.mode > 2 || (version < 27 && h.role == 0 && h.mode == 0) ||
          !std::isfinite(h.exposure[0]) || !std::isfinite(h.exposure[1]) || h.exposure[1] < h.exposure[0]) {
        continue;
      }
      if (!std::isfinite(h.bias)) {
        h.bias = 0.f;
      }
      if (!(h.sigma >= 0.f && h.sigma < 10000.f)) {
        h.sigma = 32.f;
      }
      if (!(h.staticLerp >= 0.f && h.staticLerp <= 1.f)) {
        h.staticLerp = 0.5f;
      }
      out.fluidHints.push_back(h);
    }
  }
  if (version >= 22) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const uint32_t volumes = ReadLE32(data.data() + at);
    at += 4;
    constexpr size_t kVolumeBytes = 15 * 4;
    if (volumes > kMaxWorldVolumes || (data.size() - at) / kVolumeBytes < volumes) {
      error = "cut short";
      return false;
    }
    out.worldVolumes.resize(volumes);
    for (uint32_t i = 0; i < volumes; ++i, at += kVolumeBytes) {
      WorldVolume& v = out.worldVolumes[i];
      float* const fields[3] = {v.centre, v.half, v.axes};
      const size_t counts[3] = {3, 3, 9};
      size_t o = at;
      for (int f = 0; f < 3; ++f) {
        for (size_t k = 0; k < counts[f]; ++k, o += 4) {
          fields[f][k] = ReadLEFloat(data.data() + o);
        }
      }
      // A box that reads wrong holds no point (its index stays, as links name it).
      bool ok = true;
      for (float x : v.centre) ok = ok && std::isfinite(x);
      for (float x : v.half) ok = ok && std::isfinite(x);
      for (float x : v.axes) ok = ok && std::isfinite(x);
      if (!ok) {
        v = WorldVolume{};
        v.half[0] = v.half[1] = v.half[2] = -1.f;
      }
    }
  }
  if (version >= 23) {
    if (data.size() - at < 12) {
      error = "cut short";
      return false;
    }
    for (int i = 0; i < 3; ++i, at += 4) {
      const float v = ReadLEFloat(data.data() + at);
      out.worldShift[i] = std::isfinite(v) ? v : 0.f;
    }
    for (FogHint& fog : out.fogs) {
      std::copy(out.worldShift, out.worldShift + 3, fog.worldShift);
    }
  }
  if (version >= 26) {
    if (data.size() - at < 4) {
      error = "cut short";
      return false;
    }
    const float v = ReadLEFloat(data.data() + at);
    at += 4;
    out.envFxIntensity = std::isfinite(v) && v >= 0.f ? v : 1.f;
  }
  out.data = std::move(data);
  return true;
}

bool InsideWorldVolume(const WorldVolume& volume, const float pos[3]) {
  const float d[3] = {pos[0] - volume.centre[0], pos[1] - volume.centre[1], pos[2] - volume.centre[2]};
  for (int axis = 0; axis < 3; ++axis) {
    const float* const a = volume.axes + 3 * axis;
    if (!(std::fabs(d[0] * a[0] + d[1] * a[1] + d[2] * a[2]) <= volume.half[axis])) {
      return false;
    }
  }
  return true;
}

bool SameWorldVolumes(const std::vector<WorldVolume>& a, const std::vector<WorldVolume>& b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(WorldVolume)) == 0);
}

void WorldVolumeState::Clear() {
  inside.clear();
  entered.clear();
  exited.clear();
  seq = 0;
}

void WorldVolumeState::Update(const std::vector<WorldVolume>& volumes, const float pos[3], std::vector<Event>& out) {
  if (inside.size() != volumes.size()) {
    Clear();
    inside.assign(volumes.size(), 0);
    entered.assign(volumes.size(), 0);
    exited.assign(volumes.size(), 0);
  }
  for (size_t i = 0; i < volumes.size(); ++i) {
    const bool now = InsideWorldVolume(volumes[i], pos);
    if (now == (inside[i] != 0)) {
      continue;
    }
    inside[i] = now ? 1 : 0;
    (now ? entered : exited)[i] = ++seq;
    out.push_back({uint32_t(i), now ? 0 : 1});
  }
}

void WorldVolumeState::Replay(std::vector<Event>& out) const {
  std::vector<std::pair<uint64_t, Event>> events;
  for (size_t i = 0; i < entered.size() && i < exited.size(); ++i) {
    if (entered[i] != 0) {
      events.push_back({entered[i], {uint32_t(i), 0}});
      if (exited[i] > entered[i]) {
        events.push_back({exited[i], {uint32_t(i), 1}});
      }
    }
  }
  std::sort(events.begin(), events.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  for (const auto& e : events) {
    out.push_back(e.second);
  }
}

bool ExposureHints::Load(const std::vector<FluidHint>& hints, bool cameraInside, uint8_t state) {
  inside = false;
  seen = false;
  pairState = state;
  active.assign(hints.size(), 0);
  disabled.assign(hints.size(), 0);
  order.assign(hints.size(), 0);
  if (cameraInside) {
    inside = seen = true;
  }
  return Update(hints, false);
}

bool ExposureHints::Set(const std::vector<FluidHint>& hints, bool cameraInside) {
  if (cameraInside == inside) {
    return false;
  }
  inside = cameraInside;
  seen = true;
  return Update(hints, true);
}

bool ExposureHints::SetPair(const std::vector<FluidHint>& hints, uint8_t state) {
  if (state == pairState) {
    return false;
  }
  pairState = state;
  return Update(hints, true);
}

bool ExposureHints::Update(const std::vector<FluidHint>& hints, bool snapMode0) {
  if (active.size() != hints.size()) {
    active.assign(hints.size(), 0);
    disabled.assign(hints.size(), 0);
    order.assign(hints.size(), 0);
  }
  const uint64_t firstRegistration = next;
  priority.resize(hints.size());
  for (size_t i = 0; i < hints.size(); ++i) {
    const FluidHint& hint = hints[i];
    priority[i] = hint.priority;
    disabled[i] = hint.role == 0 ? 0 : (hint.role == 1 ? pairState == 0 : pairState != 0) ? 1 : 0;
    bool eligible = false;
    switch (hint.mode) {
    case 0:
      eligible = true;
      break;
    case 1:
      eligible = inside;
      break;
    case 2:
      eligible = seen && !inside;
      break;
    default:
      break;
    }
    const uint8_t now = eligible && hint.on && !disabled[i] ? 1 : 0;
    if (now == active[i]) {
      continue;
    }
    active[i] = now;
    if (now != 0) {
      order[i] = next++;
    } else {
      order[i] = 0;
    }
  }
  const int selected = Pick(hints.size());
  // The transition type belongs to the selected registration, not any other
  // hint that happened to register this frame (UpdateTonemapping 0xaffb18).
  return selected >= 0 && order[selected] >= firstRegistration && (hints[selected].mode != 0 || snapMode0);
}

int ExposureHints::Pick(size_t count) const {
  int best = -1;
  for (size_t i = 0; i < count && i < order.size(); ++i) {
    // RegisterAutoExposureHintObject 0xb0029c inserts before the first priority <=
    // the new one. UpdateTonemapping 0xaffb18 takes the first matching area:
    // higher priority first, newest registration first only for equal priorities.
    if (order[i] != 0 && (best < 0 || priority[i] > priority[best] ||
                         (priority[i] == priority[best] && order[i] > order[best]))) {
      best = int(i);
    }
  }
  return best;
}

void ParamTransition::Snap(const float target[6]) {
  std::memcpy(from, target, sizeof(from));
  std::memcpy(to, target, sizeof(to));
  std::memcpy(shown, target, sizeof(shown));
  progress = 0.f;
  moving = false;
}

void ParamTransition::Step(const float target[6], float dt, float time) {
  std::memcpy(to, target, sizeof(to));
  if (!moving && std::memcmp(target, from, sizeof(from)) != 0) {
    moving = true;
    progress = 0.f;
  }
  if (moving) {
    progress = std::min(progress + dt / time, 1.f);
    for (int i = 0; i < 6; ++i) {
      shown[i] = from[i] + (to[i] - from[i]) * progress;
    }
    if (progress >= 1.f) {
      std::memcpy(from, to, sizeof(from));
      moving = false;
    }
  } else {
    std::memcpy(shown, from, sizeof(shown));
  }
}

void Convergence::SetSigma(float sigma) {
  // The coefficients of CGaussianConvergence's constructor.
  const double s = sigma;
  double q;
  if (s >= 2.5) {
    q = 0.98711 * s - 0.96330;
  } else if (s >= 0.5) {
    q = 3.97156 - 4.14554 * std::sqrt(std::max(1.0 - 0.26891 * s, 1e-35));
  } else {
    q = s * 0.1147 * 2.0;
  }
  const double q2 = q * q;
  const double q3 = q2 * q;
  const double b0 = 1.57825 + 2.44413 * q + 1.4281 * q2 + 0.422205 * q3;
  const double b1 = 2.44413 * q + 2.85619 * q2 + 1.26661 * q3;
  const double b2 = -(1.4281 * q2 + 1.26661 * q3);
  const double b3 = 0.422205 * q3;
  coeff[0] = 1.0 - (b1 + b2 + b3) / b0;
  coeff[1] = b1 / b0;
  coeff[2] = b2 / b0;
  coeff[3] = b3 / b0;
}

void Convergence::SetValue(float v) {
  value = history[0] = history[1] = history[2] = v;
}

void Convergence::Step(float target) {
  const double next = coeff[0] * target + coeff[1] * history[0] + coeff[2] * history[1] + coeff[3] * history[2];
  history[2] = history[1];
  history[1] = history[0];
  history[0] = next;
  value = float(next);
}

void ExposureAverage::Reset() {
  L = -1.f;
  created = false;
}

void ExposureAverage::Take(const float rgb[3]) {
  L = std::max(std::max(rgb[0], rgb[1]), rgb[2]);
}

float ExposureAverage::Update(bool hint, float tonemapEv, float lo, float hi, float bias, float s, bool snap,
                              int steps, float* target) {
  constexpr float kGrey = 0.2158605f; // sRGB 128, linear
  if (!hint || !(L >= 0.f)) {
    *target = tonemapEv;
    return tonemapEv;
  }
  if (sigma != s) {
    sigma = s;
    ev.SetSigma(s);
  }
  *target = std::min(std::max(std::log2(L / kGrey) + 3.f + bias, lo), hi);
  if (!created) {
    created = true;
    ev.SetValue(*target);
  } else if (snap) {
    ev.SetValue(*target);
  } else {
    for (int i = steps; i > 0; --i) {
      ev.Step(*target);
    }
  }
  return ev.value;
}

void SetExtents(Probe& probe) {
  float volume = 8.f;
  for (int row = 0; row < 3; ++row) {
    const float* r = probe.worldToBox + row * 4;
    // The row's length is 1 / half extent.
    const float scale = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    probe.half[row] = scale > 1e-12f ? 1.f / scale : 0.f;
    volume *= probe.half[row];
  }
  probe.volume = volume;
}

bool ProbeOn(const Probe& probe, uint64_t activeLayers) {
  return probe.layer < 0 || probe.layer >= 64 || (activeLayers >> probe.layer & 1) != 0;
}

Pick PickProbe(const File& file, const float pos[3], uint64_t activeLayers) {
  Pick best;
  for (size_t i = 0; i < file.probes.size(); ++i) {
    const Probe& probe = file.probes[i];
    if (!ProbeOn(probe, activeLayers)) {
      continue;
    }
    const float* m = probe.worldToBox;
    Pick pick;
    pick.probe = int(i);
    pick.inside = true;
    float distance2 = 0.f;
    for (int row = 0; row < 3; ++row) {
      const float* r = m + row * 4;
      const float u = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
      const float half = probe.half[row];
      const float over = std::fabs(u) - 1.f;
      if (!(over <= 0.f)) {
        pick.inside = false;
        distance2 += over * half * over * half;
      }
    }
    pick.score = pick.inside ? probe.volume : std::sqrt(distance2);
    if (pick.Better(best)) {
      best = pick;
    }
  }
  return best;
}

float ProbeFade(const Probe& probe, const float pos[3], bool& inside) {
  // The farthest a point is outside the box along any of its axes, in metres.
  float farthest = 0.f;
  for (int row = 0; row < 3; ++row) {
    const float* r = probe.worldToBox + row * 4;
    const float u = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
    const float scale = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    const float over = std::max(std::fabs(u) - 1.f, 0.f);
    farthest = std::max(farthest, scale > 1e-12f ? over / scale : over);
  }
  inside = farthest <= 0.f;
  if (inside) {
    return 1.f;
  }
  if (!(probe.padding > 0.f)) {
    return 0.f;
  }
  return 1.f - std::min(farthest / probe.padding, 1.f);
}

void UpdateBlend(const BlendCandidate* candidates, size_t count, const float pos[3], Blend& blend) {
  auto find = [&](uint64_t key) -> const Probe* {
    for (size_t i = 0; i < count; ++i) {
      if (candidates[i].key == key) {
        return candidates[i].probe;
      }
    }
    return nullptr;
  };
  std::vector<BlendEntry> next;
  auto listed = [&](uint64_t key) {
    return std::any_of(next.begin(), next.end(), [key](const BlendEntry& e) { return e.key == key; });
  };
  bool inside = false;
  // The last frame's probes first, so that they win ties of priority.
  for (const BlendEntry& entry : blend.entries) {
    const Probe* probe = find(entry.key);
    if (probe != nullptr && !listed(entry.key) && ProbeFade(*probe, pos, inside) > 0.f) {
      next.push_back({entry.key, probe, 0.f});
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const BlendCandidate& c = candidates[i];
    if (c.probe != nullptr && !listed(c.key) && ProbeFade(*c.probe, pos, inside) > 0.f) {
      next.push_back({c.key, c.probe, 0.f});
    }
  }
  std::stable_sort(next.begin(), next.end(),
                   [](const BlendEntry& a, const BlendEntry& b) { return a.probe->priority > b.probe->priority; });
  if (next.size() > kMaxBlend) {
    next.resize(kMaxBlend);
  }
  float remaining = 1.f;
  for (BlendEntry& entry : next) {
    const float fade = ProbeFade(*entry.probe, pos, inside);
    if (inside) {
      entry.weight = remaining;
      remaining = 0.f;
    } else {
      entry.weight = remaining * fade;
      remaining *= 1.f - fade;
    }
  }
  next.erase(std::remove_if(next.begin(), next.end(), [](const BlendEntry& e) { return std::fabs(e.weight) < 1e-5f; }),
             next.end());
  blend.entries = std::move(next);
  if (blend.entries.empty()) {
    blend.intensity = 1.f;
    blend.min = 0.f;
    blend.max = 1.f;
  } else if (blend.entries.size() == 1) {
    BlendEntry& only = blend.entries[0];
    only.weight = 1.f;
    blend.intensity = only.probe->scale;
    blend.min = only.probe->intensityMin;
    blend.max = only.probe->intensityMax;
  } else {
    float sum = 0.f;
    float min = 0.f;
    float max = 0.f;
    for (const BlendEntry& entry : blend.entries) {
      sum += entry.weight * entry.probe->scale;
      min += entry.weight * entry.probe->intensityMin;
      max += entry.weight * entry.probe->intensityMax;
    }
    if (std::fabs(sum) >= 1e-5f) {
      for (BlendEntry& entry : blend.entries) {
        entry.weight = entry.weight * entry.probe->scale / sum;
      }
    }
    blend.intensity = sum;
    blend.min = min;
    blend.max = max;
  }
}

bool SampleGrid(const File& file, const Grid& grid, const float pos[3], Ambient& out) {
  const float* m = grid.worldToGrid;
  float at[3];
  int cell[3];
  for (int row = 0; row < 3; ++row) {
    const float* r = m + row * 4;
    at[row] = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
    // The baked-probe textures are sampled with CLAMP_TO_BORDER and a transparent black
    // border (0x1c93f0): taps past the edge read 0, so the light is gone half a cell out.
    if (!(at[row] >= -0.5f && at[row] <= float(grid.size[row]) - 0.5f)) {
      return false;
    }
    cell[row] = int(std::floor(at[row]));
    at[row] -= float(cell[row]);
  }
  // mean, lobe, sharpness, then the three directions along the grid's axes
  float sum[18] = {};
  for (int corner = 0; corner < 8; ++corner) {
    float weight = 1.f;
    size_t index = 0;
    bool outside = false;
    for (int axis = 2; axis >= 0; --axis) {
      const int high = (corner >> axis) & 1;
      const int i = cell[axis] + high;
      weight *= high != 0 ? at[axis] : 1.f - at[axis];
      outside = outside || i < 0 || i >= int(grid.size[axis]);
      index = index * grid.size[axis] + size_t(outside ? 0 : i);
    }
    if (outside || weight <= 0.f) {
      continue; // border texel: zero
    }
    const uint8_t* base = grid.points ? grid.points->data() : file.data.data() + grid.offset;
    const uint8_t* p = base + index * kPointSize;
    const float mean[3] = {GetHalf(p), GetHalf(p + 2), GetHalf(p + 4)};
    for (int i = 0; i < 3; ++i) {
      sum[i] += weight * mean[i];
      sum[3 + i] += weight * GetHalf(p + 6 + i * 2);
      sum[6 + i] += weight * float(p[12 + i]) / 255.f;
    }
    for (int i = 0; i < 9; ++i) {
      sum[9 + i] += weight * (float(p[15 + i]) / 127.5f - 1.f);
    }
  }
  // Plain trilinear, as the GPU pass: empty texels count as stored, nothing renormalised.
  // The grid's axes are the world's turned and scaled alike, so a direction goes back
  // through the transpose.
  const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  if (!(scale > 1e-12f)) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    out.mean[i] = sum[i];
    out.lobe[i] = sum[3 + i];
    out.sharpness[i] = sum[6 + i];
    const float* d = sum + 9 + i * 3;
    for (int axis = 0; axis < 3; ++axis) {
      out.direction[i][axis] = (m[axis] * d[0] + m[4 + axis] * d[1] + m[8 + axis] * d[2]) / scale;
    }
  }
  return true;
}

void PowerBombBakedLight(float seconds, float rgb[3]) {
  float t = 0.f;
  if (seconds >= 4.5f) {
    t = 0.f;
  } else if (seconds >= 4.f) {
    t = 1.f - 2.f * (seconds - 4.f);
  } else if (seconds >= 3.5f) {
    t = 1.f;
  } else if (seconds >= 1.75f) {
    t = (seconds - 1.75f) / 1.75f;
  }
  constexpr float kFlash[3] = {1.f * 35.f, 0.643f * 35.f, 0.298f * 35.f};
  for (int c = 0; c < 3; ++c) {
    rgb[c] = 1.f + (kFlash[c] - 1.f) * t;
  }
}

} // namespace PortRoomEnv
