#pragma once

// Loader-only state. A PART's early PIRN selects particle-initial rather than load-time
// constants. Each description starts retail; recursive loads restore their caller.
struct CPortParticleParseScope {
  inline static thread_local bool particleInitialRandom = false;
  const bool previous = particleInitialRandom;
  CPortParticleParseScope() { particleInitialRandom = false; }
  ~CPortParticleParseScope() { particleInitialRandom = previous; }
  CPortParticleParseScope(const CPortParticleParseScope&) = delete;
  CPortParticleParseScope& operator=(const CPortParticleParseScope&) = delete;
};
