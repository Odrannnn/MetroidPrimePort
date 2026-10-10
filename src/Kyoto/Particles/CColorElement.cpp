#include "Kyoto/Particles/CColorElement.hpp"

#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Streams/CInputStream.hpp"

#include "rstl/math.hpp"

CCEConstant::CCEConstant(CRealElement* r, CRealElement* g, CRealElement* b, CRealElement* a)
: x4_r(r), x8_g(g), xc_b(b), x10_a(a) {}

CCEConstant::~CCEConstant() {
  delete x4_r;
  delete x8_g;
  delete xc_b;
  delete x10_a;
}

bool CCEConstant::GetValue(int frame, CColor& colorOut) const {
  float r, g, b, a;
  x4_r->GetValue(frame, r);
  r = CMath::Clamp(0.f, r, 1.f);
  x8_g->GetValue(frame, g);
  g = CMath::Clamp(0.f, g, 1.f);
  xc_b->GetValue(frame, b);
  b = CMath::Clamp(0.f, b, 1.f);
  x10_a->GetValue(frame, a);
  a = CMath::Clamp(0.f, a, 1.f);
  colorOut.Set(r, g, b, a);
  return false;
}

CCEFastConstant::CCEFastConstant(const float r, const float g, const float b, const float a) {
   float cr = CMath::Clamp(0.f, r, 1.f);
   float cg = CMath::Clamp(0.f, g, 1.f);
   float cb = CMath::Clamp(0.f, b, 1.f);
   float ca = CMath::Clamp(0.f, a, 1.f);
   x4_val.Set(cr, cg, cb, ca);
#ifdef TARGET_PC
   xPortHdr[0] = r;
   xPortHdr[1] = g;
   xPortHdr[2] = b;
   xPortHdr[3] = a;
#endif
}

CCEFastConstant::~CCEFastConstant() {}

bool CCEFastConstant::GetValue(int frame, CColor& colorOut) const {
  colorOut = x4_val;
  return false;
}

CCEFade::CCEFade(CColorElement* a, CColorElement* b, CRealElement* end)
: x4_a(a), x8_b(b), xc_endFrame(end) {}

CCEFade::~CCEFade() {
  delete x4_a;
  delete x8_b;
  delete xc_endFrame;
}

bool CCEFade::GetValue(int frame, CColor& colorOut) const {
  float c;
  xc_endFrame->GetValue(frame, c);

  float t = static_cast< float >(frame) * (1.f / c);
  if (t >= 1.f) {
    x8_b->GetValue(frame, colorOut);
  } else {
    CColor colA;
    CColor colB;
    x4_a->GetValue(frame, colA);
    x8_b->GetValue(frame, colB);

    float ar, ag, ab, aa;
    float br, bg, bb, ba;
    colA.Get(ar, ag, ab, aa);
    colB.Get(br, bg, bb, ba);

    float nt = 1.f - t;
    colorOut = CColor(ar * nt + br * t, ag * nt + bg * t, ab * nt + bb * t, aa * nt + ba * t);
  }
  return false;
}

CCEFadeEnd::CCEFadeEnd(CColorElement* a, CColorElement* b, CRealElement* start, CRealElement* end)
: x4_a(a), x8_b(b), xc_startFrame(start), x10_endFrame(end) {}

CCEFadeEnd::~CCEFadeEnd() {
  delete x4_a;
  delete x8_b;
  delete xc_startFrame;
  delete x10_endFrame;
}

bool CCEFadeEnd::GetValue(int frame, CColor& colorOut) const {
  float start;
  xc_startFrame->GetValue(frame, start);

  float frameF = static_cast< float >(frame);
  if (frameF < start) {
    x4_a->GetValue(frame, colorOut);
    return false;
  }

  float end;
  x10_endFrame->GetValue(frame, end);

  CColor colA;
  CColor colB;
  x4_a->GetValue(frame, colA);
  x8_b->GetValue(frame, colB);

  float ar, ag, ab, aa;
  float br, bg, bb, ba;
  colA.Get(ar, ag, ab, aa);
  colB.Get(br, bg, bb, ba);

  float t = (frameF - start) / (end - start);
  float nt = 1.f - t;
  colorOut = CColor(ar * nt + br * t, ag * nt + bg * t, ab * nt + bb * t, aa * nt + ba * t);
  return false;
}

CCETimeChain::CCETimeChain(CColorElement* a, CColorElement* b, CIntElement* c)
: x4_a(a), x8_b(b), xc_swFrame(c) {}

CCETimeChain::~CCETimeChain() {
  delete x4_a;
  delete x8_b;
  delete xc_swFrame;
}

bool CCETimeChain::GetValue(int frame, CColor& colorOut) const {
  int v;
  xc_swFrame->GetValue(frame, v);
  if (frame < v) {
    return x4_a->GetValue(frame, colorOut);
  } else {
    return x8_b->GetValue(frame - v, colorOut);
  }
}

CCEPulse::CCEPulse(CIntElement* a, CIntElement* b, CColorElement* c, CColorElement* d)
: x4_aDuration(a), x8_bDuration(b), xc_aVal(c), x10_bVal(d) {}

CCEPulse::~CCEPulse() {
  delete x4_aDuration;
  delete x8_bDuration;
  delete xc_aVal;
  delete x10_bVal;
}

bool CCEPulse::GetValue(int frame, CColor& colorOut) const {
  int a, b;
  x4_aDuration->GetValue(frame, a);
  x8_bDuration->GetValue(frame, b);
  int cv = a + b + 1;
  if (cv < 0) {
    cv = 1;
  }

  if (b >= 1) {
    if (frame % cv > a) {
      x10_bVal->GetValue(frame, colorOut);
    } else {
      xc_aVal->GetValue(frame, colorOut);
    }
  } else {
    xc_aVal->GetValue(frame, colorOut);
  }
  return false;
}

CCEKeyframeEmitter::CCEKeyframeEmitter(CInputStream& in)
: x4_percent(in.ReadLong())
, x8_unk1(in.ReadLong())
, xc_loop(in.ReadBool())
, xd_unk2(in.ReadBool())
, x10_loopEnd(in.ReadLong())
, x14_loopStart(in.ReadLong())
#ifdef TARGET_PC
, x18_keys()
#else
, x18_keys(in)
#endif
{
#ifdef TARGET_PC
  // Same stream layout as rstl::vector< CColor >(in): a count, then four floats per key. The
  // floats are kept unclamped for GetValueHdr.
  const int keyCount = in.ReadLong();
  x18_keys.reserve(keyCount);
  xPortKeys.reserve(keyCount > 0 ? keyCount : 0);
  for (int i = 0; i < keyCount; ++i) {
    std::array< float, 4 > k;
    for (float& c : k) {
      c = in.ReadFloat();
    }
    CColor key;
    key.Set(k[0], k[1], k[2], k[3]); // as CColor(CInputStream&)
    x18_keys.push_back(key);
    xPortKeys.push_back(k);
  }
#endif
  if (x14_loopStart >= x10_loopEnd) {
    x14_loopStart = 0;
  }
}

CCEKeyframeEmitter::~CCEKeyframeEmitter() {}

bool CCEKeyframeEmitter::GetValue(int frame, CColor& valOut) const {
  if (x4_percent == 0) {
    int emitterTime = CParticleGlobals::GetEmitterTime();
    if (xc_loop) {
      if (emitterTime >= x10_loopEnd) {
        emitterTime -= x14_loopStart;
        emitterTime = emitterTime % (x10_loopEnd - x14_loopStart);
        emitterTime += x14_loopStart;
      }
      valOut = x18_keys[emitterTime];
    } else {
      emitterTime = rstl::min_val(emitterTime, x10_loopEnd - 1);
      valOut = x18_keys[emitterTime];
    }
    return false;
  }

  if (CParticleGlobals::GetParticleLifetimePercentage() == 100) {
    valOut = x18_keys[CParticleGlobals::GetParticleLifetimePercentage()];
  } else {
    valOut = CColor::Lerp(x18_keys[CParticleGlobals::GetParticleLifetimePercentage()],
                          x18_keys[CParticleGlobals::GetParticleLifetimePercentage() + 1],
                          CParticleGlobals::GetParticleLifetimePercentageRemainder());
  }
  return false;
}

bool CCEParticleColor::GetValue(int frame, CColor& colorOut) const {
  colorOut = CParticleGlobals::GetCurrentParticle()->x34_color;
  return false;
}

#ifdef TARGET_PC
bool CColorElement::GetValueHdr(int frame, float out[4]) const {
  CColor c;
  const bool ret = GetValue(frame, c);
  c.Get(out[0], out[1], out[2], out[3]);
  return ret;
}

bool CCEConstant::GetValueHdr(int frame, float out[4]) const {
  out[0] = out[1] = out[2] = out[3] = 0.f;
  x4_r->GetValue(frame, out[0]);
  x8_g->GetValue(frame, out[1]);
  xc_b->GetValue(frame, out[2]);
  x10_a->GetValue(frame, out[3]);
  return false;
}

bool CCEFastConstant::GetValueHdr(int, float out[4]) const {
  for (int i = 0; i < 4; ++i) {
    out[i] = xPortHdr[i];
  }
  return false;
}

bool CCEFade::GetValueHdr(int frame, float out[4]) const {
  float c;
  xc_endFrame->GetValue(frame, c);

  float t = static_cast< float >(frame) * (1.f / c);
  if (t >= 1.f) {
    x8_b->GetValueHdr(frame, out);
  } else {
    float a[4], b[4];
    x4_a->GetValueHdr(frame, a);
    x8_b->GetValueHdr(frame, b);
    float nt = 1.f - t;
    for (int i = 0; i < 4; ++i) {
      out[i] = a[i] * nt + b[i] * t;
    }
  }
  return false;
}

bool CCEFadeEnd::GetValueHdr(int frame, float out[4]) const {
  float start;
  xc_startFrame->GetValue(frame, start);

  float frameF = static_cast< float >(frame);
  if (frameF < start) {
    x4_a->GetValueHdr(frame, out);
    return false;
  }

  float end;
  x10_endFrame->GetValue(frame, end);

  float a[4], b[4];
  x4_a->GetValueHdr(frame, a);
  x8_b->GetValueHdr(frame, b);

  float t = (frameF - start) / (end - start);
  float nt = 1.f - t;
  for (int i = 0; i < 4; ++i) {
    out[i] = a[i] * nt + b[i] * t;
  }
  return false;
}

bool CCETimeChain::GetValueHdr(int frame, float out[4]) const {
  int v;
  xc_swFrame->GetValue(frame, v);
  if (frame < v) {
    return x4_a->GetValueHdr(frame, out);
  }
  return x8_b->GetValueHdr(frame - v, out);
}

bool CCEPulse::GetValueHdr(int frame, float out[4]) const {
  int a, b;
  x4_aDuration->GetValue(frame, a);
  x8_bDuration->GetValue(frame, b);
  int cv = a + b + 1;
  if (cv < 0) {
    cv = 1;
  }

  if (b >= 1 && frame % cv > a) {
    x10_bVal->GetValueHdr(frame, out);
  } else {
    xc_aVal->GetValueHdr(frame, out);
  }
  return false;
}

bool CCEKeyframeEmitter::GetValueHdr(int frame, float out[4]) const {
  auto key = [&](int i, float dst[4]) {
    for (int c = 0; c < 4; ++c) {
      dst[c] = xPortKeys[i][c];
    }
  };
  if (x4_percent == 0) {
    int emitterTime = CParticleGlobals::GetEmitterTime();
    if (xc_loop) {
      if (emitterTime >= x10_loopEnd) {
        emitterTime -= x14_loopStart;
        emitterTime = emitterTime % (x10_loopEnd - x14_loopStart);
        emitterTime += x14_loopStart;
      }
    } else {
      emitterTime = rstl::min_val(emitterTime, x10_loopEnd - 1);
    }
    key(emitterTime, out);
    return false;
  }

  const int pct = CParticleGlobals::GetParticleLifetimePercentage();
  if (pct == 100) {
    key(pct, out);
  } else {
    float a[4], b[4];
    key(pct, a);
    key(pct + 1, b);
    const float t = CParticleGlobals::GetParticleLifetimePercentageRemainder();
    const float omt = 1.f - t;
    for (int i = 0; i < 4; ++i) {
      out[i] = omt * a[i] + t * b[i];
    }
  }
  return false;
}
#endif
