#ifndef _CCOLORELEMENT
#define _CCOLORELEMENT

#include "types.h"

#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Particles/IElement.hpp"

#ifdef TARGET_PC
#include <array>
#include <vector>
#endif

class CCEConstant : public CColorElement {
  CRealElement* x4_r;
  CRealElement* x8_g;
  CRealElement* xc_b;
  CRealElement* x10_a;

public:
  CCEConstant(CRealElement* r, CRealElement* g, CRealElement* b, CRealElement* a);
  ~CCEConstant() override;
  bool GetValue(int frame, CColor& colorOut) const override;
#ifdef TARGET_PC
  bool GetValueHdr(int frame, float out[4]) const override;
#endif
};

class CCEFastConstant : public CColorElement {
  CColor x4_val;
#ifdef TARGET_PC
  float xPortHdr[4];
#endif

public:
  CCEFastConstant(float r, float g, float b, float a);
  ~CCEFastConstant() override;
  bool GetValue(int frame, CColor& colorOut) const override;
#ifdef TARGET_PC
  bool GetValueHdr(int frame, float out[4]) const override;
#endif
};

class CCEFade : public CColorElement {
  CColorElement* x4_a;
  CColorElement* x8_b;
  CRealElement* xc_endFrame;

public:
  CCEFade(CColorElement* a, CColorElement* b, CRealElement* end);
  ~CCEFade() override;
  bool GetValue(int frame, CColor& colorOut) const override;
#ifdef TARGET_PC
  bool GetValueHdr(int frame, float out[4]) const override;
#endif
};

class CCEFadeEnd : public CColorElement {
  CColorElement* x4_a;
  CColorElement* x8_b;
  CRealElement* xc_startFrame;
  CRealElement* x10_endFrame;

public:
  CCEFadeEnd(CColorElement* a, CColorElement* b, CRealElement* start, CRealElement* end);
  ~CCEFadeEnd() override;
  bool GetValue(int frame, CColor& colorOut) const override;
#ifdef TARGET_PC
  bool GetValueHdr(int frame, float out[4]) const override;
#endif
};

class CCETimeChain : public CColorElement {
  CColorElement* x4_a;
  CColorElement* x8_b;
  CIntElement* xc_swFrame;

public:
  CCETimeChain(CColorElement* a, CColorElement* b, CIntElement* c);
  ~CCETimeChain() override;
  bool GetValue(int frame, CColor& colorOut) const override;
#ifdef TARGET_PC
  bool GetValueHdr(int frame, float out[4]) const override;
#endif
};

class CCEPulse : public CColorElement {
  CIntElement* x4_aDuration;
  CIntElement* x8_bDuration;
  CColorElement* xc_aVal;
  CColorElement* x10_bVal;

public:
  CCEPulse(CIntElement* a, CIntElement* b, CColorElement* c, CColorElement* d);
  ~CCEPulse() override;
  bool GetValue(int frame, CColor& colorOut) const override;
#ifdef TARGET_PC
  bool GetValueHdr(int frame, float out[4]) const override;
#endif
};

class CCEKeyframeEmitter : public CColorElement {
  int x4_percent;
  int x8_unk1;
  bool xc_loop;
  bool xd_unk2;
  int x10_loopEnd;
  int x14_loopStart;
  rstl::vector< CColor > x18_keys;
#ifdef TARGET_PC
  std::vector< std::array< float, 4 > > xPortKeys; // the keys as read, unclamped
#endif

public:
  CCEKeyframeEmitter(CInputStream& in);
  ~CCEKeyframeEmitter() override;
  bool GetValue(int frame, CColor& valOut) const override;
#ifdef TARGET_PC
  bool GetValueHdr(int frame, float out[4]) const override;
#endif
};

class CCEParticleColor : public CColorElement {
public:
  ~CCEParticleColor() override {}
  bool GetValue(int frame, CColor& colorOut) const override;
};

#endif // _CCOLORELEMENT
