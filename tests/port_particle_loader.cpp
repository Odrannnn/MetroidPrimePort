// Linked against the game's real objects by tools/test_particle_loader.py: no parser/getter stubs.
#include "Kyoto/Particles/CParticleDataFactory.hpp"
#include "Kyoto/Particles/CPortParticleParseScope.hpp"
#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/CRealElement.hpp"
#include "Kyoto/Particles/CVectorElement.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "port_remastered_effect_convert.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

static void Check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Bytes {
  std::vector<unsigned char> b;
  Bytes& tag(const char* t) { b.insert(b.end(), t, t+4); return *this; }
  Bytes& word(unsigned w) { for (int i=24;i>=0;i-=8) b.push_back(w>>i); return *this; }
  Bytes& real(float f) { unsigned w; std::memcpy(&w,&f,4); return tag("CNST").word(w); }
  Bytes& irnd(float lo,float hi) { return tag("IRND").real(lo).real(hi); }
  Bytes& marker() { return tag("PIRN").tag("CNST").word(1); }
  Bytes& basis() { return tag("CNST").real(90).real(0).real(180); }
};
static Bytes Fixture(int mode) {
  Bytes s; s.tag("GPSM"); if(mode==1) s.marker();
  s.tag("PMSC").tag("BSSC").tag("MULT").tag("CNST").irnd(.75f,2).real(.75f).irnd(1,3);
  s.tag("RTOV").irnd(.27f,.392f);
  s.tag("PMRT").tag("PQAD").basis().tag("PQAD").tag("CNST").real(180).real(0).irnd(35,250).basis();
  // A fourth loader roll: checks the original retail RNG order, not just the three values above.
  s.tag("PMOP").tag("CNST").irnd(0,1).real(0).real(0);
  if(mode==2) s.marker();
  s.tag("_END"); return s;
}
static std::unique_ptr<CGenDescription> Parse(const Bytes& s) {
  CMemoryInStream in(s.b.data(), s.b.size());
  return std::unique_ptr<CGenDescription>(CParticleDataFactory::GetGeneratorDesc(in,nullptr,0x1234));
}
static float Theta(const CVector3f& e) {
  constexpr float d=.01745329251994329577f*.5f;
  CQuaternion q=CQuaternion(std::cos(e.GetZ()*d),0,0,std::sin(e.GetZ()*d))*
      CQuaternion(std::cos(e.GetY()*d),0,std::sin(e.GetY()*d),0)*
      CQuaternion(std::cos(e.GetX()*d),std::sin(e.GetX()*d),0,0);
  auto m=q.BuildTransform(); float a=std::atan2(-m.Get20(),m.Get00())/ (2*d);
  return a<0?a+360:a;
}
static void Diversity(CGenDescription& desc) {
  Check(desc.xPortIrnd && !desc.x5c_PMRT->IsFastConstant(),"early PIRN keeps nested PQAD dynamic");
  CElementGen::CParticle p;
  float firstTheta=0,firstX=0,firstZ=0; bool thetaDiff=false,xDiff=false,zDiff=false;
  for(unsigned seed=1;seed<=16;++seed) {
    p.xPortSeed=seed; CParticleGlobals::xPortIrndParticle=&p;
    CVector3f a(0,0,0),b(0,0,0),s(0,0,0),t(0,0,0);
    desc.x5c_PMRT->GetValue(7,a); desc.x5c_PMRT->GetValue(39,b);
    desc.x60_PMSC->GetValue(7,s); desc.x60_PMSC->GetValue(39,t);
    Check((a-b).MagSquared()==0 && (s-t).MagSquared()==0,"same particle stable across evaluations/ages");
    float theta=Theta(a), scalar=s.GetZ()/.75f, x=s.GetX()/scalar,z=s.GetY()/scalar;
    Check(theta>=34.999f && theta<=250.001f && scalar>=.27f && scalar<=.392f &&
          x>=.75f && x<=2 && z>=1 && z<=3,"authored PMRQ/PMSC ranges");
    if(seed==1) { firstTheta=theta;firstX=x;firstZ=z; }
    else { thetaDiff|=std::fabs(theta-firstTheta)>.01f; xDiff|=std::fabs(x-firstX)>.001f; zDiff|=std::fabs(z-firstZ)>.001f; }
  }
  CParticleGlobals::xPortIrndParticle=nullptr;
  Check(thetaDiff && xDiff && zDiff,"distinct seeds vary angle and BOTH inner scale components, not load-seed99");
}
static void Retail(CGenDescription& desc) {
  Check(desc.x5c_PMRT->IsFastConstant(),"retail nested vector folding unchanged");
  CElementGen::CParticle p; p.xPortSeed=42; CParticleGlobals::xPortIrndParticle=&p;
  CVector3f a(0,0,0),s(0,0,0),op(0,0,0);
  desc.x5c_PMRT->GetValue(0,a);desc.x60_PMSC->GetValue(0,s);desc.x58_PMOP->GetValue(0,op);
  CParticleGlobals::xPortIrndParticle=nullptr;
  const float scalar=s.GetZ()/.75f;
  Check(std::fabs(Theta(a)-108.605553f)<.0002f && std::fabs(s.GetX()/scalar-1.295357466f)<1e-6f &&
        std::fabs(s.GetY()/scalar-1.193270802f)<1e-6f,"retail first three seed99 rolls unchanged");
  unsigned state=99;for(int i=0;i<4;++i) state=state*0x41c64e6d+0x3039;
  Check(std::fabs(op.GetX()-float((state>>16)&65535)*1.52590222e-5f)<1e-7f,"retail fourth roll/RNG ordering unchanged");
}
// Exercise recursive real factory calls while an outer description is still being read.
// One-byte blocks make the child load occur immediately after the parent's marker/header.
class NestedStream : public CInputStream {
  const Bytes& bytes;
  size_t pos=0;
  bool parentConverted, childConverted;
public:
  NestedStream(const Bytes& b,bool parent,bool child)
  : CInputStream(1),bytes(b),parentConverted(parent),childConverted(child) {}
  size_t Read(void* dest,size_t len) override {
    if(pos==(parentConverted?16u:4u)) {
      Check(CPortParticleParseScope::particleInitialRandom==parentConverted,"outer factory is active at recursive load");
      auto child=Parse(Fixture(childConverted?1:0));
      if(childConverted) Diversity(*child);else Retail(*child);
      Check(CPortParticleParseScope::particleInitialRandom==parentConverted,"recursive factory restores outer mode");
    }
    if(!len || pos==bytes.b.size()) return 0;
    *static_cast<unsigned char*>(dest)=bytes.b[pos++];return 1;
  }
};
int main(int argc,char** argv) {
  auto converted=Parse(Fixture(1)); Diversity(*converted);
  Check(!CPortParticleParseScope::particleInitialRandom,"loader scope restores retail");
  Retail(*Parse(Fixture(0))); Retail(*Parse(Fixture(2))); // Late PIRN is explicitly not repaired.
  {
    CPortParticleParseScope outer; CPortParticleParseScope::particleInitialRandom=true;
    Retail(*Parse(Fixture(0))); Check(CPortParticleParseScope::particleInitialRandom,"nested retail restores converted parent");
    Diversity(*Parse(Fixture(1))); Check(CPortParticleParseScope::particleInitialRandom,"nested converted restores parent");
  }
  Retail(*Parse(Fixture(0)));
  {
    // The converter writes MPXV as PMXV: an unread element stops the description there, dropping MAXP after it.
    Bytes s; s.tag("GPSM").tag("ADV1").tag("PMXV").tag("CNST").word(0).real(.5f);
    s.tag("MAXP").tag("CNST").word(7).tag("_END");
    auto desc=Parse(s); int maxp=0;
    Check(desc->x10c_ADV1!=nullptr && desc->x1c_MAXP!=nullptr && !desc->x1c_MAXP->GetValue(0,maxp) && maxp==7,
          "PMXV reads and the properties after it load");
  }
  for(bool parent:{false,true}) for(bool child:{false,true}) {
    Bytes bytes=Fixture(parent?1:0);NestedStream in(bytes,parent,child);
    std::unique_ptr<CGenDescription> desc(CParticleDataFactory::GetGeneratorDesc(in,nullptr,0x5678));
    if(parent) Diversity(*desc);else Retail(*desc);
    Check(!CPortParticleParseScope::particleInitialRandom,"recursive factory tree leaves no mode behind");
  }
  if(argc==2) {
    std::ifstream f(argv[1],std::ios::binary);Bytes s;
    s.b.assign(std::istreambuf_iterator<char>(f),{});Check(s.b.size()>16,"fixture read");
    std::vector<PortRemastered::RetailPartProperty> props;std::string error;
    Check(PortRemastered::SplitRetailPart(s.b.data(),s.b.size(),props,error),"target structural parse");
    Check(!props.empty() && props[0].fourcc==PortRemastered::EffectFourCC("PIRN"),"target PIRN is FIRST");
    Bytes selected;selected.tag("GPSM");
    for(const auto& prop:props) {
      // Preserve exact serialized values/order; resource-free projection avoids needing a disc/pool.
      if(prop.fourcc==PortRemastered::EffectFourCC("PIRN") || prop.fourcc==PortRemastered::EffectFourCC("PMRT") ||
         prop.fourcc==PortRemastered::EffectFourCC("PMSC")) {
        selected.word(prop.fourcc);selected.b.insert(selected.b.end(),prop.value.begin(),prop.value.end());
      }
    }
    selected.tag("_END");auto target=Parse(selected);Diversity(*target);
  }
  std::puts("PASS: actual PART parser, nested PMRQ/PMSC, stable/diverse seeds, retail load RNG, late-marker and scope restoration");
}
