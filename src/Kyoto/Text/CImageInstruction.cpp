#include "Kyoto/Text/CImageInstruction.hpp"

#include "Kyoto/Math/CVector2i.hpp"
#include "Kyoto/Text/CFontRenderState.hpp"
#include "Kyoto/Text/CLineInstruction.hpp"
#include "Kyoto/Text/CTextRenderBuffer.hpp"

int CFontImageDef::CalculateHeight() const {
  return GetHeight() - (GetHeight() - CalculateBaseline());
}

void CImageInstruction::Invoke(CFontRenderState& state, CTextRenderBuffer* buf) const {
  if (x4_image.IsLoaded() && !x4_image.GetImages().empty()) {
    if (state.GetBlock()->GetTextDirection() == kTD_Horizontal) {
      if (buf) {
#ifdef TARGET_PC
        // Retail sits an image's baseline 5/6 down it, which suited the GameCube
        // art; the port's prompt icons sit at the quad's centre and rode about a
        // third of their own height above the text. A baseline 2/3 down centres
        // them on the capitals.
        int y = state.GetY() + state.GetLine()->GetBaseline() - x4_image.GetHeight() * 2 / 3;
#else
        int y = state.GetY() + state.GetLine()->GetBaseline() - x4_image.CalculateBaseline();
#endif
        buf->AddImage(CVector2i(state.GetX(), y), x4_image);
      }
      state.SetX(state.GetX() + x4_image.GetWidth());
    } else {
      int offset = (state.GetLine()->GetWidth() - x4_image.GetWidth()) / 2;
      if (buf) {
        buf->AddImage(CVector2i(offset + state.GetX(), state.GetY()), x4_image);
      }
      state.SetY(state.GetY() + x4_image.CalculateHeight());
    }
  }
}

void CImageInstruction::GetAssets(rstl::vector< CToken >& assetsOut) const {
  for (int i = 0; i < x4_image.GetImages().size(); ++i) {
    assetsOut.push_back(x4_image.GetImages()[i]);
  }
}
