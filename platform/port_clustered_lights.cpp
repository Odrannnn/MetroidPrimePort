// Remastered's clustered light gather on the CPU: the grid, the 80-byte light records and the
// per-cluster mask. See port_clustered_lights.h for what native does and where the port cannot
// match it.
#include "port_clustered_lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PortClusteredLights {
namespace {

// The shader's slice scale, c5[1].y: float(32)/500.
constexpr float kSliceScale = float(kSlices) / kMaxDepth;
// CLightGatherBin::FilterLights treats a colour this close to black as black (0x1d14690's
// 1e-5).
constexpr float kBlack = 1e-5f;

// A cluster's face, as dot(n, p) + d <= 0 inside, n not yet unit length.
struct Plane {
  float nx, ny, nz, d;
};

Plane Unit(Plane p) {
  const float len = std::sqrt(p.nx * p.nx + p.ny * p.ny + p.nz * p.nz);
  const float inv = len > 0.f ? 1.f / len : 0.f;
  return {p.nx * inv, p.ny * inv, p.nz * inv, p.d * inv};
}

bool Outside(const Plane& p, const float c[3], float r) {
  return p.nx * c[0] + p.ny * c[1] + p.nz * c[2] + p.d > r;
}

float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool FiniteLight(const RawLight& l) {
  if (!std::isfinite(l.radius)) {
    return false;
  }
  for (size_t i = 0; i < kLightFloats; ++i) {
    if (!std::isfinite(l.record[i])) {
      return false;
    }
  }
  return true;
}

// The camera's frustum: |x| <= tanHalfX * depth, |y| <= tanHalfY * depth, nearZ <= depth <=
// farZ, with depth = -z. Native tests the light's sphere against the camera's planes
// (node+0xc4 against param_4+0x6c); these are the same six.
void FrustumPlanes(const Camera& camera, Plane planes[6]) {
  planes[0] = Unit({1.f, 0.f, camera.tanHalfX, 0.f});
  planes[1] = Unit({-1.f, 0.f, camera.tanHalfX, 0.f});
  planes[2] = Unit({0.f, 1.f, camera.tanHalfY, 0.f});
  planes[3] = Unit({0.f, -1.f, camera.tanHalfY, 0.f});
  planes[4] = {0.f, 0.f, 1.f, camera.nearZ};
  planes[5] = {0.f, 0.f, -1.f, -camera.farZ};
}

} // namespace

void DistanceAttenuation(float nearD, float farD, float& a, float& b, float& far) {
  const float near = std::max(nearD, 0.f);
  far = std::max(farD, 0.f);
  if (!(far > near)) {
    far = near + 0.001f; // FromValues 0x173798
  }
  a = 1.f / (far - near);
  b = -near * a;
}

void ConeAttenuation(bool spot, float innerDeg, float outerDeg, float& scale, float& bias) {
  // CLight ctor 0x1741b4 gives a point light SLightAngleAttenuation::None: (0, 1).
  scale = 0.f;
  bias = 1.f;
  if (!spot) {
    return;
  }
  constexpr float kDegrees = 3.14159265f / 180.f;
  // build_light_angle_attenuation 0x89d2a0 constructs CAbsAngle half angles;
  // FromValues 0x173a50 widens an equal/reversed cone by 0.001 radians.
  const auto wrap = [](float radians) {
    const float value = radians - std::trunc(radians * 0.15915494f) * 6.2831855f;
    return value < 0.f ? value + 6.2831855f : value;
  };
  const float inner = wrap(innerDeg * 0.5f * kDegrees);
  float outer = wrap(outerDeg * 0.5f * kDegrees);
  if (outer <= inner) {
    outer = wrap(inner + 0.001f);
  }
  const float cosIn = std::cos(inner);
  const float cosOut = std::cos(outer);
  const float span = cosIn - cosOut;
  scale = 1.f / span;
  bias = -cosOut / span;
}

void FalloffCubic(int falloff, float cubic[4]) {
  static constexpr float kTable[4][4] = {
      {0.f, 0.f, 0.f, 1.f},  // 0: no falloff
      {0.f, 0.f, -1.f, 1.f}, // 1: 1 - t
      {0.f, 1.f, -2.f, 1.f}, // 2: (1 - t)^2
      {2.f, -3.f, 0.f, 1.f}, // 3: 1 - 3t^2 + 2t^3
  };
  const int type = std::clamp(falloff, 0, 3);
  for (int i = 0; i < 4; ++i) {
    cubic[i] = kTable[type][i];
  }
}

RawLight MakeLight(const LightSource& source, const float worldToView[3][4]) {
  RawLight out;
  for (int c = 0; c < 3; ++c) {
    out.record[0 + c] = source.color[c];
  }
  out.record[3] = 0.f; // widen: CLight+0x74, 0 for a script light
  for (int r = 0; r < 3; ++r) {
    const float* m = worldToView[r];
    out.record[4 + r] = m[0] * source.pos[0] + m[1] * source.pos[1] + m[2] * source.pos[2] + m[3];
    out.record[8 + r] = m[0] * source.toLight[0] + m[1] * source.toLight[1] + m[2] * source.toLight[2];
  }
  float scale = 0.f;
  float bias = 1.f;
  ConeAttenuation(source.spot, source.innerDeg, source.outerDeg, scale, bias);
  out.record[7] = scale;  // +0x1c
  out.record[11] = bias;  // +0x2c
  float a = 0.f;
  float b = 0.f;
  DistanceAttenuation(source.nearD, source.farD, a, b, out.radius);
  out.record[12] = a; // +0x30
  out.record[13] = b; // +0x34
  out.record[14] = 0.f;
  out.record[15] = 0.f;
  FalloffCubic(source.falloff, out.record + 16); // +0x40
  return out;
}

Camera MakeCamera(const float worldToView[3][4], float left, float right, float top, float bottom, float nearZ,
                  float farZ) {
  Camera camera;
  std::memcpy(camera.worldToView, worldToView, sizeof(camera.worldToView));
  const float near = nearZ > 0.f ? nearZ : 1e-4f;
  camera.tanHalfX = (right - left) * 0.5f / near;
  camera.tanHalfY = (top - bottom) * 0.5f / near;
  camera.nearZ = nearZ;
  camera.farZ = farZ;
  return camera;
}

Grid MakeGrid(uint32_t width, uint32_t height) {
  Grid grid;
  grid.width = width;
  grid.height = height;
  const int across = int((width + kPixelsPerTile - 1) / kPixelsPerTile);
  const int down = int((height + kPixelsPerTile - 1) / kPixelsPerTile);
  grid.tilesX = width == 0 ? 0 : std::min(across, kMaxTilesX);
  grid.tilesY = height == 0 ? 0 : std::min(down, kMaxTilesY);
  return grid;
}

uint32_t TilePixelsX(const Grid& grid) {
  return grid.tilesX > 0 ? (grid.width + uint32_t(grid.tilesX) - 1) / uint32_t(grid.tilesX) : 0;
}

uint32_t TilePixelsY(const Grid& grid) {
  return grid.tilesY > 0 ? (grid.height + uint32_t(grid.tilesY) - 1) / uint32_t(grid.tilesY) : 0;
}

int SliceForDepth(float depth, float depthOrigin) {
  const float clamped = std::clamp(depth - depthOrigin, 0.f, kMaxDepth);
  const int slice = int(clamped * kSliceScale);
  return std::clamp(slice, 0, kSlices - 1);
}

int SliceForViewZ(float viewZ, float depthOrigin) { return SliceForDepth(-viewZ, depthOrigin); }

void Header(const Grid& grid, float out[kHeaderFloats], uint32_t validity, float depthOrigin) {
  // recip(ucvtf(CClusteredData+0xc)) and +0x10 (build_clustered_light_material_data 0x3b8750).
  out[0] = grid.tilesX > 0 ? 1.f / float(TilePixelsX(grid)) : 1.f / float(kPixelsPerTile);
  out[1] = grid.tilesY > 0 ? 1.f / float(TilePixelsY(grid)) : 1.f / float(kPixelsPerTile);
  out[2] = float(grid.tilesX);
  out[3] = float(grid.tilesY);
  out[4] = 1.f; // 1 / CClusteredData+0x14, the depth-mapping exponent (a power of two here, so 1)
  out[5] = kSliceScale;
  out[6] = depthOrigin;
  out[7] = kMaxDepth;
  // c5[2].x and .y are ints in float slots; the shader reads c5[2].y with floatBitsToInt.
  const uint32_t tiles = uint32_t(grid.tilesX * grid.tilesY);
  const uint32_t across = uint32_t(grid.tilesX);
  uint32_t bits[4] = {tiles, across, validity, 0u}; // .w: mask source, 0 = a texture
  for (int i = 0; i < 4; ++i) {
    std::memcpy(&out[8 + i], &bits[i], sizeof(uint32_t));
  }
}

uint32_t Frame::Mask(int tileX, int tileY, int slice) const {
  if (!(grid.tilesX > 0 && grid.tilesY > 0) || tileX < 0 || tileY < 0 || slice < 0 || tileX >= grid.tilesX ||
      tileY >= grid.tilesY || slice >= kSlices) {
    return 0;
  }
  const size_t tiles = size_t(grid.tilesX) * size_t(grid.tilesY);
  const size_t at = size_t(slice) * tiles + size_t(tileY) * size_t(grid.tilesX) + size_t(tileX);
  return at < mask.size() ? mask[at] : 0;
}

int Frame::SetBits() const {
  int bits = 0;
  for (uint32_t word : mask) {
    for (uint32_t m = word; m != 0; m &= m - 1) {
      ++bits;
    }
  }
  return bits;
}

void Build(const Camera& camera, uint32_t width, uint32_t height, const RawLight* lights, int count, Frame& out) {
  out.grid = MakeGrid(width, height);
  out.lightCount = 0;
  out.validity = 0;
  out.depthOrigin = 0.f; // *(CCameraSceneNode+0x64), unverified; 0 until it is known
  std::memset(out.records, 0, sizeof(out.records));
  out.mask.assign(size_t(std::max(out.grid.Clusters(), 0)), 0u);
  Header(out.grid, out.header, 0, out.depthOrigin);
  if (out.grid.tilesX <= 0 || out.grid.tilesY <= 0 || lights == nullptr || count <= 0) {
    return;
  }

  // FilterLights: the lights that are in the vector at all, in order, then the 32-light cap
  // the mask's one word per cluster allows.
  Plane frustum[6];
  FrustumPlanes(camera, frustum);
  for (int i = 0; i < count && out.lightCount < kMaxLights; ++i) {
    const RawLight& light = lights[i];
    if (!FiniteLight(light)) {
      continue;
    }
    const float* rgb = light.record;
    if (!(rgb[0] > kBlack || rgb[1] > kBlack || rgb[2] > kBlack)) {
      continue; // ~black
    }
    if (!(light.radius > 0.f)) {
      continue;
    }
    const float* pos = light.record + 4;
    // FilterLights also drops a light farther than `ctx+0x64 + 1 + node+0xd0 * 1.2584085` from
    // the view position. ctx+0x64 is the same unverified camera field the block's depth origin
    // comes from; taking it as 0 would drop every light whose influence radius does not reach
    // the eye, so the rule is left out until that field is pinned (the per-cluster sphere test
    // below still trims what each cluster draws).
    bool misses = false;
    for (const Plane& plane : frustum) {
      misses = misses || Outside(plane, pos, light.radius);
    }
    if (misses) {
      continue;
    }
    std::memcpy(out.records + out.lightCount * kLightFloats, light.record, kLightBytes);
    out.validity |= 1u << out.lightCount;
    ++out.lightCount;
  }
  Header(out.grid, out.header, out.validity, out.depthOrigin);

  // CullLightsGPUByType: the clusters each light's sphere reaches. A cluster is the tile's
  // angular sector between two slice planes; the sphere misses it when it is wholly outside
  // any of the six faces.
  const size_t tilesWide = size_t(out.grid.tilesX) * size_t(out.grid.tilesY);
  const uint32_t tileW = TilePixelsX(out.grid);
  const uint32_t tileH = TilePixelsY(out.grid);
  for (int i = 0; i < out.lightCount; ++i) {
    const RawLight& light = lights[i];
    const float* pos = light.record + 4;
    const float r = light.radius;
    const float depthNear = -pos[2] - r;
    const float depthFar = -pos[2] + r;
    if (depthFar < camera.nearZ) {
      continue;
    }
    // The slice range: the shader maps depth to a slice linearly, so the sphere's own depth
    // interval is the answer (the per-cluster test below trims it further).
    const int sliceLo = SliceForDepth(std::max(depthNear, 0.f), out.depthOrigin);
    const int sliceHi = SliceForDepth(std::max(depthFar, 0.f), out.depthOrigin);
    // The tile range: the sphere's view-space box projected through the frustum's tangents.
    // The eye is inside the box when depthNear <= 0, and then every tile is a candidate.
    int tileXLo = 0;
    int tileXHi = out.grid.tilesX - 1;
    int tileYLo = 0;
    int tileYHi = out.grid.tilesY - 1;
    if (depthNear > 1e-4f && camera.tanHalfX > 0.f && camera.tanHalfY > 0.f) {
      const float depths[2] = {depthNear, depthFar};
      float ndcXLo = 1e30f;
      float ndcXHi = -1e30f;
      float ndcYLo = 1e30f;
      float ndcYHi = -1e30f;
      for (float x : {pos[0] - r, pos[0] + r}) {
        for (float depth : depths) {
          const float q = x / depth / camera.tanHalfX;
          ndcXLo = std::min(ndcXLo, q);
          ndcXHi = std::max(ndcXHi, q);
        }
      }
      for (float y : {pos[1] - r, pos[1] + r}) {
        for (float depth : depths) {
          const float q = y / depth / camera.tanHalfY;
          ndcYLo = std::min(ndcYLo, q);
          ndcYHi = std::max(ndcYHi, q);
        }
      }
      // NDC to the shader's tile: x from the left, y from the top (a fragment's position is in
      // target pixels, y down), tiles of TilePixels pixels.
      const auto toTile = [](float ndc, uint32_t pixels, uint32_t tilePixels) {
        const float t = std::clamp(ndc * 0.5f + 0.5f, 0.f, 1.f);
        return int(std::floor(t * float(pixels) / float(tilePixels)));
      };
      // One tile of slack either side keeps the range conservative; the per-cluster test is
      // what decides.
      tileXLo = std::max(0, toTile(ndcXLo, out.grid.width, tileW) - 1);
      tileXHi = std::min(out.grid.tilesX - 1, toTile(ndcXHi, out.grid.width, tileW) + 1);
      tileYLo = std::max(0, toTile(-ndcYHi, out.grid.height, tileH) - 1);
      tileYHi = std::min(out.grid.tilesY - 1, toTile(-ndcYLo, out.grid.height, tileH) + 1);
    }
    for (int tileY = tileYLo; tileY <= tileYHi; ++tileY) {
      // Each face is 0 on its own edge (view -z forward: an edge at ndc n is y = -tanHalf * n * z) and
      // positive outside the tile.
      // Row tileY spans pixels tileY*tileH .. (tileY+1)*tileH from the top, the last cut at the edge.
      const float ndcY0 = 1.f - 2.f * float(std::min(uint32_t(tileY + 1) * tileH, out.grid.height)) /
                                    float(out.grid.height);
      const float ndcY1 = 1.f - 2.f * float(std::min(uint32_t(tileY) * tileH, out.grid.height)) /
                                    float(out.grid.height);
      const Plane planeY0 = Unit({0.f, 1.f, camera.tanHalfY * ndcY1, 0.f});
      const Plane planeY1 = Unit({0.f, -1.f, -camera.tanHalfY * ndcY0, 0.f});
      for (int tileX = tileXLo; tileX <= tileXHi; ++tileX) {
        const float ndcX0 = -1.f + 2.f * float(std::min(uint32_t(tileX) * tileW, out.grid.width)) /
                                     float(out.grid.width);
        const float ndcX1 = -1.f + 2.f * float(std::min(uint32_t(tileX + 1) * tileW, out.grid.width)) /
                                     float(out.grid.width);
        const Plane planeX0 = Unit({1.f, 0.f, camera.tanHalfX * ndcX1, 0.f});
        const Plane planeX1 = Unit({-1.f, 0.f, -camera.tanHalfX * ndcX0, 0.f});
        if (Outside(planeX0, pos, r) || Outside(planeX1, pos, r) || Outside(planeY0, pos, r) ||
            Outside(planeY1, pos, r)) {
          continue;
        }
        for (int slice = sliceLo; slice <= sliceHi; ++slice) {
          const float sliceNear = std::max(out.depthOrigin + float(slice) * kSliceDepth, 0.f);
          const float sliceFar = out.depthOrigin + float(slice + 1) * kSliceDepth;
          const Plane planeNear = {0.f, 0.f, 1.f, sliceNear};
          const Plane planeFar = {0.f, 0.f, -1.f, -sliceFar};
          if (Outside(planeNear, pos, r) || Outside(planeFar, pos, r)) {
            continue;
          }
          out.mask[size_t(slice) * tilesWide + size_t(tileY) * size_t(out.grid.tilesX) + size_t(tileX)] |=
              1u << i;
        }
      }
    }
  }
}

namespace {

// The cache Update keeps: everything Build reads, so a frame that changes none of it reuses
// the last build.
struct Cache {
  bool valid = false;
  Camera camera;
  uint32_t width = 0;
  uint32_t height = 0;
  int count = 0;
  RawLight lights[kMaxLights];
  Frame frame;
};

Cache& Cached() {
  static Cache cache;
  return cache;
}

bool SameCamera(const Camera& a, const Camera& b) { return std::memcmp(&a, &b, sizeof(Camera)) == 0; }

} // namespace

const Frame& Update(const Camera& camera, uint32_t width, uint32_t height, const RawLight* lights, int count) {
  Cache& cache = Cached();
  const int kept = std::clamp(count, 0, kMaxLights);
  if (cache.valid && cache.width == width && cache.height == height && cache.count == kept &&
      SameCamera(cache.camera, camera) &&
      (kept == 0 || std::memcmp(cache.lights, lights, sizeof(RawLight) * size_t(kept)) == 0)) {
    return cache.frame;
  }
  if (kept > 0) {
    std::memcpy(cache.lights, lights, sizeof(RawLight) * size_t(kept));
  }
  cache.camera = camera;
  cache.width = width;
  cache.height = height;
  cache.count = kept;
  cache.valid = true;
  Build(camera, width, height, cache.lights, kept, cache.frame);
  return cache.frame;
}

const Frame& Current() { return Cached().frame; }

void ResetCache() { Cached() = Cache{}; }

} // namespace PortClusteredLights
