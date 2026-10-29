// Remastered model -> GameCube CMDL/CSKR/TXTR. See port_remastered_convert.h.
//
// The arithmetic follows the reference converter this was ported from step for
// step (double precision geometry, the order sums are taken in, round half to
// even), so that both produce the same bytes for the same model and one can be
// checked against the other. Where a line looks needlessly particular about
// the order of an addition, that is why.
//
// This file holds the textures (Converter::State) and the conversion itself; the rest is in
// the files port_remastered_convert_internal.h lists.

#include "port_remastered_convert_internal.h"

namespace PortRemastered {

void NormalMatrix(const double (*M)[3], double out[3][3]) {
  // The cofactor matrix over the determinant is the inverse transpose.
  const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                     M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                     M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      if (std::fabs(det) < 1e-12) {
        out[r][c] = 0.0;
        continue;
      }
      const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
      out[r][c] = (M[r1][c1] * M[r2][c2] - M[r1][c2] * M[r2][c1]) / det;
    }
  }
}

using namespace ConvertDetail;

// ---- textures ----

struct Converter::State {
  ConvertIO io;
  std::map<std::string, std::optional<uint32_t>> ids;  // tag -> texture id, or none where retail's stays
  std::map<uint32_t, std::string> owner;               // texture id -> the tag it was written for
  // The last few textures decoded: a map that was measured is usually written
  // straight after.
  std::vector<std::pair<std::string, Image>> opened;
  int pbr = 0, tev = 0;
  uint32_t model = 0;  // the retail id being converted, for the joint log

  void Log(const std::string& line) const {
    if (io.log) {
      io.log(line);
    }
  }

  const Image& Open(const MapRef& map) {
    for (auto& e : opened) {
      if (e.first == map.src) {
        return e.second;
      }
    }
    Image img;
    std::string error;
    if (!(map.volume ? io.volume && io.volume(map.id, img, error) : io.texture(map.id, img, error))) {
      throw Fail{"texture " + IdToString(map.id) + ": " + error};
    }
    if (img.width <= 0 || img.height <= 0 || img.rgba.size() != size_t(img.width) * size_t(img.height) * 4) {
      throw Fail{"texture " + IdToString(map.id) + " decoded to nothing"};
    }
    if (opened.size() >= 3) {
      opened.erase(opened.begin());
    }
    opened.emplace_back(map.src, std::move(img));
    return opened.back().second;
  }

  // What an earlier import measured of a texture (ConvertIO::recall), else measure() now: a
  // texture whose outputs are reused is then not decoded just to name them.
  template <class F>
  std::string Fact(const std::string& key, F&& measure) {
    std::string value;
    if (io.recall && io.recall(key, value)) {
      return value;
    }
    value = measure();
    if (io.remember) {
      io.remember(key, value, {});
    }
    return value;
  }

  uint32_t TexId(const std::string& tag) const {
    uint32_t h = Crc32("mprem:" + tag);
    while (io.retailId && io.retailId(h)) {
      ++h;
    }
    return h;
  }

  // How much alpha the retail texture in a slot carries, so the replacement
  // keeps as much: "none", "punch" (CMPR with a transparent texel) or "full".
  std::string RetailAlpha(uint32_t id) const {
    Blob d;
    if (!io.retail(FourCC('T', 'X', 'T', 'R'), id, d) || d.size() < 12) {
      return "none";
    }
    const Span s{d.data(), d.size()};
    const uint32_t fmt = R32(s, 0);
    const uint32_t w = R16(s, 4), h = R16(s, 6);
    if (fmt == 0 || fmt == 1 || fmt == 7) {
      return "none";
    }
    if (fmt != 10) {
      return "full";
    }
    const size_t bytes = std::min(size_t(std::max(8u, w)) * std::max(8u, h) / 2, d.size() - 12) / 8 * 8;
    for (size_t o = 12; o < 12 + bytes; o += 8) {
      const uint16_t c0 = R16(s, o), c1 = R16(s, o + 2);
      if (c0 > c1) {
        continue;
      }
      for (int k = 0; k < 4; ++k) {
        const uint8_t b = d[o + 4 + k];
        if ((b & 0xC0) == 0xC0 || (b & 0x30) == 0x30 || (b & 0x0C) == 0x0C || (b & 0x03) == 0x03) {
          return "punch";
        }
      }
    }
    return "none";
  }

  void Write(const std::string& name, const Blob& data) const {
    if (!io.write(name, data)) {
      throw Fail{"could not write " + name};
    }
    if (written != nullptr) {
      written->push_back(name);
    }
  }

  // The files a texture's conversion writes, for ConvertIO::remember.
  std::vector<std::string>* written = nullptr;
  struct Writing {
    State& c;
    std::vector<std::string> files;
    explicit Writing(State& owner) : c(owner) { c.written = &files; }
    ~Writing() { c.written = nullptr; }
    Writing(const Writing&) = delete;
    Writing& operator=(const Writing&) = delete;
  };

  // Puts the files an earlier import wrote for `key` in the folder, if its value is `value`.
  bool Reuse(const std::string& key, const std::string& value) {
    std::string was;
    return io.recall && io.relink && io.recall(key, was) && was == value && io.relink(key);
  }

  void Remember(const std::string& key, const std::string& value, const std::vector<std::string>& files) {
    if (io.remember) {
      io.remember(key, value, files);
    }
  }

  static Image Solid(uint8_t r, uint8_t g, uint8_t b) {
    Image img;
    img.width = img.height = 8;
    img.rgba.resize(8 * 8 * 4);
    for (size_t i = 0; i < 64; ++i) {
      img.rgba[i * 4] = r;
      img.rgba[i * 4 + 1] = g;
      img.rgba[i * 4 + 2] = b;
      img.rgba[i * 4 + 3] = 255;
    }
    return img;
  }

  // A material's own reflection cube (RemMaterial::refl), written once as <ID>.envcube:
  // 'MPCB', u32 edge, u32 mips, then RGBA16F, every mip of face 0 from the largest down,
  // then face 1 and so on (GXCreatePBRCube's layout), little endian. The faces are
  // Remastered's, in its world (PortRoomEnv's probes map a direction into it the same way).
  // The colours are what the GPU samples (io.cube decodes by format: most REFL cubes are
  // UNORM ASTC and stay as stored, BC6H keeps its range), a box filter down to kCubeEdge
  // and on to 1x1. 0 when the material has none or the import cannot read cubes.
  static constexpr uint32_t kCubeEdge = 128;
  uint32_t Cube(const MapRef& refl) {
    if (!refl.has || !io.cube) {
      return 0;
    }
    const std::string tag = "cube:" + refl.src;
    const auto known = ids.find(tag);
    if (known != ids.end()) {
      return known->second.value_or(0);
    }
    const uint32_t tid = TexId(tag);
    const std::string key = "tex:" + tag;
    std::string was;
    const bool kept = io.recall && io.recall(key, was) && was == Hex8(tid);
    uint32_t edge = 0;
    std::vector<float> faces;
    const auto read = [&] {
      std::string error;
      if (!io.cube(refl.id, edge, faces, error) || edge == 0 || faces.size() != size_t(edge) * edge * 24) {
        Log("  note: cube " + IdToString(refl.id) + " not read" + (error.empty() ? "" : ": " + error));
        ids[tag] = std::nullopt;
        return false;
      }
      return true;
    };
    if (!kept && !read()) {
      return 0;
    }
    const auto owned = owner.find(tid);
    if (owned != owner.end() && owned->second != tag) {
      throw Fail{"texture id clash on " + Hex8(tid) + ": " + owned->second + " and " + tag};
    }
    owner[tid] = tag;
    ids[tag] = tid;
    if (io.claim && !io.claim(tid)) {
      return tid;
    }
    if (kept && (Reuse(key, Hex8(tid)) || !read())) {
      return ids[tag].value_or(0);
    }
    Writing writing(*this);
    const uint32_t top = std::min(edge, kCubeEdge);
    uint32_t mips = 0;
    while ((top >> mips) != 0) {
      ++mips;
    }
    Blob out;
    const auto le32 = [&](uint32_t v) {
      for (int i = 0; i < 4; ++i) {
        out.push_back(uint8_t(v >> (8 * i)));
      }
    };
    out.insert(out.end(), {'M', 'P', 'C', 'B'});
    le32(top);
    le32(mips);
    for (int face = 0; face < 6; ++face) {
      const float* src = faces.data() + size_t(face) * edge * edge * 4;
      // The top mip: each texel the mean of the block of source texels it covers.
      const uint32_t step = edge / top;
      std::vector<float> level(size_t(top) * top * 4);
      for (uint32_t y = 0; y < top; ++y) {
        for (uint32_t x = 0; x < top; ++x) {
          float sum[3] = {0.0f, 0.0f, 0.0f};
          for (uint32_t sy = 0; sy < step; ++sy) {
            for (uint32_t sx = 0; sx < step; ++sx) {
              const float* p = src + (size_t(y * step + sy) * edge + x * step + sx) * 4;
              for (int c = 0; c < 3; ++c) {
                sum[c] += p[c];
              }
            }
          }
          float* d = level.data() + (size_t(y) * top + x) * 4;
          for (int c = 0; c < 3; ++c) {
            d[c] = sum[c] / float(step * step);
          }
          d[3] = 1.0f;
        }
      }
      for (uint32_t mip = 0; mip < mips; ++mip) {
        const uint32_t e = top >> mip;
        for (float v : level) {
          const uint16_t h = HalfBits(v);
          out.push_back(uint8_t(h));
          out.push_back(uint8_t(h >> 8));
        }
        if (e > 1) {
          std::vector<float> next(size_t(e / 2) * (e / 2) * 4);
          for (uint32_t y = 0; y < e / 2; ++y) {
            for (uint32_t x = 0; x < e / 2; ++x) {
              for (int c = 0; c < 4; ++c) {
                const auto at = [&](uint32_t ix, uint32_t iy) { return level[(size_t(iy) * e + ix) * 4 + c]; };
                next[(size_t(y) * (e / 2) + x) * 4 + c] =
                    0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
              }
            }
          }
          level.swap(next);
        }
      }
    }
    Write(Hex8(tid) + ".envcube", out);
    Remember(key, Hex8(tid), writing.files);
    return tid;
  }

  // A non-negative float as half bits, rounded to nearest and capped at the largest half.
  static uint16_t HalfBits(float v) {
    if (!(v > 0.0f)) {
      return 0;
    }
    if (v >= 65504.0f) {
      return 0x7BFF;
    }
    int e = 0;
    const float m = std::frexp(v, &e);  // v = m * 2^e, m in [0.5, 1)
    if (e < -13) {
      return uint16_t(std::lround(std::ldexp(v, 24)));  // subnormal (rounds up into the normals)
    }
    uint32_t bits = uint32_t(e + 14) << 10;
    const long frac = std::lround((m * 2.0f - 1.0f) * 1024.0f);
    return uint16_t(std::min<uint32_t>(bits + uint32_t(frac), 0x7BFF));
  }

  // The texture id for a role of a Remastered material, converting and writing
  // the texture the first time its tag is seen. `role` is "pbr:<map>" or a TEV
  // slot role; `alpha` is RetailAlpha() of the texture a TEV slot replaces.
  // No value means the retail texture stays.
  std::optional<uint32_t> Get(const std::string& role, const MapRef* rt, std::string alpha,
                              const ConvertOptions& opt) {
    const MapRef* src = nullptr;
    std::string tag;
    int ncap = 0;  // largest edge of the native .dds, 0 for none
    int k = -1;    // the PBR map, on that path
    // A PBR colour texture (base, glow, a shader's ramp) follows its source format, as
    // Remastered's GPU does: an sRGB one is filtered in linear light and written sRGB, so
    // the GPU decodes it; a UNORM one is data, filtered and sampled raw. MR and normal
    // maps are never decoded.
    bool colourTex = false, srgbTex = false;
    if (role.rfind("pbr:", 0) == 0) {
      for (int i = 0; i < kMaps; ++i) {
        if (role.compare(4, std::string::npos, kMapName[i]) == 0) {
          k = i;
        }
      }
      if (rt[k].has) {
        src = &rt[k];
        tag = std::string("pbr:") + kMapName[k] + ":" + src->src + (src->raw ? ":raw" : "") +
              (src->volume ? ":vol" : "");
        if (k == kBase && alpha == "mask" && !src->mask.empty()) {
          tag += ":amask:" + src->mask[0].src;
        }
        colourTex = k != kNormal && (k != kMr || src->raw);
        if (colourTex || k == kNormal) {
          srgbTex = Fact("srgb:" + src->src, [&] { return std::string(Open(*src).srgb ? "1" : "0"); }) == "1";
          tag += srgbTex ? ":srgb" : ":unorm";
        }
        // Named fields, not positional: the size and the alpha follow them.
        if (k == kMr && !src->raw) {
          tag += ":mmax=" + FormatG(src->metalMax);
        }
      } else {
        tag = std::string("const:") + kMapNeutral[k];
      }
    } else if (role == "diffuse" && rt[kBase].has) {
      src = &rt[kBase];
      tag = "base:" + src->src;
    } else if (role == "emissive" && rt[kEmissive].has) {
      src = &rt[kEmissive];
      tag = "emis:" + src->src + ":1";
    } else if (role == "reflect" && rt[kMr].has) {
      src = &rt[kMr];
      tag = "refl:" + src->src + ":0.6";
      // A stage writing C2 is not always a reflectivity mask: the Metroid's
      // dome multiplies its colour by it. A map with no metal anywhere would
      // turn black and blank the stage, so the retail texture stays.
      const std::string any = Fact("metal:" + src->src, [&] {
        const Image& mr = Open(*src);
        for (size_t i = 0; i + 3 < mr.rgba.size(); i += 4) {
          const float g = mr.rgba[i + 1] / 255.0f, b = mr.rgba[i + 2] / 255.0f;
          if (uint8_t(std::clamp(b * (1.0f - 0.7f * g) * 0.6f, 0.0f, 1.0f) * 255.0f) != 0) {
            return std::string("1");
          }
        }
        return std::string("0");
      });
      if (any != "1") {
        return std::nullopt;
      }
    } else if (role == "envmap") {
      // A sphere-mapped reflection: its texture is a gradient, not surface
      // detail, so there is nothing to convert from Remastered.
      return std::nullopt;
    } else if (role == "lightmap") {
      tag = "const:c0c0c0";
    } else {
      tag = "const:000000";
    }
    if (role == "reflect" && alpha == "punch") {
      alpha = "none";  // the reflectivity map's alpha is 0 by construction
    }
    const bool raw = src && src->raw;
    int cap;
    if (k >= 0) {
      // The albedo is what shows and can be CMPR; AO/roughness/metal would
      // blotch and the normals would tilt, so the normal map stays RGBA8.
      // A cutout's base is the exception: its alpha is the shape.
      // A blended surface's base keeps its whole alpha, which is its opacity.
      alpha = k == kNormal || raw ? "rgba"
              : k == kBase && (alpha == "punch" || alpha == "cut" || alpha == "blend" || alpha == "mask" || alpha == "mask2") ? alpha
                                                                                                             : "none";
      cap = kPbrMax[k];
      if (src) {
        ncap = opt.nativeMax > 0 ? std::min(kPbrNative[k], opt.nativeMax) : kPbrNative[k];
        cap = std::min(cap, kPbrStub[k]);
        if (src->volume) {
          ncap = 0;  // every texel exact: the atlas is the TXTR, never a .dds
          cap = 512;
        }
      }
    } else if (alpha == "full") {
      cap = 1024;
    } else {
      cap = opt.maxTexture;
    }
    tag += ":" + std::to_string(cap);
    if (ncap) {
      tag += ":dds" + std::to_string(ncap);
    }
    if (src && alpha != "rgba") {
      tag += ":" + alpha;
    }
    const auto known = ids.find(tag);
    if (known != ids.end()) {
      return known->second;
    }
    const uint32_t tid = TexId(tag);
    const auto owned = owner.find(tid);
    if (owned != owner.end() && owned->second != tag) {
      throw Fail{"texture id clash on " + Hex8(tid) + ": " + owned->second + " and " + tag};
    }
    owner[tid] = tag;
    ids[tag] = tid;
    const std::string name = Hex8(tid);
    if (!src) {
      const unsigned long rgb = std::strtoul(tag.substr(6, 6).c_str(), nullptr, 16);
      Write(name + ".TXTR", EncodeTxtrRgba8(Solid(uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb)), 8));
      return tid;
    }
    // Converted by an earlier import: neither decoded nor encoded again.
    const std::string key = "tex:" + tag;
    bool claimed = false;
    if (std::string was; io.recall && io.recall(key, was)) {
      if (was == "-") {
        ids[tag] = std::nullopt;
        return std::nullopt;
      }
      if (was == name) {
        if (k >= 0 && io.claim && !io.claim(tid)) {
          return tid;
        }
        claimed = k >= 0;
        if (Reuse(key, name)) {
          return tid;
        }
      }
    }
    Writing writing(*this);
    Image img = Open(*src);
    const size_t count = img.rgba.size() / 4;
    // How the image is filtered when it is resized and mipped: base, emissive
    // and the TEV colour slots are sRGB colour, the normal map a direction,
    // and the MR and reflect maps are plain data.
    const MapKind mapKind = k == kNormal                  ? MapKind::Normal
                            : k >= 0                          ? (srgbTex ? MapKind::Colour : MapKind::Data)
                            : role == "reflect"               ? MapKind::Data
                                                              : MapKind::Colour;
    if (k == kNormal) {
      // Two-channel normal maps leave B at 0; the shader rebuilds z. Keep A opaque.
      // A few are stored in an sRGB format (ASTC 8x5), which Remastered's sampler
      // decodes before the shader unpacks them; BC5 has no sRGB form, so decode here.
      for (size_t i = 0; i < count; ++i) {
        if (srgbTex) {
          for (int c = 0; c < 3; ++c) {
            img.rgba[i * 4 + c] = SrgbToLinearByte(img.rgba[i * 4 + c]);
          }
        }
        img.rgba[i * 4 + 3] = 255;
      }
      srgbTex = false;  // decoded: from here on it is data
    }
    if (k == kBase && alpha == "mask" && !src->mask.empty()) {
      // The glow mask is another map's red: it becomes the base's alpha, linear as the
      // shader reads it.
      const Image& m = Open(src->mask[0]);
      const Image t = m.width == img.width && m.height == img.height ? m : Resize(m, img.width, img.height, MapKind::Data);
      for (size_t i = 0; i < count; ++i) {
        img.rgba[i * 4 + 3] = m.srgb ? SrgbToLinearByte(t.rgba[i * 4]) : t.rgba[i * 4];
      }
    }
    if (alpha == "blend" || alpha == "mask2") {
      // Remastered's shader squares the base map's alpha into the opacity, or
      // into the glow mask ("mask2", the port's mask being the alpha as is).
      for (size_t i = 0; i < count; ++i) {
        const unsigned a = img.rgba[i * 4 + 3];
        img.rgba[i * 4 + 3] = uint8_t((a * a + 127) / 255);
      }
    }
    const bool keepAlpha = alpha == "punch" || alpha == "cut" || alpha == "blend" || alpha == "mask" || alpha == "mask2";
    // A PBR map's id is its tag's alone, so whoever wrote it wrote the same file.
    // Asked only now: up to here the tag alone decides whether there is a
    // texture at all (none, a solid, or a failed decode), so every converter
    // that gets this far would write one, and the costly part below runs once.
    if (k >= 0 && !claimed && io.claim && !io.claim(tid)) {
      return tid;
    }
    if (k == kMr && !raw) {
      const uint8_t ceiling = uint8_t(std::nearbyint(float(src->metalMax * 255.0)));
      for (size_t i = 0; i < count; ++i) {
        img.rgba[i * 4 + 2] = std::min(img.rgba[i * 4 + 2], ceiling);
      }
    }
    if (role == "reflect") {
      // glTF order: G roughness, B metal.
      for (size_t i = 0; i < count; ++i) {
        const float g = img.rgba[i * 4 + 1] / 255.0f, b = img.rgba[i * 4 + 2] / 255.0f;
        const uint8_t v = uint8_t(std::clamp(b * (1.0f - 0.7f * g) * 0.6f, 0.0f, 1.0f) * 255.0f);
        img.rgba[i * 4] = img.rgba[i * 4 + 1] = img.rgba[i * 4 + 2] = v;
        img.rgba[i * 4 + 3] = 0;
      }
    }
    if (ncap) {
      const int w = std::max(8, std::min(ncap, NextPow2(img.width)));
      const int h = std::max(8, std::min(ncap, NextPow2(img.height)));
      if (k == kBase && !raw && !keepAlpha) {
        for (size_t i = 0; i < count; ++i) {
          img.rgba[i * 4 + 3] = 255;  // alpha "none", as the stub's CMPR
        }
      }
      // otherwise the stub already holds every texel (an sRGB one is always drawn from its
      // .dds: the stub holds 8-bit linear, which is coarse in the darks)
      // A BC6H colour map (a sky) keeps its range: its .dds is its own blocks, or half floats,
      // never the clamped bytes. BC6H has no alpha, so a map whose alpha comes from another
      // map ("mask") keeps the 8-bit path.
      bool hdrWritten = false;
      if (img.hdr && io.hdrDds && colourTex && !(alpha == "mask" && !src->mask.empty())) {
        std::vector<uint8_t> dds;
        std::string hdrError;
        if (io.hdrDds(src->id, opt.nativeMax, dds, hdrError) && !dds.empty()) {
          Write(name + ".dds", dds);
          hdrWritten = true;
        } else {
          Log("texture " + IdToString(src->id) + ": HDR .dds: " + hdrError + "; writing 8-bit");
        }
      }
      if (!hdrWritten && (srgbTex || std::max(w, h) > cap)) {
        const DdsFormat format = k == kNormal ? NormalDdsFormat() : ColourDdsFormat();
        const bool punch = alpha == "punch";
        if (w == img.width && h == img.height) {
          Write(name + ".dds", EncodeDds(img, format, punch, mapKind, srgbTex));
        } else {
          Write(name + ".dds", EncodeDds(Resize(img, w, h, mapKind), format, punch, mapKind, srgbTex));
        }
      }
    }
    const int w = std::max(8, std::min(cap, NextPow2(img.width)));
    const int h = std::max(8, std::min(cap, NextPow2(img.height)));
    if (w != img.width || h != img.height) {
      img = Resize(img, w, h, mapKind);
    }
    // The stub is sampled raw: an sRGB texture's holds linear bytes, mipped as data.
    MapKind stubKind = mapKind;
    if (srgbTex) {
      for (size_t i = 0; i < img.rgba.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
          img.rgba[i + c] = SrgbToLinearByte(img.rgba[i + c]);
        }
      }
      stubKind = MapKind::Data;
    }
    if (alpha == "none" || alpha == "punch") {
      Write(name + ".TXTR", EncodeTxtrCmpr(img, alpha == "punch", stubKind));
    } else {
      Write(name + ".TXTR", EncodeTxtrRgba8(img, role == "emissive" ? 4 : 8, stubKind));
    }
    Remember(key, name, writing.files);
    return tid;
  }

  void Convert(const Model& model, const ConvertOptions& opt);
};

void Converter::State::Convert(const Model& model, const ConvertOptions& opt) {
  // NModelDataMP1::is_null_model_id (0xfa4830), called before creating the
  // static render proxy (0xfa56c0). These are collision/bounds placeholders,
  // not an invisible shader family. Match the source, including when imported
  // under a generated room-model id; never suppress its separate X-ray model.
  std::string sourceId;
  for (char c : opt.source) {
    if (c != '-') {
      sourceId.push_back(c >= 'A' && c <= 'F' ? char(c + ('a' - 'A')) : c);
    }
  }
  const bool nullModel = sourceId == "100000000000f000f00000003801de98" ||
                         sourceId == "100000000000f000f000000012dbb060" ||
                         sourceId == "100000000000f000f00000001c0b0bef";
  Retail retail;
  if (opt.standalone) {
    // What retail's own opaque lit world materials carry (measured over every
    // retail CMDL: flags 0x1083 and one channel word 0x3001 are the common case).
    retail.nmat = 1;
    retail.flags = 0x4;  // the packed texcoord section is there, empty
    RetailMaterial lit;
    lit.flags = 0x1083;
    lit.vtx = 0xF;  // position and normal; the texcoord slots follow the model
    lit.blendSrc = 1;
    lit.blendDst = 0;
    lit.chans = {0x3001};
    retail.mats.push_back(lit);
    // And the same with retail's alpha test, for a Remastered cutout.
    lit.flags |= 0x20;
    retail.mats.push_back(lit);
    // And as retail's blended surfaces are: drawn in the sorted pass over what
    // is behind them, tested against the depth buffer without writing to it.
    lit.flags = 0x1013;
    lit.blendSrc = 4;  // GX_BL_SRCALPHA, GX_BL_INVSRCALPHA
    lit.blendDst = 5;
    retail.mats.push_back(lit);
    // And glass, whose colour is premultiplied: GX_BL_ONE, GX_BL_INVSRCALPHA.
    lit.blendSrc = 1;
    retail.mats.push_back(lit);
    // And an additive surface (mesh class 3): GX_BL_SRCALPHA, GX_BL_ONE.
    lit.blendSrc = 4;
    lit.blendDst = 1;
    retail.mats.push_back(lit);
  } else {
    if (!io.retail(FourCC('C', 'M', 'D', 'L'), opt.retail, retail.data)) {
      throw Fail{"retail model " + Hex8(opt.retail) + " is not on the disc"};
    }
    retail.Parse();
    if (opt.materialSets > retail.nmat && !retail.sets.empty()) {
      retail.sets.resize(opt.materialSets, retail.sets[0]);
      retail.nmat = opt.materialSets;
    }
  }

  const double (*M)[3] = opt.orient;
  const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
                     M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                     M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);

  double NM[3][3];
  NormalMatrix(M, NM);

  // Primitives in GameCube space; a vertex buffer is shared by many meshes and
  // is transformed once.
  std::vector<RemMaterial> mats;
  for (const ModelMaterial& m : model.materials) {
    mats.push_back(ReadMaterial(m, opt));
    // The wind a material sways with: its index into the model's WIND sets (0xFF none). Only the plain lit
    // foliage (kind 0) of a standalone model, whose vertices can carry the weight in their colour alpha.
    RemMaterial& rem = mats.back();
    if (rem.kind == 45) {
      // 5586D988 VS000 scales source X/Z about the source origin and preserves Y.
      // Conjugate that operation by the actual import transform, including fitted
      // actor transforms; a hardcoded R2G apex would only work for room geometry.
      const double apex[3] = {rem.shieldRows[8], rem.shieldRows[9], rem.shieldRows[10]};
      double alongOffset = 0.0;
      for (int r = 0; r < 3; ++r) alongOffset += NM[r][1] * opt.offset[r];
      double origin[3];
      for (int r = 0; r < 3; ++r) {
        rem.shieldRows[8 + r] = opt.offset[r];
        for (int c = 0; c < 3; ++c) rem.shieldRows[8 + r] += M[r][c] * apex[c];
        rem.layerHeight[r] = M[r][1];
        origin[r] = opt.offset[r] - M[r][1] * alongOffset;
      }
      rem.layerHeight[3] = NM[0][1];
      rem.kindParam[1] = NM[1][1];
      rem.kindParam[2] = NM[2][1];
      rem.kindParam[3] = origin[0];
      rem.kindStrength = origin[1];
      rem.shieldRows[27] = origin[2];
    }
    const size_t at = mats.size() - 1;
    if (opt.standalone && rem.kind == 0 && !rem.unlit && !rem.colorUnlit && !rem.blended && !rem.layered &&
        std::find(std::begin(kShaderWindFoliage), std::end(kShaderWindFoliage), rem.shader) !=
            std::end(kShaderWindFoliage) &&
        at < model.windMaterialSet.size() && model.windMaterialSet[at] < model.windSets.size()) {
      rem.wind = true;
      rem.vcolor = true;
      for (int i = 0; i < 9; ++i) {
        rem.shieldRows[i] = ShortestDouble(model.windSets[model.windMaterialSet[at]][size_t(i)]);
      }
    }
  }
  // How Remastered blends a mesh is its class (the two-bit map after the meshes),
  // not a material flag: 0 opaque, 2 alpha tested, 1 sorted and blended straight
  // (SrcA, InvSrcA), 3 sorted and added (SrcA, One). Every model gives all of a
  // material's meshes the same class, and 1 and 3 only to materials flagged 0x1.
  // An additive surface's alpha is its opacity unless the base alpha masks the glow.
  for (const ModelMesh& mesh : model.meshes) {
    if (mesh.material < mats.size() && mats[mesh.material].blendClass < 0) {
      mats[mesh.material].blendClass = mesh.bits2;
      mats[mesh.material].classBlend = !opt.standalone;
    }
    if (mesh.bits2 == 3 && mesh.material < mats.size()) {
      RemMaterial& m = mats[mesh.material];
      m.additive = true;
      m.blended = m.blended || (!m.mask && !m.cutout);
    }
  }
  // The model's animated UVs: the map names, per mesh, the entry whose transforms move
  // the material's texcoords. Meshes sharing a material are expected to agree; if they
  // don't, the first one's entry is used.
  Anuv anuv;
  AnuvCounts anuvCounts;
  if (!model.anuv.empty()) {
    std::string anuvError;
    if (!ParseAnuv(model.anuv.data(), model.anuv.size(), anuv, anuvError)) {
      Log("  note: ANUV not read: " + anuvError);
    }
    std::vector<int> entryOf(mats.size(), -2);
    for (size_t mi = 0; mi < model.meshes.size() && mi < anuv.matmap.size(); ++mi) {
      const uint32_t mat = model.meshes[mi].material;
      const int entry = anuv.matmap[mi] == 0xff || anuv.matmap[mi] >= anuv.entries.size() ? -1 : anuv.matmap[mi];
      if (mat >= mats.size()) {
        continue;
      }
      if (entryOf[mat] == -2) {
        entryOf[mat] = entry;
      } else if (entryOf[mat] != entry) {
        ++anuvCounts.entryDisagree;
      }
    }
    for (size_t i = 0; i < mats.size(); ++i) {
      if (entryOf[i] >= 0 && anuv.entries[size_t(entryOf[i])].skip == AnuvSkip::None) {
        mats[i].anuv = entryOf[i];
      } else if (entryOf[i] >= 0) {
        ++anuvCounts.notFlattened;
      }
    }
  }
  std::vector<Buffer> buffers(model.vertexBuffers.size());
  std::vector<Prim> prims;
  std::vector<uint32_t> bufOrder;  // buffers in the order the primitives reach them
  // Each level of detail is a set of meshes of its own, so drawing every mesh
  // stacks the coarse copies on the fine one. Keep the chosen level's.
  std::vector<bool> finest(model.meshes.size(), false);
  bool anyFinest = false;
  std::unordered_map<uint64_t, uint32_t> vmshIndex;  // (buffer, vertex) -> its place in vmshV
  std::vector<float> vmshV;                          // pos, nrm, uv, tan, colour per vertex (VMSH v2)
  uint32_t vmshFlags = 3;                            // bit 0 every vertex has a tangent, bit 1 a colour
  std::vector<uint32_t> vmshI;
  const size_t lodBase = size_t(std::max(opt.lod, 0)) * 5;
  if (opt.lod > 0 && lodBase >= model.lods.size()) {
    throw Fail{"the model has no level of detail " + std::to_string(opt.lod)};
  }
  for (size_t r = lodBase; r < lodBase + 5 && r < model.lods.size(); ++r) {
    const ModelLod& range = model.lods[r];
    for (uint64_t i = range.indexOffset; i < uint64_t(range.indexOffset) + range.indexCount; ++i) {
      if (i < model.lodMeshes.size() && model.lodMeshes[i] < finest.size()) {
        finest[model.lodMeshes[i]] = true;
        anyFinest = true;
      }
    }
  }
  for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
    const ModelMesh& mesh = model.meshes[meshIndex];
    if (anyFinest && !finest[meshIndex]) {
      continue;
    }
    if (mesh.material >= mats.size() || mesh.vertexBuffer >= buffers.size()) {
      throw Fail{"a Remastered mesh names a material or vertex buffer that does not exist"};
    }
    const std::string& name = mats[mesh.material].name;
    bool skip = false;
    for (const std::string& s : opt.skip) {
      skip = skip || name.find(s) != std::string::npos;
    }
    if (skip || mats[mesh.material].hidden || (opt.standalone && !mats[mesh.material].maps[kBase].has)) {
      continue;
    }
    Buffer& b = buffers[mesh.vertexBuffer];
    if (!b.loaded) {
      const ModelVertexBuffer& vb = model.vertexBuffers[mesh.vertexBuffer];
      b.loaded = true;
      b.src = &vb;
      b.n = vb.vertexCount;
      if (vb.positions.size() != b.n * 3 || vb.normals.size() != b.n * 3) {
        throw Fail{"a Remastered vertex buffer has no positions or normals"};
      }
      if (vb.uvs.empty() || vb.uvs[0].size() != b.n * 2) {
        throw Fail{"a Remastered vertex buffer has no texture coordinates"};
      }
      b.P.resize(b.n * 3);
      b.N.resize(b.n * 3);
      for (size_t v = 0; v < b.n; ++v) {
        double nrm[3];
        for (int r = 0; r < 3; ++r) {
          double p = 0.0, q = 0.0;
          for (int c = 0; c < 3; ++c) {
            p += double(vb.positions[v * 3 + c]) * M[r][c];
            q += double(vb.normals[v * 3 + c]) * NM[r][c];
          }
          b.P[v * 3 + r] = p + opt.offset[r];
          nrm[r] = q;
        }
        const double len = std::max(std::sqrt((nrm[0] * nrm[0] + nrm[1] * nrm[1]) + nrm[2] * nrm[2]), 1e-9);
        for (int r = 0; r < 3; ++r) {
          b.N[v * 3 + r] = nrm[r] / len;
        }
      }
      if (vb.tangents.size() == b.n * 4) {
        // Moved with the positions; a mirror turns the handedness over.
        b.T.resize(b.n * 4);
        for (size_t v = 0; v < b.n; ++v) {
          double t[3];
          for (int r = 0; r < 3; ++r) {
            t[r] = 0.0;
            for (int c = 0; c < 3; ++c) {
              t[r] += double(vb.tangents[v * 4 + c]) * M[r][c];
            }
          }
          const double len = std::max(std::sqrt((t[0] * t[0] + t[1] * t[1]) + t[2] * t[2]), 1e-9);
          for (int r = 0; r < 3; ++r) {
            b.T[v * 4 + r] = t[r] / len;
          }
          b.T[v * 4 + 3] = (vb.tangents[v * 4 + 3] < 0.0f) != (det < 0.0) ? -1.0 : 1.0;
        }
      }
      // (not vb.Find: the converter tests don't link the CMDL parser)
      const ModelAttribute* second = nullptr;
      for (const ModelAttribute& attribute : vb.attributes) {
        if (attribute.name == "TANGENT_1") {
          second = &attribute;
          break;
        }
      }
      if (second != nullptr && !b.T.empty() && !second->isInteger && second->components == 4 &&
          second->data.size() == b.n * 4) {
        b.T1.resize(b.n * 4);
        for (size_t v = 0; v < b.n; ++v) {
          double t[3];
          for (int r = 0; r < 3; ++r) {
            t[r] = 0.0;
            for (int c = 0; c < 3; ++c) {
              t[r] += double(second->data[v * 4 + c]) * M[r][c];
            }
          }
          const double len = std::max(std::sqrt((t[0] * t[0] + t[1] * t[1]) + t[2] * t[2]), 1e-9);
          for (int r = 0; r < 3; ++r) {
            b.T1[v * 4 + r] = t[r] / len;
          }
          b.T1[v * 4 + 3] = (second->data[v * 4 + 3] < 0.0f) != (det < 0.0) ? -1.0 : 1.0;
        }
      }
      // Indexed by a material's texcoord, which takes two from each attribute
      // (TEXCOORD_n.xy, then its zw): the vertex shaders hand varying 2 + c to texcoord c.
      for (size_t c = 0; c < vb.uvs.size() * 2; ++c) {
        const std::vector<float>* uv = c % 2 == 0 ? &vb.uvs[c / 2] : c / 2 < vb.uvsZw.size() ? &vb.uvsZw[c / 2] : nullptr;
        b.uv.emplace_back(uv && uv->size() == b.n * 2 ? std::vector<double>(uv->begin(), uv->end()) : std::vector<double>());
      }
      b.skinned = vb.joints.size() == b.n * 4 && vb.weights.size() == b.n * 4;
      b.C.assign(b.n * 4, 255);
      if (vb.colors.size() == b.n * 4) {
        for (size_t i = 0; i < b.n * 4; ++i) {
          b.C[i] = uint8_t(std::clamp(vb.colors[i], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        b.colored = true;
      }
    }
    Prim p;
    Prim backPrim;
    p.buffer = mesh.vertexBuffer;
    p.mat = mesh.material;
    p.visor = PortVisorMesh::FromFlags(uint32_t(mesh.unkC) | uint32_t(mesh.unkE) << 16);
    p.I = mesh.indices;
    p.I.resize(p.I.size() / 3 * 3);
    for (uint32_t i : p.I) {
      if (i >= b.src->vertexCount) {
        throw Fail{"a Remastered mesh indexes past its vertex buffer"};
      }
    }
    if (opt.joint >= 0) {
      size_t kept = 0;
      for (size_t t = 0; t + 2 < p.I.size(); t += 3) {
        if (TriangleJoint(*b.src, &p.I[t]) == opt.joint) {
          std::copy_n(p.I.begin() + t, 3, p.I.begin() + kept);
          kept += 3;
        }
      }
      p.I.resize(kept);
      if (p.I.empty()) {
        continue;
      }
    }
    if (det < 0) {
      for (size_t t = 0; t + 2 < p.I.size(); t += 3) {
        std::swap(p.I[t], p.I[t + 2]);
      }
    }
    if (opt.vmsh != nullptr && !b.uv.empty() && b.uv[0].size() >= b.n * 2) {
      // The front faces only, before the back copy below adds vertices.
      for (uint32_t i : p.I) {
        const uint64_t key = uint64_t(mesh.vertexBuffer) << 32 | i;
        auto [at, fresh] = vmshIndex.try_emplace(key, uint32_t(vmshV.size() / 16));
        if (fresh) {
          for (int r = 0; r < 3; ++r) {
            vmshV.push_back(float(b.P[size_t(i) * 3 + r]));
          }
          // The normal and tangent raw (the normal matrix x the buffer's, unnormalised), as Remastered's
          // vertex shader takes them; the colour as its float attribute.
          const ModelVertexBuffer& src = *b.src;
          for (int r = 0; r < 3; ++r) {
            double q = 0.0;
            for (int c = 0; c < 3; ++c) {
              q += double(src.normals[size_t(i) * 3 + c]) * NM[r][c];
            }
            vmshV.push_back(float(q));
          }
          vmshV.push_back(float(b.uv[0][size_t(i) * 2]));
          vmshV.push_back(float(b.uv[0][size_t(i) * 2 + 1]));
          const bool tangent = src.tangents.size() == b.n * 4;
          for (int r = 0; r < 3; ++r) {
            double t = 0.0;
            for (int c = 0; tangent && c < 3; ++c) {
              t += double(src.tangents[size_t(i) * 4 + c]) * M[r][c];
            }
            vmshV.push_back(float(t));
          }
          vmshV.push_back(!tangent ? 0.0f : (src.tangents[size_t(i) * 4 + 3] < 0.0f) != (det < 0.0) ? -1.0f : 1.0f);
          const bool colored = src.colors.size() == b.n * 4;
          for (int c = 0; c < 4; ++c) {
            vmshV.push_back(colored ? src.colors[size_t(i) * 4 + c] : 1.0f);
          }
          vmshFlags &= (tangent ? 1u : 0u) | (colored ? 2u : 0u);
        }
        vmshI.push_back(at->second);
      }
    }
    // A door shield (ColorUnlit, all vertex colour) is one-sided in Remastered, whose door
    // prefab turns its front to the room. The retail shield actors face either way (their
    // model was two-sided), so the port draws it from both sides.
    if (mesh.twoSided || mats[mesh.material].colorUnlit) {
      // Seen from behind too: Remastered draws the mesh with culling off (its MESH
      // chunk's one-bit map; every material flagged 0x400 plus holograms and glow
      // planes), the game culls back faces, so the back is drawn as a copy: each
      // vertex again with its normal turned round, each triangle wound the other way.
      // The back copy always has the normal turned round (what the TEV path lights by). A
      // material with a positive LITS gets it as a primitive of its own (BackLightScale),
      // whose record's F0 factor 0 tells the PBR shader to turn the normal back.
      const bool lits = BackLightScale(mats[mesh.material]);
      if (mats[mesh.material].hasLits && !lits && mats[mesh.material].lits != -1.0 &&
          mats[mesh.material].lits != 1.0) {
        Log("  note: material " + name + " has LITS " + std::to_string(mats[mesh.material].lits) +
            ", drawn as the older back copy");
      }
      std::unordered_map<uint32_t, uint32_t> back;
      for (uint32_t i : p.I) {
        if (back.count(i) != 0) {
          continue;
        }
        back[i] = uint32_t(b.n++);
        for (int r = 0; r < 3; ++r) {
          b.P.push_back(b.P[size_t(i) * 3 + r]);
          b.N.push_back(-b.N[size_t(i) * 3 + r]);
        }
        if (!b.T.empty()) {
          // The normal turns round and B = w * cross(N, T) stays, so w turns with it.
          for (int r = 0; r < 4; ++r) {
            b.T.push_back(r == 3 ? -b.T[size_t(i) * 4 + r] : b.T[size_t(i) * 4 + r]);
          }
        }
        if (!b.T1.empty()) {
          for (int r = 0; r < 4; ++r) {
            b.T1.push_back(r == 3 ? -b.T1[size_t(i) * 4 + r] : b.T1[size_t(i) * 4 + r]);
          }
        }
        for (std::vector<double>& uv : b.uv) {
          if (!uv.empty()) {
            uv.push_back(uv[size_t(i) * 2]);
            uv.push_back(uv[size_t(i) * 2 + 1]);
          }
        }
        for (int c = 0; c < 4; ++c) {
          b.C.push_back(b.C[size_t(i) * 4 + c]);
        }
        b.copyOf.push_back(i);
      }
      const size_t front = p.I.size();
      for (size_t t = 0; t < front; t += 3) {
        p.I.push_back(back[p.I[t + 2]]);
        p.I.push_back(back[p.I[t + 1]]);
        p.I.push_back(back[p.I[t]]);
      }
      if (lits) {
        backPrim.buffer = p.buffer;
        backPrim.mat = p.mat;
        backPrim.litsBack = true;
        backPrim.visor = p.visor;
        backPrim.I.assign(p.I.begin() + front, p.I.end());
        p.I.resize(front);
      }
    }
    if (!b.used) {
      b.used = true;
      bufOrder.push_back(mesh.vertexBuffer);
    }
    prims.push_back(std::move(p));
    if (backPrim.litsBack) {
      prims.push_back(std::move(backPrim));
    }
  }
  if (prims.empty()) {
    throw Fail{"no primitives left"};
  }
  if (opt.vmsh != nullptr) {
    opt.vmsh->clear();
    if (!vmshI.empty()) {
      const uint32_t nv = uint32_t(vmshV.size() / 16);
      const bool wide = nv > 65535;
      auto put = [&](uint32_t v, int bytes) {
        for (int k = bytes - 1; k >= 0; --k) {
          opt.vmsh->push_back(uint8_t(v >> (8 * k)));
        }
      };
      auto putFloat = [&](float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        put(bits, 4);
      };
      put(2, 4);
      put(nv, 4);
      put(uint32_t(vmshI.size() / 3), 4);
      put(vmshFlags, 4);
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
          putFloat(float(M[r][c]));
        }
      }
      for (int r = 0; r < 3; ++r) {
        putFloat(float(opt.offset[r]));
      }
      for (float f : vmshV) {
        putFloat(f);
      }
      for (uint32_t i : vmshI) {
        put(i, wide ? 4 : 2);
      }
    }
  }
  Log(Hex8(opt.retail) + ": " + std::to_string(prims.size()) + " primitives");
  // The levels of detail share their vertex buffers, so a buffer carries the other
  // levels' vertices (and those of skipped meshes) too: keep only what is indexed.
  for (uint32_t bi : bufOrder) {
    Buffer& b = buffers[bi];
    constexpr uint32_t kDropped = UINT32_MAX;
    std::vector<uint32_t> remap(b.n, kDropped);
    for (const Prim& p : prims) {
      if (p.buffer == bi) {
        for (uint32_t i : p.I) {
          remap[i] = 0;
        }
      }
    }
    size_t kept = 0;
    for (size_t v = 0; v < b.n; ++v) {
      if (remap[v] == kDropped) {
        continue;
      }
      remap[v] = uint32_t(kept);
      b.srcOf.push_back(v < b.src->vertexCount ? uint32_t(v) : b.copyOf[v - b.src->vertexCount]);
      std::copy_n(b.P.begin() + v * 3, 3, b.P.begin() + kept * 3);
      std::copy_n(b.N.begin() + v * 3, 3, b.N.begin() + kept * 3);
      if (!b.T.empty()) {
        std::copy_n(b.T.begin() + v * 4, 4, b.T.begin() + kept * 4);
      }
      if (!b.T1.empty()) {
        std::copy_n(b.T1.begin() + v * 4, 4, b.T1.begin() + kept * 4);
      }
      std::copy_n(b.C.begin() + v * 4, 4, b.C.begin() + kept * 4);
      for (std::vector<double>& uv : b.uv) {
        if (!uv.empty()) {
          std::copy_n(uv.begin() + v * 2, 2, uv.begin() + kept * 2);
        }
      }
      ++kept;
    }
    b.n = kept;
    b.P.resize(kept * 3);
    b.N.resize(kept * 3);
    b.T.resize(b.T.empty() ? 0 : kept * 4);
    b.T1.resize(b.T1.empty() ? 0 : kept * 4);
    b.C.resize(kept * 4);
    for (std::vector<double>& uv : b.uv) {
      if (!uv.empty()) {
        uv.resize(kept * 2);
      }
    }
    for (Prim& p : prims) {
      if (p.buffer == bi) {
        for (uint32_t& i : p.I) {
          i = remap[i];
        }
      }
    }
  }

  // Retail material per primitive: forced, or by a vote of the nearest retail
  // vertices among materials of the same kind (blended effect or opaque surface).
  // A retail model's blend and pass come from the Remastered mesh class; what the
  // vote still gives a PBR material is retail's vertex descriptor (a lower bound),
  // lighting flags, channels, texgens with their UV animations, a gun particle's
  // konst alpha, and the TEV fallback's slots. The fx pass picks those from an
  // effect for an effect: without it 53 of 1843 report rows took another material.
  std::vector<double> up;
  std::vector<int> upm;
  for (size_t v = 0; v < retail.Count(); ++v) {
    if (retail.vmat[v] >= 0) {
      up.insert(up.end(), retail.P.begin() + v * 3, retail.P.begin() + v * 3 + 3);
      upm.push_back(retail.vmat[v]);
    }
  }
  for (Prim& p : prims) {
    if (opt.standalone) {
      const RemMaterial& m = mats[p.mat];
      p.rmat = m.kind == 8 || m.kind == 10 || m.kind == 11 || m.kind == 14 || m.kind == 23 || m.kind == 29 || m.kind == 30 || m.kind == 34 || m.kind == 36 || m.kind == 43 ? 3 : m.cutout ? 1 : m.additive ? 4 : m.blended ? 2 : 0;
      continue;
    }
    if (opt.material >= 0) {
      if (size_t(opt.material) >= retail.mats.size()) {
        throw Fail{"the forced retail material does not exist"};
      }
      p.rmat = opt.material;
      continue;
    }
    if (upm.empty()) {
      throw Fail{"the retail model draws nothing"};
    }
    const bool fx = IsFxName(mats[p.mat].name) || mats[p.mat].additive || mats[p.mat].shell || mats[p.mat].shield;
    std::vector<double> cand;
    std::vector<int> candMat;
    for (int pass = 0; pass < 2 && cand.empty(); ++pass) {
      for (size_t i = 0; i < upm.size(); ++i) {
        if (pass == 1 || IsFx(retail.mats[upm[i]]) == fx) {
          cand.insert(cand.end(), up.begin() + i * 3, up.begin() + i * 3 + 3);
          candMat.push_back(upm[i]);
        }
      }
    }
    std::vector<uint32_t> vs = p.I;
    std::sort(vs.begin(), vs.end());
    vs.erase(std::unique(vs.begin(), vs.end()), vs.end());
    if (vs.size() > 4096) {
      std::vector<uint32_t> sample(4096);
      const double step = double(vs.size() - 1) / 4095.0;
      for (size_t i = 0; i < 4096; ++i) {
        sample[i] = vs[i == 4095 ? vs.size() - 1 : size_t(double(i) * step)];
      }
      vs = std::move(sample);
    }
    const Buffer& b = buffers[p.buffer];
    std::vector<double> pts(vs.size() * 3);
    for (size_t i = 0; i < vs.size(); ++i) {
      std::copy(b.P.begin() + size_t(vs[i]) * 3, b.P.begin() + size_t(vs[i]) * 3 + 3, pts.begin() + i * 3);
    }
    std::vector<uint32_t> nn;
    Nearest(pts.data(), vs.size(), cand.data(), candMat.size(), nn);
    std::vector<std::pair<int, size_t>> votes;  // in the order first seen, which decides a tie
    for (uint32_t i : nn) {
      auto it = std::find_if(votes.begin(), votes.end(), [&](const auto& e) { return e.first == candMat[i]; });
      if (it == votes.end()) {
        votes.emplace_back(candMat[i], 1);
      } else {
        ++it->second;
      }
    }
    size_t best = 0;
    for (size_t i = 1; i < votes.size(); ++i) {
      if (votes[i].second > votes[best].second) {
        best = i;
      }
    }
    p.rmat = votes[best].first;
  }

  // Unified vertex list: every distinct buffer once.
  size_t n = 0;
  size_t maxuv = 0;
  bool skinned = true;
  for (uint32_t bi : bufOrder) {
    Buffer& b = buffers[bi];
    b.offset = n;
    n += b.n;
    maxuv = std::max(maxuv, b.uv.size() - 1);
    skinned = skinned && b.skinned;
  }
  // A texcoord slot for every set, so each map keeps the one it was made on, as Remastered
  // samples it: a retail material's own slots stay, and the sets it lacks are added. Bits
  // 8..23 are the eight texcoord slots, two bits each, and 3 is the encoding retail uses.
  for (RetailMaterial& m : retail.mats) {
    for (size_t k = 0; k <= std::min<size_t>(maxuv, 7); ++k) {
      m.vtx |= 3u << (8 + 2 * k);
    }
  }
  // Vertex tangents (the NBT normal section, CMDL flag 0x8) only when every buffer has them.
  bool useTan = !bufOrder.empty();
  for (uint32_t bi : bufOrder) {
    useTan = useTan && !buffers[bi].T.empty();
  }
  // A second tangent frame (flag 0x10) only for the models that draw a two-tangent shader
  // with TANGENT_1 on every buffer those surfaces draw; its other surfaces keep the first nine
  // floats (0x93). Remastered's two-tangent perms all fetch TANGENT_1, so their buffers carry it;
  // the model's other buffers (other shaders, which never read it) take TANGENT_0 in its place.
  auto twoTangent = [&](const Prim& p) {
    for (uint32_t id : kShaderTwoTangent) {
      if (mats[p.mat].family == id) {
        return true;
      }
    }
    return false;
  };
  bool useTan2 = false;
  for (const Prim& p : prims) {
    useTan2 = useTan2 || twoTangent(p);
  }
  for (const Prim& p : prims) {
    useTan2 = useTan2 && useTan && (!twoTangent(p) || !buffers[p.buffer].T1.empty());
  }
  std::vector<double> P, N, T, T1;
  P.reserve(n * 3);
  N.reserve(n * 3);
  for (uint32_t bi : bufOrder) {
    P.insert(P.end(), buffers[bi].P.begin(), buffers[bi].P.end());
    N.insert(N.end(), buffers[bi].N.begin(), buffers[bi].N.end());
    if (useTan) {
      T.insert(T.end(), buffers[bi].T.begin(), buffers[bi].T.end());
    }
    if (useTan2) {
      const std::vector<double>& t1 = buffers[bi].T1.empty() ? buffers[bi].T : buffers[bi].T1;
      T1.insert(T1.end(), t1.begin(), t1.end());
    }
  }
  auto uvSet = [&](size_t i) {
    std::vector<double> out;
    out.reserve(n * 2);
    for (uint32_t bi : bufOrder) {
      const Buffer& b = buffers[bi];
      const std::vector<double>& uv = i < b.uv.size() && !b.uv[i].empty() ? b.uv[i] : b.uv[0];
      out.insert(out.end(), uv.begin(), uv.end());
    }
    return out;
  };
  for (Prim& p : prims) {
    for (uint32_t& i : p.I) {
      i += uint32_t(buffers[p.buffer].offset);
    }
  }
  // Vertex colours, for the materials that read them: tinted by them, or a shader
  // whose kind is drawn from them (the beam glow, the shields), on a retail model as
  // on a room's own; the descriptor the converter writes is its own. The alpha is an
  // opacity only on a blended surface (on an opaque one it weighs the material's
  // layers), so a vertex no blended surface uses is opaque.
  std::vector<uint8_t> C;
  bool useColor = false;
  for (const Prim& p : prims) {
    const bool reads = mats[p.mat].tinted || mats[p.mat].vcolor;
    useColor = useColor || (reads && buffers[p.buffer].colored);
  }
  if (useColor) {
    C.reserve(n * 4);
    for (uint32_t bi : bufOrder) {
      C.insert(C.end(), buffers[bi].C.begin(), buffers[bi].C.end());
    }
    std::vector<bool> keepAlpha(n, false);
    for (const Prim& p : prims) {
      if (((mats[p.mat].blended || mats[p.mat].layered) && mats[p.mat].tinted) || mats[p.mat].vcolor) {
        for (uint32_t i : p.I) {
          keepAlpha[i] = true;
        }
      }
    }
    for (size_t v = 0; v < n; ++v) {
      if (!keepAlpha[v]) {
        C[v * 4 + 3] = 255;
      }
    }
  }

  // Output materials: one per (retail material, Remastered material) pair, in
  // every material set.
  using Key = std::tuple<int, uint32_t, bool>;  // retail material, Remastered material, LITS back
  std::vector<Key> keys;
  for (Prim& p : prims) {
    const Key key(p.rmat, p.mat, p.litsBack);
    auto it = std::find(keys.begin(), keys.end(), key);
    if (it == keys.end()) {
      keys.push_back(key);
      it = keys.end() - 1;
    }
    p.omat = int(it - keys.begin());
  }

  // The texcoord arrays the output carries, each n long. A display list
  // attribute names one by its place in this list.
  std::vector<std::vector<double>> uvArrays;
  std::vector<std::tuple<size_t, bool>> uvKeys;
  auto uvIndex = [&](size_t uvi, const char* role) -> uint32_t {
    const bool squeezed = role && opt.squeeze && opt.squeezeRole == role;
    const std::tuple<size_t, bool> key(uvi, squeezed);
    const auto it = std::find(uvKeys.begin(), uvKeys.end(), key);
    if (it != uvKeys.end()) {
      return uint32_t(it - uvKeys.begin());
    }
    std::vector<double> u = uvSet(uvi);
    if (squeezed) {
      // A deliberate artistic remap, so it runs on the raw coordinates and the
      // result is left alone.
      const double u0 = opt.squeezeFrom[0], u1 = opt.squeezeFrom[1];
      const double lo = opt.squeezeTo[0], hi = opt.squeezeTo[1];
      for (size_t v = 0; v < n; ++v) {
        u[v * 2] = lo + (hi - lo) * std::clamp((u[v * 2] - u0) / (u1 - u0), 0.0, 1.0);
      }
    }
    // Otherwise the coordinates stay as Remastered authored them (U can span
    // -3.9..5.0, or sit wholly outside 0..1): each map samples them with its own
    // wrap modes, which the PBR record carries. Remapping a set onto 0..1 stretched
    // offset or partial tiles (-4.97..-4.77 drew the whole texture).
    uvKeys.push_back(key);
    uvArrays.push_back(std::move(u));
    return uint32_t(uvArrays.size() - 1);
  };

  std::vector<std::vector<Blob>> setBlobs(retail.nmat);
  std::vector<std::vector<uint32_t>> setTex(retail.nmat);
  auto texIndex = [&](size_t si, uint32_t tid) {
    auto it = std::find(setTex[si].begin(), setTex[si].end(), tid);
    if (it == setTex[si].end()) {
      setTex[si].push_back(tid);
      return uint32_t(setTex[si].size() - 1);
    }
    return uint32_t(it - setTex[si].begin());
  };
  std::vector<std::vector<uint32_t>> dlAttrs;  // per output material: the uv array of each texcoord attribute
  std::vector<bool> dlColor;                   // and whether a colour index comes before them
  // The materials report: one row per output material, as it is pushed.
  auto decide = [&](const RemMaterial& rem, uint32_t sourceIndex, const char* path, const std::string& pathReason,
                    const std::string& notes, const std::string& tag, uint32_t cube, const RetailMaterial& pm,
                    const std::string& uv) {
    if (!io.decision) {
      return;
    }
    MaterialDecision d;
    d.cmdl = Hex8(opt.outputModel != 0 ? opt.outputModel : opt.retail);
    d.index = int(dlAttrs.size()) - 1;
    d.source = opt.source;
    d.sourceIndex = int(sourceIndex);
    d.shader = Hex8(rem.shader);
    d.role = rem.role;
    d.flags = rem.flags;
    d.tag = tag;
    d.kind = rem.kind;
    d.mode = PbrMode(rem);
    d.path = path;
    d.pathReason = pathReason;
    d.kindReason = notes + rem.reason;
    d.emissive = rem.emissive;
    d.backlight = rem.backlight;
    d.strength = rem.kindStrength;
    for (int i = 0; i < 4; ++i) {
      d.p[i] = rem.kindParam[i];
    }
    d.cube = cube != 0 ? Hex8(cube) : "-";
    char retailBlend[48];
    std::snprintf(retailBlend, sizeof(retailBlend), "%u,%u 0x%X", pm.blendSrc, pm.blendDst, pm.flags);
    d.retail = retailBlend;
    // The TEV path writes the retail material as it is.
    uint32_t src = pm.blendSrc;
    uint32_t dst = pm.blendDst;
    if (std::strcmp(path, "pbr") == 0) {
      PbrBlend(rem, pm, src, dst);
    }
    d.meshClass = rem.blendClass >= 0 ? std::to_string(rem.blendClass) : "-";
    d.blend = std::to_string(src) + "," + std::to_string(dst);
    uint32_t classSrc = 0;
    uint32_t classDst = 0;
    d.classBlend = !ClassBlend(rem.blendClass, classSrc, classDst) ? "-" : classSrc == src && classDst == dst ? "yes" : "no";
    d.uv = uv;
    io.decision(d);
  };
  for (size_t outputMaterial = 0; outputMaterial < keys.size(); ++outputMaterial) {
    const auto& key = keys[outputMaterial];
    std::string loopNotes;  // what this loop changed on the material, for the report
    const int rmat = std::get<0>(key);
    const RetailMaterial& pm = retail.mats[rmat];
    RemMaterial rem = mats[std::get<1>(key)];
    const size_t lightmapCoord = LightmapSourceCoordinate(rem.family);
    // LMUV is one descriptor shared by every primitive in this output-material
    // group. Emit it only when each such primitive's buffer has the selected
    // channel; otherwise uvSet() would silently substitute UV0 for the missing
    // channel on that buffer.
    bool hasLightmapUv = false;
    if (opt.lightmapUv) {
      bool hasPrimitive = false;
      bool allHaveLightmapUv = true;
      for (const Prim& p : prims) {
        if (p.omat < 0 || size_t(p.omat) != outputMaterial) {
          continue;
        }
        hasPrimitive = true;
        const Buffer& b = buffers[p.buffer];
        allHaveLightmapUv = allHaveLightmapUv && lightmapCoord < b.uv.size() && !b.uv[lightmapCoord].empty();
      }
      hasLightmapUv = hasPrimitive && allHaveLightmapUv;
    }
    if (std::get<2>(key)) {
      rem.lightScale[0] = std::abs(rem.lits);
      rem.lightScale[1] = 0.0;
    }
    // Remastered's missile lock-on highlight is a runtime effect: its map is
    // solid red, so baked in it turns grey shards red.
    if (Lower(rem.name).find("missilelock") != std::string::npos) {
      loopNotes += "missilelock: emissive map dropped; ";
      rem.maps[kEmissive].has = false;
    }
    // The Ice Beam cannon's frost shell (kind 12) needs no vertex colours: its glow mask is
    // the shader's own.
    const bool frostShell = rem.kind == 12;
    // The pause visor's two materials (kinds 36, 37) are retail fx surfaces drawn by their shaders,
    // and only by those: no vertex colour, no second layer.
    const bool inventory = rem.kind == 36 || rem.kind == 37;
    if (rem.kind == 19 || rem.kind == 25 || rem.kind == 42) {
      // Row 6 w (unused by this kind otherwise): the retail konst alpha, a factor of the alpha of a particle model.
      rem.shieldRows[27] = GunFxParticle(pm) ? pm.konstAlpha : 1.0;
    }
    // A retail model's material keeps its kind, macro and second layer as a room's does: its
    // vertices carry the colour and its texcoords every set (above). A second layer is
    // weighed by a kind's shader or by the vertex colour.
    rem.layered = rem.layered && (rem.kind != 0 || (useColor && rem.tinted));
    rem.macroLayered = rem.macroLayered && rem.layered;
    const MapRef* rt = rem.maps;
    // Every set has its slot already (above), the beam glow's noise and the frost shell's
    // glow map (ICAN, texcoord 1) among them.
    uint32_t vtx = pm.vtx;
    size_t ntexattr = 0;
    for (int k = 0; k < 8; ++k) {
      ntexattr += ((vtx >> (8 + 2 * k)) & 3) ? 1 : 0;
    }
    // Bits 4 and 5 are colour 0. Only a material that reads it declares it.
    const bool colored = useColor && (rem.tinted || rem.vcolor);
    rem.tinted = colored && rem.tinted;
    if (colored) {
      vtx |= 3u << 4;
    }
    dlColor.push_back(colored);
    // A retail model's material blends as its mesh class (PbrBlend), so its alpha and shading
    // are the Remastered material's, as on a room's own.
    // A plain lit cutout (no vertex colour) is Remastered's 1-bit alpha shader: full alpha,
    // cut by the shader. Any other keeps the CMPR punch and retail's alpha compare.
    // The glow masks (and the frost shell's, the Vfx shaders', the pickups' and the pause
    // visor's alpha) are the shaders' own, squared where USE_DIFFUSE_AS_INCAN_MASK squares it.
    const bool exactCut = rem.cutout && (rem.kind == 0 || rem.kind == 24) && !rem.unlit && !rem.layered;
    const char* const baseAlpha =
        rem.cutout ? (exactCut ? "cut" : "punch")
        : rem.mask || rem.layered || rem.height > 0.0 || frostShell || inventory || rem.shader == kShaderVfxBase ||
                rem.shader == kShaderVfxBase2 || (rem.kind >= 16 && rem.kind <= 18) || rem.kind == 45
            ? (!frostShell && rem.maskSquared ? "mask2" : "mask")
        : rem.blended ? "blend"
                      : "";
    // Blended retail effects draw as their Remastered shader: every retail fx shader has a port kind.
    const bool usePbr = opt.pbr && rt[kBase].has && Get("pbr:base", rt, baseAlpha, opt).has_value();
    // The base map keeps its whole alpha, and the shader cuts it as Remastered's does.
    rem.cutExact = usePbr && exactCut;
    if (usePbr) {
      ++pbr;
      std::string recordTag;
      // A layered material's base alphas are the two heights the blend compares.
      const int nmaps = rem.macroLayered ? kMacroMaps : rem.layered || rem.macro ? kLayeredMaps : kMaps;
      MapRef both[kMacroMaps];
      std::copy(rt, rt + kMaps, both);
      std::copy(rem.layer, rem.layer + 3, both + kMaps);
      both[kLayeredMaps] = rem.macroMap;
      uint32_t tids[kMacroMaps];
      for (int k = 0; k < nmaps; ++k) {
        const int m = k == kLayeredMaps ? int(kNormal) : k % kMaps;
        if (rem.macro && !rem.layered && (k == 4 || k == 5)) {
          tids[k] = tids[0];
          continue;
        }
        tids[k] = *Get(std::string("pbr:") + kMapName[m], both + (k - m), m == kBase ? baseAlpha : "", opt);
      }
      // Each map keeps the texcoord set it was authored on: the descriptor has a
      // slot for every set up to the eighth. Only a coord past that is folded,
      // onto the base's, or onto 0 if the base's is past it too.
      size_t bset = std::min<size_t>(rt[kBase].coord, maxuv);
      bset = bset < ntexattr ? bset : 0;
      uint32_t coords[kMacroMaps];
      for (int k = 0; k < nmaps; ++k) {
        const size_t c = std::min<size_t>(both[k].has ? both[k].coord : bset, maxuv);
        coords[k] = uint32_t(c < ntexattr ? c : bset);
      }
      uint32_t authored[kMacroMaps];
      for (int k = 0; k < nmaps; ++k) {
        authored[k] = both[k].has ? both[k].authored : 0xFFFFFFFFu;
      }
      const AnuvEntry* entry = rem.anuv >= 0 ? &anuv.entries[size_t(rem.anuv)] : nullptr;
      // Two maps on one source set through different transforms cannot share a
      // texgen, since the texgen's matrix moves every map that reads it. The later
      // one is given a texcoord attribute of its own, a second copy of the set (the
      // descriptor then declares one more), as long as one of the eight is free; if
      // none is, the maps share and the first transform drives both.
      std::vector<uint32_t> attrs(ntexattr, 0xFFFFFFFFu);
      if (entry != nullptr) {
        struct Slot {
          uint32_t source, transform, slot;
        };
        std::vector<Slot> slots;
        bool split = false, starved = false;
        for (int k = 0; k < nmaps; ++k) {
          if (!both[k].has) {
            continue;
          }
          const uint32_t a = authored[k];
          const uint32_t transform = a < 3 && !entry->xf[a].Identity() ? a : 3;  // 3: no motion
          const uint32_t source = coords[k];
          const auto same = std::find_if(slots.begin(), slots.end(), [&](const Slot& o) {
            return o.source == source && o.transform == transform;
          });
          if (same != slots.end()) {
            coords[k] = same->slot;
            continue;
          }
          const bool taken = std::any_of(slots.begin(), slots.end(), [&](const Slot& o) { return o.source == source; });
          uint32_t slot = source;
          if (taken && ntexattr < 8) {
            slot = uint32_t(ntexattr);
            vtx |= 3u << (8 + 2 * ntexattr);
            attrs.push_back(uvIndex(source, nullptr));
            ++ntexattr;
            split = true;
          } else if (taken) {
            starved = true;
          }
          slots.push_back({source, transform, slot});
          coords[k] = slot;
        }
        anuvCounts.slotSplit += split ? 1 : 0;
        anuvCounts.outOfSlots += starved ? 1 : 0;
      }
      for (int k = 0; k < nmaps; ++k) {
        if (attrs[coords[k]] == 0xFFFFFFFFu) {
          attrs[coords[k]] = uvIndex(coords[k], nullptr);
        }
      }
      // Each map's sampler, 2 bits an axis (GX: 0 clamp, 1 repeat, 2 mirror); a
      // map the material lacks, or one without a sampler, repeats.
      uint32_t wrap = 0x55555555u;
      for (int k = 0; k < nmaps; ++k) {
        for (int c = 0; c < 2 && both[k].has; ++c) {
          const int32_t w = both[k].wrap[c];
          const uint32_t shift = uint32_t(k * 4 + c * 2);
          wrap = (wrap & ~(3u << shift)) | (uint32_t(w >= 0 && w <= 2 ? w : 1) << shift);
        }
      }
      const uint32_t zero = uvIndex(0, nullptr);
      for (uint32_t& a : attrs) {
        a = a == 0xFFFFFFFFu ? zero : a;
      }
      // The selected lightmap coordinates ride on one more attribute, if every
      // primitive in this material group has them and a slot is free; the material's
      // 'LMUV' trailer names the slot.
      int lightmapSlot = -1;
      if (opt.lightmapUv && hasLightmapUv && ntexattr < 8) {
        lightmapSlot = int(ntexattr);
        vtx |= 3u << (8 + 2 * ntexattr);
        attrs.push_back(uvIndex(lightmapCoord, nullptr));
        ++ntexattr;
      }
      dlAttrs.push_back(attrs);
      const uint32_t group = 0x40000000u | uint32_t(dlAttrs.size() - 1);
      const uint32_t cube = Cube(rem.refl);
      for (size_t si = 0; si < retail.nmat; ++si) {
        uint32_t idx[kMacroMaps];
        for (int k = 0; k < nmaps; ++k) {
          idx[k] = texIndex(si, tids[k]);
        }
        // This set's swapped maps (ConvertOptions::setTextures), the same map otherwise.
        if (si < opt.setTextures.size() && !opt.setTextures[si].empty()) {
          for (int k = 0; k < nmaps; ++k) {
            const int m = k == kLayeredMaps ? int(kNormal) : k % kMaps;
            const MapRef& ref = both[k];
            const auto swap = ref.has ? opt.setTextures[si].find(ref.id) : opt.setTextures[si].end();
            if (swap == opt.setTextures[si].end()) {
              continue;
            }
            MapRef swapped[kMacroMaps];
            std::copy(both, both + kMacroMaps, swapped);
            swapped[k].id = swap->second;
            swapped[k].src = opt.texturePrefix + IdToString(swap->second) + opt.textureSuffix;
            idx[k] = texIndex(si, *Get(std::string("pbr:") + kMapName[m], swapped + (k - m),
                                       m == kBase ? baseAlpha : "", opt));
          }
        }
        AnuvCounts scratch;  // counted once, not once per material set
        setBlobs[si].push_back(PbrMaterial(pm, vtx, idx, group, coords, rem, wrap, cube, authored, entry,
                                           si == 0 ? anuvCounts : scratch));
        if (si == 0) {
          recordTag.assign(setBlobs[si].back().end() - 4, setBlobs[si].back().end());
        }
        if (lightmapSlot >= 0) {
          P32(setBlobs[si].back(), uint32_t(lightmapSlot));
          setBlobs[si].back().insert(setBlobs[si].back().end(), {'L', 'M', 'U', 'V'});
        }
      }
      std::string uv = std::to_string(ntexattr) + (dlColor.back() ? "c:" : ":");
      for (int k = 0; k < nmaps; ++k) {
        uv += (k ? "," : "") + std::to_string(coords[k]);
      }
      decide(rem, std::get<1>(key), "pbr", "ok", loopNotes, recordTag, cube, pm, uv);
      continue;
    }
    ++tev;
    const std::string tevReason =
        !opt.pbr ? "pbr off"
        : !rt[kBase].has ? "no base map"
                                                   : "the base map did not resolve";
    // The TEV path: the retail material with each texture slot refilled from
    // the Remastered map that matches what its stage does.
    const std::vector<SlotRole> roles = SlotRoles(pm);
    std::vector<uint32_t> attrs(ntexattr, 0xFFFFFFFFu);
    std::map<uint32_t, uint32_t> slotIds;
    // Each slot's sampler, as the PBR record's wrap word (slot i: S in bits 4i..4i+1, T in
    // 4i+2..4i+3): a slot refilled from a Remastered map takes that map's modes; a retail
    // texture, a constant or a slot past 7 repeats, as in retail.
    uint32_t wrap = 0x55555555u;
    for (const SlotRole& r : roles) {
      Role role = r.role;
      if (r.slot >= pm.tex.size() || pm.tex[r.slot] >= retail.sets[0].tex.size()) {
        throw Fail{"a retail material samples a texture it does not list"};
      }
      const std::optional<uint32_t> tid =
          Get(RoleName(role), rt, RetailAlpha(retail.sets[0].tex[pm.tex[r.slot]]), opt);
      if (tid) {
        slotIds[r.slot] = *tid;
      } else {
        role = Role::Keep;
      }
      const int rk = role == Role::Emissive ? kEmissive : role == Role::Reflect || role == Role::EnvMap ? kMr : kBase;
      // The maps Get draws from (a constant has no sampler of its own).
      const bool mapped = (role == Role::Diffuse || role == Role::Emissive || role == Role::Reflect) && rt[rk].has;
      for (int c = 0; c < 2 && r.slot < 8; ++c) {
        const int32_t w = mapped ? rt[rk].wrap[c] : 1;
        const uint32_t shift = r.slot * 4 + uint32_t(c) * 2;
        wrap = (wrap & ~(3u << shift)) | (uint32_t(w >= 0 && w <= 2 ? w : 1) << shift);
      }
      const uint32_t uvi = rt[rk].has ? rt[rk].coord : rt[kBase].has ? rt[kBase].coord : 0;
      if (r.uvSrc >= 0 && size_t(r.uvSrc) < ntexattr && attrs[r.uvSrc] == 0xFFFFFFFFu) {
        attrs[r.uvSrc] = uvIndex(std::min<size_t>(uvi, maxuv), RoleName(role));
      }
    }
    for (uint32_t& a : attrs) {
      if (a == 0xFFFFFFFFu) {
        a = uvIndex(0, nullptr);
      }
    }
    dlAttrs.push_back(attrs);
    decide(rem, std::get<1>(key), "tev", tevReason, loopNotes, wrap != 0x55555555u ? "WRAP" : "TEV", 0, pm,
           std::to_string(ntexattr) + (dlColor.back() ? "c:-" : ":-"));
    for (size_t si = 0; si < retail.nmat; ++si) {
      const MaterialSet& set = retail.sets[si];
      if (size_t(rmat) >= set.mats.size()) {
        throw Fail{"the retail material sets differ in length"};
      }
      Blob blob(set.mats[rmat].p, set.mats[rmat].p + set.mats[rmat].n);
      const RetailMaterial q = ParseMaterial(set.mats[rmat]);
      for (size_t i = 0; i < q.tex.size(); ++i) {
        if (q.tex[i] >= set.tex.size()) {
          throw Fail{"a retail material samples a texture it does not list"};
        }
        const auto it = slotIds.find(uint32_t(i));
        const uint32_t idx = texIndex(si, it != slotIds.end() ? it->second : set.tex[q.tex[i]]);
        for (int k = 0; k < 4; ++k) {
          blob[8 + i * 4 + k] = uint8_t(idx >> (24 - 8 * k));
        }
      }
      if (vtx != q.vtx) {
        // A texcoord slot added above has to reach the material as well: the
        // display list now sends that index.
        const size_t at = 8 + q.tex.size() * 4;
        for (int k = 0; k < 4; ++k) {
          blob[at + k] = uint8_t(vtx >> (24 - 8 * k));
        }
      }
      // Read back from the material's end (PortPbrRecord::Read), so nothing retail parses moves.
      if (wrap != 0x55555555u) {
        P32(blob, wrap);
        blob.insert(blob.end(), {'W', 'R', 'A', 'P'});
      }
      setBlobs[si].push_back(std::move(blob));
    }
  }
  if (uvArrays.empty()) {  // no material samples a texture, but the section must exist
    uvIndex(0, nullptr);
  }

  // Skinning: the weight group of every vertex. A model's skin is not named by
  // the CMDL: the character and its CINF decide, so a model can appear under
  // several skins, and the weights are written under every one whose skeleton
  // has the bones of the one they were mapped against. A replaced CMDL left
  // with retail's CSKR, laid out for retail's vertex count, explodes.
  std::vector<int> weights;       // per vertex: index into weightKeys
  std::vector<WeightKey> weightKeys;
  std::vector<uint32_t> skins;
  if (!opt.skins.empty()) {
    std::vector<uint32_t> sids = opt.skins;
    std::sort(sids.begin(), sids.end());
    sids.erase(std::unique(sids.begin(), sids.end()), sids.end());
    Blob ref;
    if (!io.retail(FourCC('C', 'S', 'K', 'R'), sids[0], ref)) {
      throw Fail{"retail skin " + Hex8(sids[0]) + " is not on the disc"};
    }
    const Span refSpan{ref.data(), ref.size()};
    const std::vector<uint32_t> refBones = SkinBones(refSpan);
    skins.push_back(sids[0]);
    for (size_t i = 1; i < sids.size(); ++i) {
      if (!opt.boneWeights.empty()) {
        // Given weights name the bones of the caller's skeleton, which binds every one of these.
        skins.push_back(sids[i]);
        continue;
      }
      Blob other;
      if (io.retail(FourCC('C', 'S', 'K', 'R'), sids[i], other)) {
        const std::vector<uint32_t> bones = SkinBones(Span{other.data(), other.size()});
        if (std::includes(bones.begin(), bones.end(), refBones.begin(), refBones.end())) {
          skins.push_back(sids[i]);
          continue;
        }
      }
      Log("  note: skin " + Hex8(sids[i]) + " has a different skeleton, not writing weights for it");
    }
    std::vector<WeightKey> perVertex;
    if (!opt.boneWeights.empty()) {
      // Weights chosen by the caller, already on retail bones: written as given.
      std::vector<std::vector<std::pair<uint32_t, double>>> given;
      given.reserve(n);
      for (uint32_t bi : bufOrder) {
        if (bi >= opt.boneWeights.size() || opt.boneWeights[bi].size() < buffers[bi].src->vertexCount) {
          throw Fail{"the given skin weights do not cover vertex buffer " + std::to_string(bi)};
        }
        for (uint32_t v : buffers[bi].srcOf) {
          auto& row = given.emplace_back();
          for (const auto& w : opt.boneWeights[bi][v]) {
            row.emplace_back(w.first, double(w.second));
          }
        }
      }
      perVertex = QuantiseWeights(given);
      Log(Hex8(this->model) + "  weights as given: " + std::to_string(n) + " vertices");
    }
    std::vector<uint16_t> J;
    std::vector<float> W;
    if (skinned && opt.boneWeights.empty()) {
      for (uint32_t bi : bufOrder) {
        const ModelVertexBuffer& src = *buffers[bi].src;
        for (uint32_t v : buffers[bi].srcOf) {
          J.insert(J.end(), src.joints.begin() + size_t(v) * 4, src.joints.begin() + size_t(v) * 4 + 4);
          W.insert(W.end(), src.weights.begin() + size_t(v) * 4, src.weights.begin() + size_t(v) * 4 + 4);
        }
      }
    }
    if (opt.boneWeights.empty()) {
      // The joints' pivots, in the space P is in.
      std::vector<std::array<double, 3>> pivots;
      for (const auto& p : opt.jointPivots) {
        auto& q = pivots.emplace_back();
        for (int a = 0; a < 3; ++a) {
          q[a] = opt.orient[a][0] * p[0] + opt.orient[a][1] * p[1] + opt.orient[a][2] * p[2] + opt.offset[a];
        }
      }
      const bool pivoted = skinned && !pivots.empty() && !opt.retailBones.empty();
      // The triangles (already in P's numbering), so a static model's pieces stay whole.
      std::vector<uint32_t> tris;
      if (!skinned) {
        for (const Prim& p : prims) {
          tris.insert(tris.end(), p.I.begin(), p.I.end());
        }
      }
      Retail voters;
      Blob voterSkin;
      if (opt.weightModel != 0) {
        if (!io.retail(FourCC('C', 'M', 'D', 'L'), opt.weightModel, voters.data) ||
            !io.retail(FourCC('C', 'S', 'K', 'R'), opt.weightSkin, voterSkin)) {
          throw Fail{"retail model " + Hex8(opt.weightModel) + " or skin " + Hex8(opt.weightSkin) +
                     " is not on the disc"};
        }
        voters.Parse();
        Log(Hex8(this->model) + "  weights voted by " + Hex8(opt.weightModel) + "/" + Hex8(opt.weightSkin));
      }
      const bool byVoters = opt.weightModel != 0;
      perVertex = SkinWeights(P, n, skinned ? &J : nullptr, skinned ? &W : nullptr, byVoters ? voters : retail,
                              byVoters ? Span{voterSkin.data(), voterSkin.size()} : refSpan,
                              [this](const std::string& line) { Log(Hex8(this->model) + line); },
                              pivoted ? &pivots : nullptr, pivoted ? &opt.retailBones : nullptr,
                              skinned ? nullptr : &tris);
    }
    std::map<WeightKey, int> intern;
    weights.resize(n);
    for (size_t v = 0; v < n; ++v) {
      const auto it = intern.emplace(perVertex[v], int(weightKeys.size()));
      if (it.second) {
        weightKeys.push_back(perVertex[v]);
      }
      weights[v] = it.first->second;
    }
  }
  const bool hasSkin = !skins.empty();

  // Weld: Remastered splits vertices far more than the written attributes
  // need. Vertices equal in every written value (float32 position, normal, all
  // UV arrays, weight group) become one, so this is lossless; the first of
  // each keeps its place.
  {
    const size_t width = (6 + (useTan ? 4 : 0) + (useTan2 ? 4 : 0) + uvArrays.size() * 2) * 4 + 4 + 4;
    std::unordered_map<std::string, uint32_t> seen;
    seen.reserve(n * 2);
    std::vector<uint32_t> vmap(n), rep;
    std::string keyBytes(width, '\0');
    for (size_t v = 0; v < n; ++v) {
      char* out = keyBytes.data();
      auto put = [&](double d) {
        const float f = float(d);
        std::memcpy(out, &f, 4);
        out += 4;
      };
      for (int c = 0; c < 3; ++c) {
        put(P[v * 3 + c]);
      }
      for (int c = 0; c < 3; ++c) {
        put(N[v * 3 + c]);
      }
      if (useTan) {
        for (int c = 0; c < 4; ++c) {
          put(T[v * 4 + c]);
        }
      }
      if (useTan2) {
        for (int c = 0; c < 4; ++c) {
          put(T1[v * 4 + c]);
        }
      }
      for (const auto& a : uvArrays) {
        put(a[v * 2]);
        put(a[v * 2 + 1]);
      }
      const int32_t wid = hasSkin ? weights[v] : 0;
      std::memcpy(out, &wid, 4);
      if (useColor) {
        std::memcpy(out + 4, &C[v * 4], 4);
      }
      const auto it = seen.emplace(keyBytes, uint32_t(rep.size()));
      if (it.second) {
        rep.push_back(uint32_t(v));
      }
      vmap[v] = it.first->second;
    }
    for (Prim& p : prims) {
      for (uint32_t& i : p.I) {
        i = vmap[i];
      }
    }
    auto take = [&](std::vector<double>& a, size_t per) {
      std::vector<double> out(rep.size() * per);
      for (size_t i = 0; i < rep.size(); ++i) {
        std::copy(a.begin() + size_t(rep[i]) * per, a.begin() + size_t(rep[i]) * per + per, out.begin() + i * per);
      }
      a = std::move(out);
    };
    take(P, 3);
    take(N, 3);
    if (useTan) {
      take(T, 4);
    }
    if (useTan2) {
      take(T1, 4);
    }
    for (auto& a : uvArrays) {
      take(a, 2);
    }
    if (useColor) {
      std::vector<uint8_t> c(rep.size() * 4);
      for (size_t i = 0; i < rep.size(); ++i) {
        std::copy(C.begin() + size_t(rep[i]) * 4, C.begin() + size_t(rep[i]) * 4 + 4, c.begin() + i * 4);
      }
      C = std::move(c);
    }
    if (hasSkin) {
      std::vector<int> w(rep.size());
      for (size_t i = 0; i < rep.size(); ++i) {
        w[i] = weights[rep[i]];
      }
      weights = std::move(w);
    }
    Log("  welded " + std::to_string(n) + " -> " + std::to_string(rep.size()) + " verts");
    n = rep.size();
  }
  const size_t nuv = n * uvArrays.size();
  // Past 16-bit indices, write windowed surfaces (the port's own 'PBIX' base
  // indices).
  const bool big = n >= 0xFFFF || nuv >= 0x10000;

  auto writeCskr = [&](const std::vector<std::pair<const WeightKey*, uint32_t>>& runs, size_t total) {
    const Blob cskr = CskrBytes(runs, total);
    for (uint32_t s : skins) {
      uint32_t id = s;
      for (size_t i = 0; i < opt.outputSkins.size() && i < opt.skins.size(); ++i) {
        if (opt.skins[i] == s) {
          id = opt.outputSkins[i];
        }
      }
      Write(Hex8(id) + ".CSKR", cskr);
    }
  };

  // One NBT entry of the normal section: N, B, T, with B = w * cross(N, T) so the shader can
  // read the handedness back.
  auto putNormal = [&](Blob& out, size_t v) {
    for (int c = 0; c < 3; ++c) {
      PF(out, N[v * 3 + c]);
    }
    if (useTan) {
      const double* nn = &N[v * 3];
      const double* tt = &T[v * 4];
      const double cr[3] = {nn[1] * tt[2] - nn[2] * tt[1], nn[2] * tt[0] - nn[0] * tt[2],
                            nn[0] * tt[1] - nn[1] * tt[0]};
      for (int c = 0; c < 3; ++c) {
        PF(out, cr[c] * tt[3]);
      }
      for (int c = 0; c < 3; ++c) {
        PF(out, tt[c]);
      }
      if (useTan2) {
        const double* t1 = &T1[v * 4];
        const double c1[3] = {nn[1] * t1[2] - nn[2] * t1[1], nn[2] * t1[0] - nn[0] * t1[2],
                              nn[0] * t1[1] - nn[1] * t1[0]};
        for (int c = 0; c < 3; ++c) {
          PF(out, c1[c] * t1[3]);
        }
        for (int c = 0; c < 3; ++c) {
          PF(out, t1[c]);
        }
      }
    }
  };

  std::vector<Blob> surfs;
  Blob secP, secN, secC, secUV;
  double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
  bool haveBounds = false;
  auto bound = [&](const double* p) {
    for (int c = 0; c < 3; ++c) {
      lo[c] = haveBounds ? std::min(lo[c], p[c]) : p[c];
      hi[c] = haveBounds ? std::max(hi[c], p[c]) : p[c];
    }
    haveBounds = true;
  };
  const size_t kDlChunk = 65535 / 3 * 3;
  if (big) {
    // Each surface covers a run of at most `window` vertices (duplicated where
    // windows meet) and stores base indices after its bounds, which the port
    // adds to every display list index. UVs are interleaved per vertex so one
    // base serves every texture attribute. Skin runs are per window, so weight
    // groups may repeat.
    const size_t k = uvArrays.size();
    const size_t window = 0xFFFF / k;
    std::vector<std::pair<const WeightKey*, uint32_t>> runs;
    size_t start = 0;
    std::vector<int> local(n, -1);
    for (const Prim& p : prims) {
      const std::vector<uint32_t>& attrs = dlAttrs[p.omat];
      std::vector<std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> chunks;  // vertices, triangles
      std::vector<uint32_t> verts, cur;
      auto flush = [&]() {
        for (uint32_t v : verts) {
          local[v] = -1;
        }
        chunks.emplace_back(std::move(verts), std::move(cur));
        verts.clear();
        cur.clear();
      };
      for (size_t t = 0; t + 2 < p.I.size(); t += 3) {
        const uint32_t a = p.I[t], b = p.I[t + 1], c = p.I[t + 2];
        size_t fresh = local[a] < 0 ? 1 : 0;
        fresh += local[b] < 0 && b != a ? 1 : 0;
        fresh += local[c] < 0 && c != a && c != b ? 1 : 0;
        if (verts.size() + fresh > window) {
          flush();
        }
        for (uint32_t v : {a, b, c}) {
          if (local[v] < 0) {
            local[v] = int(verts.size());
            verts.push_back(v);
          }
          cur.push_back(v);
        }
      }
      if (!cur.empty()) {
        flush();
      }
      for (auto& chunk : chunks) {
        std::vector<uint32_t>& cv = chunk.first;
        if (hasSkin) {
          std::vector<int> keyOrder(weightKeys.size(), -1);
          std::vector<int> order;
          std::vector<uint32_t> counts;
          for (uint32_t v : cv) {
            if (keyOrder[weights[v]] < 0) {
              keyOrder[weights[v]] = int(order.size());
              order.push_back(weights[v]);
              counts.push_back(0);
            }
            ++counts[keyOrder[weights[v]]];
          }
          std::stable_sort(cv.begin(), cv.end(),
                           [&](uint32_t x, uint32_t y) { return keyOrder[weights[x]] < keyOrder[weights[y]]; });
          for (size_t i = 0; i < order.size(); ++i) {
            runs.emplace_back(&weightKeys[order[i]], counts[i]);
          }
        }
        for (size_t i = 0; i < cv.size(); ++i) {
          local[cv[i]] = int(i);
        }
        double centre[3] = {0, 0, 0}, clo[3], chi[3];
        for (size_t i = 0; i < cv.size(); ++i) {
          const double* pos = &P[size_t(cv[i]) * 3];
          for (int c = 0; c < 3; ++c) {
            PF(secP, pos[c]);
            centre[c] += pos[c];
            clo[c] = i ? std::min(clo[c], pos[c]) : pos[c];
            chi[c] = i ? std::max(chi[c], pos[c]) : pos[c];
          }
          bound(pos);
          putNormal(secN, size_t(cv[i]));
          if (useColor) {
            secC.insert(secC.end(), C.begin() + size_t(cv[i]) * 4, C.begin() + size_t(cv[i]) * 4 + 4);
          }
          for (const auto& a : uvArrays) {
            PF(secUV, a[size_t(cv[i]) * 2]);
            PF(secUV, a[size_t(cv[i]) * 2 + 1]);
          }
        }
        for (double& c : centre) {
          c /= double(cv.size());
        }
        Blob dl;
        const std::vector<uint32_t>& tris = chunk.second;
        for (size_t s = 0; s < tris.size(); s += kDlChunk) {
          const size_t count = std::min(kDlChunk, tris.size() - s);
          P8(dl, useTan2 && twoTangent(p) ? 0x94 : useTan ? 0x93 : 0x90);
          P16(dl, uint32_t(count));
          for (size_t i = s; i < s + count; ++i) {
            const uint32_t j = uint32_t(local[tris[i]]);
            P16(dl, j);
            P16(dl, j);
            if (dlColor[p.omat]) {
              P16(dl, j);
            }
            for (uint32_t a : attrs) {
              P16(dl, uint32_t(j * k + a));
            }
          }
        }
        Pad(dl);
        for (uint32_t v : cv) {
          local[v] = -1;
        }
        Blob s;
        SurfaceHeader(s, centre, uint32_t(p.omat), dl.size(), 64);
        for (double v : clo) {
          PF(s, v);
        }
        for (double v : chi) {
          PF(s, v);
        }
        P32(s, 0x50424958);  // 'PBIX': position, normal, colour, uv, packed uv bases
        P32(s, uint32_t(start));
        P32(s, uint32_t(start));
        P32(s, useColor ? uint32_t(start) : 0);
        P32(s, uint32_t(start * k));
        P32(s, 0);
        P32(s, p.visor);  // disc 0x5c: ConvertSurfaceHeader reads it (0 = no filter bits)
        s.resize(s.size() + 12, 0);
        Pad(s);
        Append(s, dl);
        surfs.push_back(std::move(s));
        start += cv.size();
      }
    }
    if (hasSkin) {
      writeCskr(runs, start);
    }
    Log("  windowed: " + std::to_string(surfs.size()) + " surfaces, " + std::to_string(start) + " verts");
    n = start;
  } else {
    // A skin is a list of weight groups, each covering a run of vertices, so
    // the vertices are ordered by group.
    std::vector<uint32_t> order(n);
    for (size_t v = 0; v < n; ++v) {
      order[v] = uint32_t(v);
    }
    if (hasSkin) {
      std::vector<int> keyOrder(weightKeys.size(), -1);
      std::vector<int> first;
      std::vector<uint32_t> counts;
      for (size_t v = 0; v < n; ++v) {
        if (keyOrder[weights[v]] < 0) {
          keyOrder[weights[v]] = int(first.size());
          first.push_back(weights[v]);
          counts.push_back(0);
        }
        ++counts[keyOrder[weights[v]]];
      }
      std::stable_sort(order.begin(), order.end(),
                       [&](uint32_t x, uint32_t y) { return keyOrder[weights[x]] < keyOrder[weights[y]]; });
      std::vector<std::pair<const WeightKey*, uint32_t>> runs;
      for (size_t i = 0; i < first.size(); ++i) {
        runs.emplace_back(&weightKeys[first[i]], counts[i]);
      }
      writeCskr(runs, n);
    }
    std::vector<uint32_t> remap(n);
    for (size_t i = 0; i < n; ++i) {
      remap[order[i]] = uint32_t(i);
    }
    for (size_t i = 0; i < n; ++i) {
      const double* pos = &P[size_t(order[i]) * 3];
      bound(pos);
      for (int c = 0; c < 3; ++c) {
        PF(secP, pos[c]);
      }
      putNormal(secN, size_t(order[i]));
      if (useColor) {
        secC.insert(secC.end(), C.begin() + size_t(order[i]) * 4, C.begin() + size_t(order[i]) * 4 + 4);
      }
    }
    for (const auto& a : uvArrays) {
      for (size_t i = 0; i < n; ++i) {
        PF(secUV, a[size_t(order[i]) * 2]);
        PF(secUV, a[size_t(order[i]) * 2 + 1]);
      }
    }
    std::vector<uint8_t> mark(n, 0);
    for (const Prim& p : prims) {
      const std::vector<uint32_t>& attrs = dlAttrs[p.omat];
      Blob dl;
      for (size_t s = 0; s < p.I.size(); s += kDlChunk) {
        const size_t count = std::min(kDlChunk, p.I.size() - s);
        P8(dl, useTan2 && twoTangent(p) ? 0x94 : useTan ? 0x93 : 0x90);
        P16(dl, uint32_t(count));
        for (size_t i = s; i < s + count; ++i) {
          const uint32_t j = remap[p.I[i]];
          mark[j] = 1;
          P16(dl, j);
          P16(dl, j);
          if (dlColor[p.omat]) {
            P16(dl, j);
          }
          for (uint32_t a : attrs) {
            P16(dl, uint32_t(j + a * n));
          }
        }
      }
      Pad(dl);
      // The mean of the surface's vertices, each once, in index order.
      double centre[3] = {0, 0, 0};
      double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
      size_t count = 0;
      for (size_t j = 0; j < n; ++j) {
        if (mark[j]) {
          mark[j] = 0;
          const double* pos = &P[size_t(order[j]) * 3];
          for (int c = 0; c < 3; ++c) {
            centre[c] += pos[c];
            lo[c] = std::min(lo[c], pos[c]);
            hi[c] = std::max(hi[c], pos[c]);
          }
          ++count;
        }
      }
      for (double& c : centre) {
        c /= double(count);
      }
      Blob s;
      if (p.visor != 0) {
        // A surface with filter bits needs the extra block that holds them (bounds, no base
        // indices); the others keep the plain header.
        SurfaceHeader(s, centre, uint32_t(p.omat), dl.size(), 64);
        for (double v : lo) {
          PF(s, v);
        }
        for (double v : hi) {
          PF(s, v);
        }
        s.resize(s.size() + 24, 0);  // no PBIX block
        P32(s, p.visor);
        s.resize(s.size() + 12, 0);
      } else {
        SurfaceHeader(s, centre, uint32_t(p.omat), dl.size(), 0);
      }
      Pad(s);
      Append(s, dl);
      surfs.push_back(std::move(s));
    }
  }
  Pad(secP);
  Pad(secN);
  Pad(secC);
  Pad(secUV);

  std::vector<Blob> secs;
  if (nullModel) {
    // Retain bounds and vertex data for actor/collision queries, but no draw
    // surfaces in any material set, mirroring the absent Remastered proxy.
    surfs.clear();
    if (opt.vmsh != nullptr) {
      opt.vmsh->clear();
    }
    Log("  null model: no render proxy (" + opt.source + ")");
  }
  for (size_t si = 0; si < retail.nmat; ++si) {
    Blob b;
    P32(b, uint32_t(setTex[si].size()));
    for (uint32_t t : setTex[si]) {
      P32(b, t);
    }
    P32(b, uint32_t(setBlobs[si].size()));
    uint32_t end = 0;
    for (const Blob& m : setBlobs[si]) {
      end += uint32_t(m.size());
      P32(b, end);
    }
    for (const Blob& m : setBlobs[si]) {
      Append(b, m);
    }
    Pad(b);
    secs.push_back(std::move(b));
  }
  secs.push_back(std::move(secP));
  secs.push_back(std::move(secN));
  secs.push_back(std::move(secC));  // colours
  secs.push_back(std::move(secUV));
  secs.emplace_back();  // packed texcoords
  {
    Blob table;
    P32(table, uint32_t(surfs.size()));
    for (const Blob& s : surfs) {
      P32(table, uint32_t(s.size()));
    }
    Pad(table);
    secs.push_back(std::move(table));
  }
  size_t tris = 0;
  for (const Prim& p : prims) {
    tris += p.I.size() / 3;
  }
  const size_t nsurf = surfs.size();
  for (Blob& s : surfs) {
    secs.push_back(std::move(s));
  }
  Blob out;
  P32(out, 0xDEADBABE);
  P32(out, 2);
  // Float normals, and the packed texcoord section is always written (empty): a retail
  // model without it (the Frigate's B8CB941D) would have the game take that empty
  // section for the surface table.
  P32(out, ((retail.flags | 0x4) & ~uint32_t(0x2)) | (useTan ? 0x8u : 0u) | (useTan2 ? 0x10u : 0u));
  for (double v : lo) {
    PF(out, v);
  }
  for (double v : hi) {
    PF(out, v);
  }
  P32(out, uint32_t(secs.size()));
  P32(out, retail.nmat);
  for (const Blob& s : secs) {
    P32(out, uint32_t(s.size()));
  }
  Pad(out);
  for (const Blob& s : secs) {
    Append(out, s);
  }
  const uint32_t outputModel = opt.outputModel != 0 ? opt.outputModel : opt.retail;
  Write(Hex8(outputModel) + ".CMDL", out);
  if (!model.anuv.empty()) {
    Log("  ANUV: " + std::to_string(anuvCounts.animated) + " materials animated, " +
        std::to_string(anuvCounts.notFlattened) + " not flattened, " + std::to_string(anuvCounts.entryDisagree) +
        " mesh entries disagree, " + std::to_string(anuvCounts.sharedSlots) + " shared slots, " +
        std::to_string(anuvCounts.scrollOverridden) + " scrolls overridden, " + std::to_string(anuvCounts.slotSplit) +
        " texcoord copies, " + std::to_string(anuvCounts.outOfSlots) + " out of texcoords");
  }
  Log("  wrote " + Hex8(outputModel) + ".CMDL: " + std::to_string(n) + " verts, " + std::to_string(tris) + " tris, " +
      std::to_string(keys.size()) + " materials, " + std::to_string(nsurf) + " surfaces");
}

Converter::Converter(ConvertIO io) : m_state(new State) { m_state->io = std::move(io); }

Converter::~Converter() { delete m_state; }

bool Converter::Convert(const Model& model, const ConvertOptions& options, std::string& error) {
  if ((!m_state->io.retail && !options.standalone) || !m_state->io.texture || !m_state->io.write) {
    error = "the converter has no way to read or write";
    return false;
  }
  try {
    m_state->model = options.retail;
    m_state->Convert(model, options);
  } catch (const Fail& f) {
    error = Hex8(options.retail) + ": " + f.what;
    return false;
  } catch (const std::exception& e) {
    error = Hex8(options.retail) + ": " + e.what();
    return false;
  }
  return true;
}

int Converter::PbrMaterials() const { return m_state->pbr; }
int Converter::TevMaterials() const { return m_state->tev; }

} // namespace PortRemastered
