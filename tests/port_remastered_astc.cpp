// The importer's ASTC 4x4 .dds writer (EncodeDds with DdsFormat::ASTC4x4):
// the header says DXGI 134 with the whole mip chain, and the blocks decode
// (with astc-encoder's own decoder) back to something close to the input.
// The DDS header is checked by hand: Aurora's reader is internal to its gfx
// library. The format choice is checked last.

#include "port_remastered_image.h"
#include "port_remastered_txtr.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "astcenc.h"
#include "port_test.h"

using namespace PortRemastered;

namespace {

uint32_t Le32(const std::vector<uint8_t>& d, size_t at) {
  return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24;
}

Image Make(int w, int h, int kind) {
  Image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(size_t(w) * size_t(h) * 4);
  std::mt19937 rng(1234);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &img.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
      switch (kind) {
      case 0:  // gradient
      {  // a smooth ramp along the diagonal, the way a colour map varies
        const double t = double(x + y) / double(std::max(1, w + h - 2));
        p[0] = uint8_t(t * 255);
        p[1] = uint8_t(20 + t * 200);
        p[2] = uint8_t(255 - t * 180);
        p[3] = 255;
      }
        break;
      case 1:  // noise
        for (int k = 0; k < 4; ++k) {
          p[k] = uint8_t(rng());
        }
        break;
      case 2:  // solid
        p[0] = 200, p[1] = 100, p[2] = 30, p[3] = 255;
        break;
      default:  // alpha cut-out: a disc
        p[0] = uint8_t(x * 255 / std::max(1, w - 1));
        p[1] = 120;
        p[2] = 60;
        p[3] = (x - w / 2) * (x - w / 2) + (y - h / 2) * (y - h / 2) < (w * w) / 9 ? 255 : 0;
        break;
      }
    }
  }
  return img;
}

// The top level back to RGBA; empty on failure.
std::vector<uint8_t> Decode(const std::vector<uint8_t>& dds, int w, int h) {
  astcenc_config cfg;
  astcenc_context* ctx = nullptr;
  if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FAST, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) !=
          ASTCENC_SUCCESS ||
      astcenc_context_alloc(&cfg, 1, &ctx, nullptr) != ASTCENC_SUCCESS) {
    return {};
  }
  const int bw = std::max(w, 4), bh = std::max(h, 4);
  std::vector<uint8_t> out(size_t(bw) * size_t(bh) * 4);
  void* slice = out.data();
  astcenc_image img{unsigned(bw), unsigned(bh), 1, ASTCENC_TYPE_U8, &slice};
  astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  const size_t blocks = size_t(bw / 4) * size_t(bh / 4) * 16;
  const bool ok = 148 + blocks <= dds.size() &&
                  astcenc_decompress_image(ctx, &dds[148], blocks, &img, &swz, 0) == ASTCENC_SUCCESS;
  astcenc_context_free(ctx);
  return ok ? out : std::vector<uint8_t>();
}

double Psnr(const Image& a, const std::vector<uint8_t>& b, bool normal) {
  double sum = 0;
  size_t n = 0;
  for (size_t i = 0; i < size_t(a.width) * size_t(a.height); ++i) {
    // Where the alpha is 0 the colour is hidden (and the importer bleeds it).
    const bool hidden = !normal && a.rgba[i * 4 + 3] == 0;
    for (int k = 0; k < (normal ? 2 : 4); ++k) {
      if (hidden && k < 3) {
        continue;
      }
      const double d = double(a.rgba[i * 4 + size_t(k)]) - double(b[i * 4 + size_t(k)]);
      sum += d * d;
      ++n;
    }
  }
  return sum == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (sum / double(n)));
}

// Windows has no setenv/unsetenv; an empty _putenv_s removes the variable.
void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void UnsetEnv(const char* name) {
#ifdef _WIN32
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

}  // namespace

int main() {
  const int sizes[][2] = {{4, 4}, {16, 16}, {64, 64}, {64, 32}, {8, 4}};
  const char* kinds[] = {"gradient", "noise", "solid", "cutout"};
  const double bounds[] = {35.0, 12.0, 45.0, 20.0};  // noise only has to be sane
  for (const auto& sz : sizes) {
    for (int kind = 0; kind < 4; ++kind) {
      const Image img = Make(sz[0], sz[1], kind);
      const std::vector<uint8_t> dds = EncodeDds(img, DdsFormat::ASTC4x4, kind == 3);
      char what[128];
      CHECK_MSG(dds.size() > 148 && std::memcmp(dds.data(), "DDS ", 4) == 0, "dds magic");
      std::snprintf(what, sizeof(what), "%s %dx%d: DXGI format 134", kinds[kind], sz[0], sz[1]);
      CHECK_MSG(dds.size() > 132 && Le32(dds, 128) == 134, what);
      int levels = 1;
      for (int s = std::max(sz[0], sz[1]); s > 1; s >>= 1) {
        ++levels;
      }
      std::snprintf(what, sizeof(what), "%s %dx%d: %d mips", kinds[kind], sz[0], sz[1], levels);
      CHECK_MSG(Le32(dds, 28) == uint32_t(levels), what);
      size_t expect = 148;
      for (int w = sz[0], h = sz[1];; w = std::max(1, w / 2), h = std::max(1, h / 2)) {
        expect += size_t(std::max(w, 4) / 4) * size_t(std::max(h, 4) / 4) * 16;
        if (w == 1 && h == 1) {
          break;
        }
      }
      std::snprintf(what, sizeof(what), "%s %dx%d: size %zu is %zu", kinds[kind], sz[0], sz[1], dds.size(), expect);
      CHECK_MSG(dds.size() == expect, what);
      const std::vector<uint8_t> back = Decode(dds, sz[0], sz[1]);
      CHECK_MSG(!back.empty(), "decode");
      if (!back.empty()) {
        const double psnr = Psnr(img, back, false);
        std::printf("%-8s %2dx%-2d  %.1f dB\n", kinds[kind], sz[0], sz[1], psnr);
        std::snprintf(what, sizeof(what), "%s %dx%d: PSNR %.1f under %.0f", kinds[kind], sz[0], sz[1], psnr,
                      bounds[kind]);
        CHECK_MSG(psnr > bounds[kind], what);
      }
    }
  }

  // A normal map keeps x and y and writes blue 0, alpha 255.
  const Image gradient = Make(32, 32, 0);
  const std::vector<uint8_t> nrm = EncodeDds(gradient, DdsFormat::ASTC4x4Normal);
  const std::vector<uint8_t> back = Decode(nrm, 32, 32);
  CHECK_MSG(!back.empty() && Psnr(gradient, back, true) > 35.0, "normal map RG PSNR");
  bool zb = !back.empty();
  for (size_t i = 0; zb && i < 32 * 32; ++i) {
    zb = back[i * 4 + 2] <= 1 && back[i * 4 + 3] >= 254;
  }
  CHECK_MSG(zb, "normal map blue 0 and alpha 255");

  // DecodeDds reads the top mip of what EncodeDds writes, BC7 and ASTC, unorm and sRGB.
  {
    const Image source = Make(64, 32, 0);
    for (const DdsFormat format : {DdsFormat::BC7, DdsFormat::ASTC4x4}) {
      for (const bool srgb : {false, true}) {
        const std::vector<uint8_t> dds = EncodeDds(source, format, false, MapKind::Data, srgb);
        PortRemastered::TxtrImage decoded;
        std::string error;
        const bool ok = PortRemastered::DecodeDds(dds.data(), dds.size(), decoded, error);
        char what[128];
        std::snprintf(what, sizeof(what), "DecodeDds %s%s: %s", format == DdsFormat::BC7 ? "BC7" : "ASTC",
                      srgb ? " sRGB" : "", error.c_str());
        CHECK_MSG(ok && decoded.width == 64 && decoded.height == 32 && decoded.srgb == srgb &&
                      decoded.rgba.size() == 64 * 32 * 4 && Psnr(source, decoded.rgba, false) > 35.0,
                  what);
        PortRemastered::TxtrImage cut;
        CHECK_MSG(!PortRemastered::DecodeDds(dds.data(), 148 + 16, cut, error), "DecodeDds: a short file fails");
      }
    }
  }

  // Format choice: BC with no GPU, ASTC when only ASTC is there, the env wins.
  UnsetEnv("MP_REMASTERED_TEXTURE_FORMAT");
  CHECK_MSG(ColourDdsFormat() == DdsFormat::BC7 && NormalDdsFormat() == DdsFormat::BC5, "default is BC");
  SetGpuTextureSupport(false, true);
  CHECK_MSG(ColourDdsFormat() == DdsFormat::ASTC4x4 && NormalDdsFormat() == DdsFormat::ASTC4x4Normal, "ASTC-only GPU");
  SetGpuTextureSupport(true, true);
  CHECK_MSG(ColourDdsFormat() == DdsFormat::BC7, "BC wins when both");
  SetEnv("MP_REMASTERED_TEXTURE_FORMAT", "astc");
  CHECK_MSG(ColourDdsFormat() == DdsFormat::ASTC4x4, "env astc");
  SetEnv("MP_REMASTERED_TEXTURE_FORMAT", "bc");
  SetGpuTextureSupport(false, true);
  CHECK_MSG(ColourDdsFormat() == DdsFormat::BC7, "env bc");

  if (port_test::Failures() == 0) {
    std::printf("port_remastered_astc_tests: ok\n");
  }
  return PORT_TEST_RESULT();
}
