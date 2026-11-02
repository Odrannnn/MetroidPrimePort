#pragma once

// Loader-only state. A PART's early PIRN selects particle-initial rather than load-time
// constants. Each description starts retail; recursive loads restore their caller.
// visibilityQuery is set by a PVSQ read in the description (Remastered sets its build
// context's +0x55 when the factory reads VISQ, GetRealElement 0x2619e8).
struct CPortParticleParseScope {
  inline static thread_local bool particleInitialRandom = false;
  inline static thread_local bool visibilityQuery = false;
  const bool previous = particleInitialRandom;
  const bool previousVisibilityQuery = visibilityQuery;
  CPortParticleParseScope() {
    particleInitialRandom = false;
    visibilityQuery = false;
  }
  ~CPortParticleParseScope() {
    particleInitialRandom = previous;
    visibilityQuery = previousVisibilityQuery;
  }
  CPortParticleParseScope(const CPortParticleParseScope&) = delete;
  CPortParticleParseScope& operator=(const CPortParticleParseScope&) = delete;
};
