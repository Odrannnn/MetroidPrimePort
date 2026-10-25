#include "port_dread_material.h"

#include "port_bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PortDread {
namespace {

using PortRemastered::Image;
using PortRemastered::ModelMaterialData;
using PortRemastered::ModelUuid;

uint64_t Fnv1a(uint64_t h, const void* data, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h;
}

uint8_t ToByte(double v) {
  return uint8_t(std::clamp(std::lround(v), 0l, 255l));
}

double SrgbEncode(double l) {
  l = std::clamp(l, 0.0, 1.0);
  return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

// The uniform's first `n` floats, or `fallback` for a missing or short one.
double Uniform1(const Bsmat& m, const char* name, size_t index, double fallback) {
  const BsmatUniform* u = m.FindUniform(name);
  return u != nullptr && u->type == 'f' && index < u->f.size() ? double(u->f[index]) : fallback;
}

// BSMAT wrap -> the model's sampler wrap (0 clamp, 1 repeat, 2 mirror); repeat when unknown.
int32_t ModelWrap(uint32_t bsmatWrap) {
  switch (bsmatWrap) {
  case kWrapClamp:
  case kWrapClampColor:
    return 0;
  case kWrapMirror:
    return 2;
  default:
    return 1;
  }
}

bool Decode(const Bctex& tex, const char* what, Image& out, std::string* error) {
  std::vector<uint8_t> rgba;
  std::string err;
  if (!tex.Valid() || !tex.DecodeRgba8(0, rgba, err)) {
    if (error != nullptr) {
      *error = std::string("suit material: ") + what + " texture: " + (err.empty() ? "not parsed" : err);
    }
    return false;
  }
  out.width = int(tex.MipWidth(0));
  out.height = int(tex.MipHeight(0));
  out.rgba = std::move(rgba);
  if (out.rgba.size() != size_t(out.width) * size_t(out.height) * 4) {
    if (error != nullptr) {
      *error = std::string("suit material: ") + what + " texture: decoded size mismatch";
    }
    return false;
  }
  return true;
}

ModelMaterialData TextureData(uint32_t usage, const ModelUuid& pakId, int32_t wrapU, int32_t wrapV) {
  ModelMaterialData d;
  d.usage = usage;
  d.kind = ModelMaterialData::Kind::Texture;
  d.texture.usage = usage;
  d.texture.id = ToModelOrder(pakId);
  d.texture.hasUsage = true;
  d.texture.texCoord = 0;
  d.texture.wrapX = wrapU;
  d.texture.wrapY = wrapV;
  d.texture.wrapZ = wrapU;
  return d;
}

ModelMaterialData ColorData(uint32_t usage, float r, float g, float b, float a) {
  ModelMaterialData d;
  d.usage = usage;
  d.kind = ModelMaterialData::Kind::Color;
  d.color[0] = r, d.color[1] = g, d.color[2] = b, d.color[3] = a;
  return d;
}

} // namespace

ModelUuid SuitTextureId(const std::string& materialName, SuitSlot slot) {
  ModelUuid id{};
  const uint32_t s = uint32_t(slot);
  for (int half = 0; half < 2; ++half) {
    uint64_t h = 0xCBF29CE484222325ull ^ (0x9E3779B97F4A7C15ull * uint64_t(half + 1));
    h = Fnv1a(h, "dread-suit", 10);
    h = Fnv1a(h, materialName.data(), materialName.size());
    h = Fnv1a(h, &s, sizeof(s));
    for (int i = 0; i < 8; ++i) {
      id[size_t(half) * 8 + size_t(i)] = uint8_t(h >> (8 * i));
    }
  }
  bool zero = true;
  for (uint8_t b : id) {
    zero = zero && b == 0;
  }
  if (zero) {
    id[15] = uint8_t(1 + s);
  }
  return id;
}

ModelUuid ToModelOrder(const ModelUuid& pakOrder) {
  // ReadMaterial's swap: groups of 4, 2 and 2 bytes reversed (an involution).
  ModelUuid id = pakOrder;
  std::swap(id[0], id[3]);
  std::swap(id[1], id[2]);
  std::swap(id[4], id[5]);
  std::swap(id[6], id[7]);
  return id;
}

void SuitEmissive(const Bsmat& bsmat, const float* emissiveColor, double out[3]) {
  const bool constant = bsmat.shader_path == "system/shd/mp_opaque_constant_selfilum.bshdat";
  const double dimmer = Uniform1(bsmat, "vConstant4", 0, 1.0);
  const double factor = Uniform1(bsmat, "fAlbedoEmissiveFactor", 0, 0.0);
  // The shader's self-illumination is
  //   bc.a * (vAlbedoEmissiveColor.rgb * fAlbedoEmissiveFactor * vConstant4.r + vConstant3 + vConstant5)
  // plus a rim override (vConstant2, vConstant1; zero in every suit BSMAT, runtime driven) and a
  // tail that scales only the part above 1 by the global g_EnvMapMultiplier.z. Those are runtime
  // state, not material data, so they are ignored here: C is the base term, vC3 and vC5 only.
  for (int i = 0; i < 3; ++i) {
    const double colour =
        emissiveColor != nullptr ? emissiveColor[i] : Uniform1(bsmat, "vAlbedoEmissiveColor", size_t(i), 0.0);
    out[i] = colour * factor * dimmer + Uniform1(bsmat, "vConstant3", size_t(i), 0.0) +
             Uniform1(bsmat, "vConstant5", size_t(i), 0.0);
    if (constant) {
      out[i] = factor * Uniform1(bsmat, "vAlbedoEmissiveColorMultiplier", size_t(i), 0.0) *
               Uniform1(bsmat, "vConstant0", size_t(i), 1.0);
    }
  }
}

bool BuildSuitMaterial(const Bsmat& bsmat, const Bctex& bc, const Bctex& at, const Bctex& nm,
                       SuitMaterial& out, std::string* error, const SuitOptions& options) {
  out = SuitMaterial{};
  const bool constant = bsmat.shader_path == "system/shd/mp_opaque_constant_selfilum.bshdat";
  if (!constant && bsmat.shader_path != "system/shd/mp_samuspbr.bshdat") {
    if (error != nullptr) {
      *error = "suit material: unsupported shader '" + bsmat.shader_path + "'";
    }
    return false;
  }
  if (constant) {
    const BsmatUniform* tint = bsmat.FindUniform("iTintAlbedo");
    if (tint == nullptr || tint->type != 'i' || tint->i.size() != 1 || tint->i[0] != 0 ||
        Uniform1(bsmat, "vConstant2", 3, 0) != 0) {
      if (error) *error = "suit material: constant selfillum requires default tint/frostbite";
      return false;
    }
  }
  Image bcImg, atImg, nmImg;
  if (!Decode(bc, "base colour", bcImg, error) || !Decode(at, "attributes", atImg, error) ||
      !Decode(nm, "normal", nmImg, error)) {
    return false;
  }

  // Uniform values; the defaults are the shader's neutral ones (kb: common values per material).
  const double albedoMul = constant ? 1.0 : Uniform1(bsmat, "vConstant0", 3, 1.0);
  double c[3];
  SuitEmissive(bsmat, options.emissiveColor, c);
  for (int i = 0; i < 3; ++i) {
    out.emissiveLinear[i] = float(c[i]);
  }
  const double s = options.strength > 0.0 ? options.strength : std::max({c[0], c[1], c[2]});
  const bool emissive = s > 0.0;

  out.name = bsmat.name;
  out.cullFront = bsmat.cull_mode == kCullFront;

  // BCLR: bc.rgb x vConstant0.w, alpha 255 (the alpha is the emissive mask, moved to ICAN).
  Image bclr = bcImg;
  bclr.srgb = true;
  for (size_t i = 0; i + 3 < bclr.rgba.size(); i += 4) {
    for (int k = 0; k < 3; ++k) {
      bclr.rgba[i + size_t(k)] = ToByte(double(bcImg.rgba[i + size_t(k)]) * albedoMul);
    }
    bclr.rgba[i + 3] = 255;
  }
  // METL (port ORM layout): R = AO (at.a), G = roughness (at.g floored at 0.1, as the shader
  // does), B = metal (at.b).
  Image metl;
  metl.width = atImg.width, metl.height = atImg.height;
  metl.rgba.resize(atImg.rgba.size());
  const uint8_t roughFloor = ToByte((constant ? 0.05 : 0.1) * 255.0);
  for (size_t i = 0; i + 3 < atImg.rgba.size(); i += 4) {
    metl.rgba[i + 0] = atImg.rgba[i + 3];
    metl.rgba[i + 1] = std::max(atImg.rgba[i + 1], roughFloor);
    metl.rgba[i + 2] = atImg.rgba[i + 2];
    metl.rgba[i + 3] = 255;
  }
  // NMAP: the BC5 pair, B 0.
  Image nmap;
  nmap.width = nmImg.width, nmap.height = nmImg.height;
  nmap.rgba.resize(nmImg.rgba.size());
  for (size_t i = 0; i + 3 < nmImg.rgba.size(); i += 4) {
    nmap.rgba[i + 0] = nmImg.rgba[i + 0];
    nmap.rgba[i + 1] = nmImg.rgba[i + 1];
    nmap.rgba[i + 2] = 0;
    nmap.rgba[i + 3] = 255;
  }

  const BsmatSampler* ws = bsmat.FindSampler("texBaseColor");
  const int32_t wu = ws != nullptr ? ModelWrap(ws->wrap_u) : 1;
  const int32_t wv = ws != nullptr ? ModelWrap(ws->wrap_v) : 1;

  using PortRemastered::ModelMaterial;
  ModelMaterial& m = out.material;
  m.name = bsmat.name;
  m.shaderId[0] = uint8_t(kSuitShaderId >> 24);
  m.shaderId[1] = uint8_t(kSuitShaderId >> 16);
  m.shaderId[2] = uint8_t(kSuitShaderId >> 8);
  m.shaderId[3] = uint8_t(kSuitShaderId);
  m.unk1 = 0;
  m.types = {port::FourCC('R', 'L', 'T', 'G')};

  auto add = [&](SuitSlot slot, uint32_t usage, Image&& img) {
    SuitImage si;
    si.id = SuitTextureId(slot == kSuitIcan ? bsmat.name + options.icanKey : bsmat.name, slot);
    si.image = std::move(img);
    m.data.push_back(TextureData(usage, si.id, wu, wv));
    out.images.push_back(std::move(si));
  };
  add(kSuitBclr, port::FourCC('B', 'C', 'L', 'R'), std::move(bclr));
  add(kSuitMetl, port::FourCC('M', 'E', 'T', 'L'), std::move(metl));
  add(kSuitNmap, port::FourCC('N', 'M', 'A', 'P'), std::move(nmap));
  // REFL: Remastered's default (black) cube, which every bfb300b6 material carries and which keeps
  // the room's probe. Without it the converter marks the surface as reflecting nothing (no env
  // specular), and the metal armour plates (at.b ~1, no diffuse) drew black.
  ModelUuid refl{};
  refl[0] = 0x7B, refl[1] = 0x98, refl[2] = 0x17, refl[3] = 0x0F;
  m.data.push_back(TextureData(port::FourCC('R', 'E', 'F', 'L'), ToModelOrder(refl), wu, wv));
  if (emissive) {
    // ICAN: bc.a (the mask) x the colour normalised by its peak, sRGB-encoded (the converter
    // reads the emissive map as colour); ICNC carries the peak back as the strength.
    Image ican;
    ican.width = bcImg.width, ican.height = bcImg.height;
    ican.srgb = true;
    ican.rgba.resize(bcImg.rgba.size());
    for (size_t i = 0; i + 3 < bcImg.rgba.size(); i += 4) {
      const double alpha = double(bcImg.rgba[i + 3]) / 255.0;
      const double a = constant ? std::pow(alpha, 2.2) : alpha;
      for (int k = 0; k < 3; ++k) {
        ican.rgba[i + size_t(k)] = ToByte(255.0 * SrgbEncode(a * std::clamp(c[k], 0.0, s) / s));
      }
      ican.rgba[i + 3] = 255;
    }
    add(kSuitIcan, port::FourCC('I', 'C', 'A', 'N'), std::move(ican));
  }
  m.data.push_back(ColorData(port::FourCC('D', 'I', 'F', 'C'), 1, 1, 1, 1));
  m.data.push_back(ColorData(port::FourCC('I', 'C', 'N', 'C'), float(s), float(s), float(s), 1));
  m.data.push_back(ColorData(port::FourCC('I', 'C', 'M', 'C'), 0, 0, 0, 0));
  return true;
}

bool SuitTexturePaths(const Bsmat& bsmat, const std::string& texturesRoot, std::string& bc,
                      std::string& at, std::string& nm, std::string* error) {
  const struct {
    const char* sampler;
    std::string* path;
  } want[] = {{"texBaseColor", &bc}, {"texAttributes", &at}, {"texNormals", &nm}};
  for (const auto& w : want) {
    const BsmatSampler* s = bsmat.FindSampler(w.sampler);
    if (s == nullptr || s->filepath.empty()) {
      if (error != nullptr) {
        *error = std::string("suit material: BSMAT '") + bsmat.name + "' has no " + w.sampler + " path";
      }
      return false;
    }
    std::string root = texturesRoot;
    if (!root.empty() && root.back() != '/' && root.back() != '\\') {
      root += '/';
    }
    *w.path = root + s->filepath;
  }
  return true;
}

bool FindTexture(const std::vector<SuitMaterial>& materials, const ModelUuid& pakOrderId, Image& out) {
  for (const SuitMaterial& m : materials) {
    for (const SuitImage& si : m.images) {
      if (si.id == pakOrderId) {
        out = si.image;
        return true;
      }
    }
  }
  return false;
}

} // namespace PortDread
