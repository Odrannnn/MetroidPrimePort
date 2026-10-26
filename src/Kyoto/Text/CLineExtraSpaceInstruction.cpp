#include "Kyoto/Text/CLineExtraSpaceInstruction.hpp"

#include "Kyoto/Text/CFontRenderState.hpp"

void CLineExtraSpaceInstruction::Invoke(CFontRenderState& state, CTextRenderBuffer* buf) const {
  state.SetExtraLineSpace(x4_spacing);
}

void CLineExtraSpaceInstruction::PageInvoke(CFontRenderState& state, CTextRenderBuffer* buf) const {
  Invoke(state, buf);
}

#ifdef TARGET_PC
// @ fn_802DFA0C
void CExtraCharacterSpaceInstruction::Invoke(CFontRenderState& state,
                                             CTextRenderBuffer* buf) const {
  state.GetOptions().SetExtraCharacterSpacing(x4_spacing);
}

// @ fn_802DFA18 (calls Invoke)
void CExtraCharacterSpaceInstruction::PageInvoke(CFontRenderState& state,
                                                 CTextRenderBuffer* buf) const {
  Invoke(state, buf);
}
#endif
