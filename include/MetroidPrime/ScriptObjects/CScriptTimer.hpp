#ifndef _CSCRIPTTIMER
#define _CSCRIPTTIMER

#include "MetroidPrime/CEntity.hpp"

class CScriptTimer : public CEntity {
  friend struct PortRoomState;

#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  uint x34_startFrame;
#endif
  float x34_time;
  float x38_startTime;
  float x3c_maxRandDelay;
  uchar x40_loop;
  bool x41_autoStart;
  bool x42_isTiming;
#ifdef TARGET_PC
  // Set when a message starts timing; the next ApplyTime skips that tick (see ApplyTime).
  bool mSkipNextTick;
#endif

  public:
    CScriptTimer(TUniqueId, const rstl::string&, const CEntityInfo&, float, float, bool, bool, bool);
    ~CScriptTimer();

    DECLARE_TYPES_MATCH_OR_ACCEPT;
    void Reset(CStateManager&);
    void AcceptScriptMsg(EScriptObjectMessage, TUniqueId, CStateManager&);
    void ApplyTime(float, CStateManager&);
    void Think(float, CStateManager&);

    bool IsTiming() const { return x42_isTiming; }
    void StartTiming(bool isTiming) {
      x42_isTiming = isTiming;
    }
};

CHECK_SIZEOF(CScriptTimer, (VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02 ? 0x48 : 0x44))

#endif // _CSCRIPTTIMER
