#ifndef _CLINEEXTRASPACINGINSTRUCTION
#define _CLINEEXTRASPACINGINSTRUCTION

#include "Kyoto/Text/CInstruction.hpp"

class CLineExtraSpaceInstruction : public CInstruction {
public:
  CLineExtraSpaceInstruction(int spacing) : x4_spacing(spacing) {}
  ~CLineExtraSpaceInstruction() {}
  
  void Invoke(CFontRenderState& state, CTextRenderBuffer* buf) const;
  void PageInvoke(CFontRenderState& state, CTextRenderBuffer* buf) const;
private:
  int x4_spacing;
};

#ifdef TARGET_PC
// Trilogy's character extra space instruction (vtable rs5mp1 80492658, Invoke fn_802DFA0C:
// state options +0x48 = x4).
class CExtraCharacterSpaceInstruction : public CInstruction {
public:
  CExtraCharacterSpaceInstruction(int spacing) : x4_spacing(spacing) {}
  ~CExtraCharacterSpaceInstruction() {}

  void Invoke(CFontRenderState& state, CTextRenderBuffer* buf) const;
  void PageInvoke(CFontRenderState& state, CTextRenderBuffer* buf) const;
private:
  int x4_spacing;
};
#endif

#endif // _CLINEEXTRASPACINGINSTRUCTION
