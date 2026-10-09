#include "port_env.h"
#include "port_remastered_effect_import.h"

#include "port_remastered_effect_convert.h"
#include "port_remastered_image.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>

namespace PortRemastered {
namespace {

constexpr uint32_t kGenp = EffectFourCC("GENP");
constexpr uint32_t kMati = EffectFourCC("MATI");
constexpr uint32_t kPart = EffectFourCC("PART");
constexpr uint32_t kSwsh = EffectFourCC("SWSH");
constexpr uint32_t kTxtr = EffectFourCC("TXTR");
constexpr uint32_t kCmdl = EffectFourCC("CMDL");

// Effect textures are drawn small; larger ones are scaled down to this side.
constexpr int kMaxTextureSide = 512;
// A flipbook atlas keeps its frames' size up to this edge.
constexpr int kMaxAtlasSide = 2048;
// A larger texture goes to a native .dds; its TXTR is scaled down to this edge.
constexpr int kStubSide = 64;

// Between a pak's byte order and the order an effect stores an id in: the
// first three groups byte-swapped (the same swap both ways).
EffectGuid Swap(const EffectGuid& id) {
  EffectGuid out = id;
  std::swap(out[0], out[3]);
  std::swap(out[1], out[2]);
  std::swap(out[4], out[5]);
  std::swap(out[6], out[7]);
  return out;
}

uint32_t Hash(const EffectGuid& id, uint32_t salt) {
  uint32_t hash = 0x811C9DC5u ^ salt;  // FNV-1a
  for (uint8_t byte : id) {
    hash = (hash ^ byte) * 0x01000193u;
  }
  return hash;
}

std::string Hex(uint32_t id) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", id);
  return text;
}

int RoundUp4(int side) { return std::max(8, (side + 3) / 4 * 4); }

bool IsLight(uint32_t fourcc) {
  for (uint32_t light : {EffectFourCC("LTYP"), EffectFourCC("LFOT"), EffectFourCC("LCLR"), EffectFourCC("LINT"),
                         EffectFourCC("LOFF"), EffectFourCC("LDIR"), EffectFourCC("LFOR"), EffectFourCC("LSLA")}) {
    if (fourcc == light) {
      return true;
    }
  }
  return false;
}

bool HasLight(const std::vector<RetailPartProperty>& properties) {
  return std::any_of(properties.begin(), properties.end(),
                     [](const RetailPartProperty& property) { return property.fourcc == EffectFourCC("LTYP"); });
}

const EffectNode* FindNode(const EffectNode& node, const EffectGuid& id) {
  for (const EffectNode& child : node.children) {
    if (child.id == id) {
      return &child;
    }
    if (const EffectNode* found = FindNode(child, id)) {
      return found;
    }
  }
  return nullptr;
}

bool HasProperty(const EffectNode& node, uint32_t fourcc) {
  return std::any_of(node.properties.begin(), node.properties.end(),
                     [&](const EffectProperty& property) { return property.fourcc == fourcc; });
}

// A generator that drew a texture or a model whose converted PART draws
// neither, or drew a model and now draws only its material's texture as a
// sprite (at the sprite's default size, a screen-filling quad): the
// conversion lost its look.
bool LostLook(const EffectNode& node, const std::vector<RetailPartProperty>& part) {
  const auto has = [&](uint32_t fourcc) {
    return std::any_of(part.begin(), part.end(),
                       [&](const RetailPartProperty& property) { return property.fourcc == fourcc; });
  };
  if (HasProperty(node, EffectFourCC("PMDL"))) {
    return !has(EffectFourCC("PMDL"));
  }
  const bool hadTexture = HasProperty(node, EffectFourCC("TEXR")) || HasProperty(node, EffectFourCC("MTIN"));
  return hadTexture && !has(EffectFourCC("TEXR"));
}

void PutFourCC(std::vector<uint8_t>& out, uint32_t fourcc) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(fourcc >> shift));
  }
}

// `converted` with its light replaced by the disc PART's. Remastered leaves
// out LOFF/LDIR/LFOR/LSLA (it has LIRD/LORD instead, which do not map onto
// them) and differs from retail in places, so the disc's light is the one
// retail's lighting was made for.
std::vector<uint8_t> WithDiscLight(const std::vector<RetailPartProperty>& converted,
                                   const std::vector<RetailPartProperty>& disc) {
  std::vector<uint8_t> out;
  PutFourCC(out, EffectFourCC("GPSM"));
  auto put = [&](const RetailPartProperty& property) {
    PutFourCC(out, property.fourcc);
    out.insert(out.end(), property.value.begin(), property.value.end());
  };
  for (const RetailPartProperty& property : converted) {
    if (!IsLight(property.fourcc)) {
      put(property);
    }
  }
  for (const RetailPartProperty& property : disc) {
    if (IsLight(property.fourcc)) {
      put(property);
    }
  }
  PutFourCC(out, EffectFourCC("_END"));
  return out;
}

// Effects Remastered gave a fresh id, matched to the retail PART they replace.
struct MatchedEffect {
  const char* id;  // as IdToString prints it (EffectGuidString of the stored id)
  uint32_t retail;
};
constexpr MatchedEffect kMatchedEffects[] = {
    {"#name", 0},  // method of the entries below
    // By the name both paks give it (the player's and global paks'). Only names
    // that one retail PART has; the 8 effects that kept a retail id all match
    // their names.
    {"fb4d5181-cd7e-4c5e-8b11-afe35d30e231", 0x1EF973EA},  // BombExplo
    {"bbbb849a-d896-489f-83da-ef76c010888d", 0xC0E95E90},  // BombSet
    {"ca6fc8a1-30a2-441f-bf7a-12b6acd4e938", 0x39F0F5C6},  // BoostBallGlow
    {"ceaee14a-a690-44cc-a90b-0bd34935ae6c", 0x523048E0},  // BusterLight
    {"9c215d05-e582-4897-b958-85eb83c9f4f3", 0x9B564161},  // BusterMuzzle
    {"c1f18af2-c6e5-486b-86e0-0609316733f2", 0x04E29C5B},  // BusterSparks
    {"9499e42a-54bc-4402-8557-aa300f3db8de", 0xD8DB86CA},  // DirtWake
    {"97280bbe-5eef-42b6-b5bd-096b8c089c76", 0xF0C02F49},  // Effect_Ash
    {"70108314-fb42-4090-9ab4-b1c8c6570ee2", 0xA6B67F45},  // Effect_FirePop
    {"4dd6affe-ffd8-40ed-a056-1a77b9dcb9d6", 0xE6FC0230},  // Effect_IceBreak
    {"7bb7ac6f-16a2-45f0-a103-f634fe027768", 0x017DFFD5},  // Effect_IcePop
    {"103f6797-5ea0-4c2b-9c9b-eafdce3e48f4", 0xABE56164},  // Effect_MorphBallIceBreak
    {"9c56f6df-a9ae-403b-8da3-6a6ec6405971", 0x2D65511C},  // Effect_OnFire
    {"7cbb6382-2788-4225-b3a2-3430ba5558b7", 0xF42646D4},  // FlameMuzzle
    {"416c81e0-e3ab-4f72-b3f5-3aab7b851ba6", 0xD5A18910},  // grappleClaw
    {"9905ed28-e8ac-4231-9b05-ca49e338eb2d", 0xCFC222B0},  // grappleHit
    {"3727f6c9-0f4f-4369-ba8f-b22e71bfdd1d", 0x2CC7F7F5},  // grappleMuzzle
    {"14e59c07-ea96-4f88-ae84-a8762b7beab8", 0x87C0BDB2},  // grappleSegment
    {"f8502f32-276d-4db5-998c-f57c7a2cb00b", 0x7072A62D},  // holoTransition
    {"7114b32f-78b8-45fd-adc0-ab905c7671eb", 0x1BBFC5A6},  // Ice2nd_1
    {"c8aa99d2-f7a9-4bc0-bff1-1fb1b42c1990", 0xF97661F1},  // Ice2nd_2
    {"a6c1a9fb-e480-4b50-8e16-49ab849c86b7", 0x21F4D9AB},  // IceAuxMuzzle
    {"0649036e-0b4f-450f-b33d-01e9045a29c9", 0x6ECDC394},  // IceCharge
    {"91b571dc-1b15-4053-8e1a-8744abd2f2e1", 0x9ADE39C3},  // IceMuzzle
    {"b589f70b-1853-490e-bba8-a4f120563ddd", 0xC82F2028},  // IceSmoke
    {"42f41dcd-0da3-40e2-b4ec-d1c6096eda5b", 0xDE1A1140},  // IceSpread1
    {"b28d85f4-67a3-4664-981f-6fa9bf044f93", 0x045DDB2F},  // IceXfer
    {"ec64d113-7337-4ce0-bda7-0f885bfd0f50", 0x43A81EEC},  // MorphBallTransitionFlash
    {"56844d59-1a0f-4313-a6a8-533292dfd75c", 0x8B8CD2F6},  // MudWake
    {"2593f90a-21cf-4249-a3f2-64ef7fabe99b", 0xF639D24E},  // NFTMainFire
    {"369cc052-6d7b-44d0-8a33-e18f6fc67f85", 0xD67EE2D9},  // NFTMainSmoke
    {"d0766ccc-22d9-444f-8529-a8a6ada6cbdd", 0x1F4FD93A},  // NFTSecondaryFire
    {"bfe5b4c5-421f-41d1-9554-6d3e00f72066", 0xAD51661F},  // NFTSecondarySmoke
    {"93004165-738e-404b-ac29-1f1a059719ae", 0x7DA3DEE5},  // NFTSecondarySparks
    {"ef65b87f-1e8e-49a2-bc3e-8d2db0405e12", 0x8F91A657},  // NFTSwooshCenter (SWSH)
    {"bd4c298c-5b95-48c2-89d2-d074284b98a9", 0xC42C2CA6},  // NFTSwooshFire (SWSH)
    {"58175f6a-d712-4852-b350-13321240dc65", 0x7754967A},  // Phazon2nd_1
    {"c8536e8f-2e70-41b2-82b9-1c8410ac9c6f", 0x1C56F6B1},  // PhazonMuzzle
    {"4474842f-b051-45be-b72b-b7d03128d755", 0x18CB74EF},  // PhazonWake
    {"3210e9de-2f83-48f3-9168-d8a3587be355", 0x6C35D8FE},  // PhazonWakeOrange
    {"541f775f-c171-4b2e-a182-6ada992a21dc", 0xC0A88A87},  // Plasma2nd_1
    {"f494900e-6c34-41bb-a372-9f6f1fe6468a", 0xB0F9DBE6},  // PlasmaAuxMuzzle
    {"b2f2c408-d488-43ab-8562-e85bca2b654f", 0xD3053354},  // PlasmaCharge
    {"d682fa42-228c-445a-856f-18581ed7866d", 0x8D7BBFB2},  // PlasmaMuzzle
    {"6a83fb12-e81e-499a-9014-15a539f53ec9", 0x5721EE48},  // PlasmaXfer
    {"51d96194-91b7-416e-8c37-13031270163e", 0x3183F0A0},  // Power2nd_1
    {"4bc8b7ca-ce44-4e88-9e8f-a764138a9601", 0x7E8ADCBA},  // PowerBombExplo
    {"835d9ae7-2f75-48ab-ac9f-b9827b8740ad", 0x4CE91ECB},  // PowerCharge
    {"597c6fe6-6fe7-439b-956b-87728a5a067f", 0x0F21403B},  // PowerMuzzle
    {"411dae2f-d4be-4324-9433-7822f188928c", 0x3DD09610},  // PowerXfer
    {"d3abe44e-d8a8-473b-a60e-c2289eb6a74d", 0x8185DEB3},  // RainWake
    {"c82dd5d6-f6d9-4908-9794-f3fd62627c32", 0xF421ED31},  // SandWake
    {"5c2798a0-12e5-4153-b187-0594d7f8fa5d", 0x60817832},  // ShotSmoke
    {"81cb1d8a-4cb4-4469-8127-d6c761752e7c", 0xC9D4BA43},  // SnowWake
    {"026c1c9a-df22-4230-937b-29e785637252", 0x22B005A1},  // SpiderBallMagnetEffect
    {"7e1ca0b3-d242-4724-b1dc-ecb96e43bc73", 0xE1341D07},  // WallSpark
    {"1b44823d-c587-4b51-9c1d-65144c5fcac5", 0x629C848F},  // Wave2nd_3
    {"50b67be4-15f3-4230-9147-deb8d6a47248", 0x7E520CBC},  // WaveAuxMuzzle
    {"a0d802cb-6516-4a42-9c1d-7614e6122ca9", 0x2BC80C63},  // WaveCharge
    {"59dba49a-777f-42eb-ab47-c5dd91a28d3e", 0x0237C838},  // WaveXfer
    {"#room-placement", 0},  // method of the entries below
    // By where the room scripts place it: a ROOM's EffectMP1 naming only this
    // effect, at the spot (under 0.1 m) of a retail object naming only this
    // PART, in every such placement, with no other fresh effect or carried-over
    // one claiming the PART. The same rule pairs all 97 such placements of the
    // carried-over effects with their own id. The comment is the first room
    // (+ how many more).
    {"0040cf36-fd79-4264-8ad9-4bbdd7b5f18e", 0xABEA897E},  // 20_reflecting_pool
    {"02707a2b-9131-4f9c-917e-fb3f9f05db1c", 0x424FFEF7},  // 08a_IntroUnderwater_ventshaft +1
    {"02d03932-c135-4244-b1c1-50f34488ad25", 0x371A7EEE},  // 00a_over_hall +7
    {"02db3cbc-dbfc-4131-821d-6f96b4a9b506", 0xD81CFC9E},  // 00F_intro_begin
    {"034bd14d-2bc9-481f-a619-f79a584e6f92", 0x7A95281E},  // 03a_crater
    {"06ee7e7c-663b-43be-80e5-520e15ee2976", 0x77B8A73B},  // 0p_connect_tunnel
    {"0a450035-0b32-4ff9-a376-bd7e0345aa1b", 0xCD494C73},  // 18_Ice_Gravity_Chamber
    {"0a6f8735-0847-455f-8365-4fa6fadddf7d", 0xA11929CF},  // 11_Ice_Observatory
    {"0b66c940-0645-410c-b505-c168ac2ede27", 0x9271FCB8},  // 02_Intro_Elevator
    {"0b97f917-0308-4d9c-87c0-429c7e2f2b2f", 0x02787FD6},  // 05_Zoo
    {"0dae37f3-9223-4187-b932-fdd74d92cf46", 0xFDB2830E},  // 14_tl_base01
    {"0e2e3afe-eab2-4739-a82b-636678e996ee", 0xD108687B},  // 12_Mines_eliteboss
    {"0edb2029-b9df-4522-a94c-8c5701d4ae5c", 0xCEE7ADCA},  // 00F_intro_begin
    {"0ff6151e-7272-42b4-8c5e-c5018cc6e44c", 0x89F18B4D},  // 11_Ice_Observatory
    {"115bd210-face-4399-a19a-476dd5ce4d43", 0x82396733},  // 05_IntroUnderwaterZoo
    {"13a0b617-347d-42c3-8009-8bcb6767bf83", 0x40B4B8AC},  // 07_Over_Stonehenge
    {"16dc475c-d50c-42c5-8a4b-20e636a4e980", 0xDBF12204},  // 11_Ice_Observatory
    {"17dd34bd-3af8-4e5d-be0b-7fae6f94c4d6", 0x8804A9BF},  // 05_Mines_forcefields
    {"1afc0d7c-babe-4bdb-b4cd-2052ccc4fb14", 0xA7C92DE0},  // 07_Over_Stonehenge
    {"2077f968-bffe-432c-bcca-6e877fdaf021", 0x50DD1328},  // 22_flaahgraChamber
    {"20cf1159-28b8-4221-8cf7-408a5a8d992f", 0xD89F3CEB},  // 13_mines_vertical_ascent
    {"2254aba7-3bfc-4b48-ae1e-e81e97c6492c", 0x6FC6B324},  // 00b_mines_connect +9
    {"22b3f678-35be-4214-9dd0-ab3d5dff147b", 0x4DDF468C},  // 06_Ice_Temple
    {"23c2978d-1961-498c-bfa5-63c8efaffb99", 0x0FBD4196},  // 07_Over_Stonehenge
    {"25a15301-1cd7-4660-bd07-327810cf6135", 0x13F23AB1},  // 00a_over_hall +7
    {"2710c664-4fbc-4358-bd8a-41824297f55c", 0xA8B1CCA8},  // 00f_ice_connect +1
    {"27ba22cf-87c7-4904-bab7-dae70760ff44", 0x5D664CA2},  // 11_Ice_Observatory
    {"291f812e-7a38-41e7-a277-eea20be5a5c3", 0xA369D3DB},  // 07_Over_Stonehenge
    {"29a7c944-95c8-4137-b63f-c3955d77c27e", 0x1D985C2C},  // 22_flaahgraChamber
    {"2c5365a6-7dc4-40b2-9729-2f7742998a81", 0x930DD780},  // 22_flaahgraChamber
    {"2ef9a569-58d8-422a-b8b1-37e184de3f50", 0x371C563C},  // 00i_Mines_connect
    {"309a173d-f865-4487-b421-c9e64edec577", 0xB4A658C3},  // 08_Mines
    {"309fc565-2f71-414a-83c6-98833253b563", 0x07F04ED5},  // 00l_over_hall
    {"335247ff-2780-4bf0-ad60-2bdc0f609c62", 0x5A5E6C8F},  // 12_Mines_eliteboss
    {"3537fd56-c4d4-4f23-ac77-3d642c52f836", 0x757EDCE9},  // 05_Over_xrayroom +2
    {"39f50802-ef90-4f7a-a9c5-adf3be6bb791", 0x06B3F06E},  // 01_intro_hanger_connect
    {"39f5c872-0e93-4a66-a847-f8465f8af571", 0x4693F099},  // 02_Intro_Elevator
    {"3a33588c-6b7f-453b-8e4a-82bca75f6de0", 0x251B04F4},  // 0v_connect_tunnel +4
    {"3bece9e5-c319-4e01-a28e-a38fee7777b8", 0xB0599D9B},  // 11_Ice_Observatory
    {"3c93322a-845d-49b5-bb15-650bbbaa2e04", 0x450DADFF},  // 00_Mines_Mapstation +2
    {"3f7ae5bd-d9d1-4c01-bca6-4d31b3937538", 0x70043FD4},  // 3_monkey_lower
    {"4044ff75-0097-4368-b240-8a3feb2f5af0", 0xE4CA5AE7},  // 00k_ice_connect +7
    {"41344111-1037-4af3-8c4e-1e137949029a", 0x1ECD34A2},  // 00i_ice_connect +10
    {"4365ca0c-3dd9-4267-99ca-321dc8ee6a3b", 0x2CD29D26},  // 00_Mines_Savestation_D
    {"445c8f18-92d2-449e-86c4-758dfba2b8fe", 0x409749DB},  // 17_ChozoBowling
    {"457769f5-1ac1-4cbe-b111-a4df50009f72", 0xFDBD7828},  // 17_Ice_Cave_B
    {"4abea211-408f-4fc3-b1fa-2984f60aecdb", 0x30F46D12},  // 03b_Crater
    {"4b2dc620-b765-4430-88ea-af0386f0e43b", 0xCEFB49E0},  // 07_Over_Stonehenge
    {"4c1a5053-e91b-4e03-b4ab-1c5b093ebf6c", 0x186971C3},  // 22_flaahgraChamber
    {"4ded839b-6e75-48e8-9218-91af6c230a64", 0x387E2199},  // 03f_Crater
    {"52394d38-35a2-44a5-97ad-995f1b27358b", 0x28016E5F},  // 07_Over_Stonehenge
    {"54da4609-8d65-4132-84e5-0b7fea7f0b30", 0xD16D45BB},  // 12_Mines_eliteboss
    {"56335892-39fe-4573-a81a-9241e7d087ab", 0xC8A42628},  // 00g_ice_connect +8
    {"57893869-e39b-43c8-b86d-e2e419030fba", 0x930B6C5F},  // 5_bathhall
    {"57bc7e0d-b2cf-43df-8fa2-b69c98d07b8c", 0xDED6D5A4},  // 00g_over_hall
    {"5811ed93-8a30-4769-ad84-0eaf8e496361", 0xA28210DF},  // 14_tl_base01
    {"59bb837a-7790-4ff1-9aeb-bd39f60abd2e", 0xDF5B7160},  // 0p_connect_tunnel
    {"5a61ec58-6e8f-48b3-b3e1-7ca6c0e951b6", 0x621D84AC},  // 12_Mines_eliteboss
    {"5b985124-2eff-4ef5-9f11-d5999646f0ae", 0x2230069F},  // 00c_lava_connect
    {"5cdf8300-04fe-4648-9a81-d64a373e8671", 0xE5DDD684},  // 16_furnaces
    {"5fb0da50-c693-41eb-8b33-00ef4ec8638b", 0xB2C0B71F},  // 19_Ice_Thardus
    {"600d7a0c-0719-4de5-99c8-59f8d28605e2", 0x37524FE9},  // 04_Ice_Boost_canyon +3
    {"60985973-2d34-42f2-8c43-2d57460e818a", 0xBD3DC521},  // 01_Over_mainplaza
    {"60dcd6c3-adf1-42c8-8ea4-7fd3934ffcc4", 0x185DD7C5},  // 00b_IntroUnderwater_connect
    {"628b3331-04f8-4da9-a533-3715c6282a70", 0x7909612F},  // 00f_over_hall +10
    {"65e71f3a-0327-4bb2-85b6-ed993f742791", 0x597475AD},  // 0p_connect_tunnel
    {"6916272d-4272-4ec4-a0b2-a4f9b3160ef6", 0x3A617BEB},  // 00b_Lava_Connect +3
    {"69c55769-8353-4afa-adfb-d5b9f9566e1b", 0x1CD31D99},  // 00F_intro_begin
    {"6a264e33-992c-46b7-94e2-112a1696a817", 0x1D93BC95},  // 10_Over_1Alavaarea
    {"6c7d0204-4456-42ce-a50a-1da0b06876d4", 0xA2D977BF},  // 07_Over_Stonehenge
    {"6ed2e1b1-03ba-48a5-9345-06636cea35a6", 0x4E63C759},  // 0p_connect_tunnel
    {"73c163b7-2cbf-4053-8a31-7e36655eda47", 0x898E04A0},  // 22_flaahgraChamber
    {"74065695-c63d-4021-baf3-123ea35b815c", 0x50A5876B},  // 19_hivetotem
    {"77224f08-a7be-42df-b1f6-38425aeeabe5", 0xB58775FC},  // 07_Over_Stonehenge
    {"7790bc6d-7532-42a3-be5e-9711f34a1f99", 0x76B916D1},  // 00e_IntroUnderwater_connect
    {"7a85a79f-a832-4a4d-80e4-051545fa7624", 0xBC84152B},  // 1a_morphball_shrine
    {"7b5059bc-5b2f-4d3c-bb2c-c44a07eaf97e", 0x47E5939E},  // 07_Over_Stonehenge
    {"7d6e4416-66fd-40ad-ba7c-9649cf725503", 0x4619C3F1},  // 07_Over_Stonehenge
    {"7f6c0fc0-adf5-4f37-b6ca-5ffa6731ed42", 0xD74B6FD2},  // 11_Ice_Observatory
    {"7fefddd7-fc42-49cb-b6a8-bc9243e45ae9", 0xDE1E2414},  // 03_over_pickup
    {"7ff707bd-0170-4d03-b140-7cc87321ffe8", 0x1B888B61},  // 18_halfpipe
    {"83e2824b-b4cc-4f42-a435-a601d828a3e5", 0x3DF9F7BE},  // 08_Ice_Ridley
    {"8c43cd20-3484-435d-82be-f1efc0fc7d42", 0xB4BE74A6},  // 00d_Intro_connect
    {"8c5a52c5-0317-44c5-96e3-b2ab015faf2a", 0x3AE271F4},  // 03f_Crater
    {"8d4865f1-bb83-4520-a7d8-da09dee766b1", 0x6A582E2B},  // 13_Over_burningeffigy
    {"8ff1284e-723b-4c98-90ec-6220a713ddc9", 0x205B13E3},  // 02_Intro_Elevator
    {"9251aa29-ab9b-466a-9a3f-03726d09c038", 0x9602A23A},  // 07_Mines_electric
    {"9317cb5b-a425-4b2a-b6eb-6f1a5891a4dd", 0xCCABC2EC},  // 3_monkey_lower
    {"95964af3-68cc-4c95-adc8-4728a3a018ee", 0x53861B29},  // 07_Over_Stonehenge
    {"96a7a567-0eb5-43da-b63a-cb007b5d0af2", 0xBD5CF12E},  // 05_Mines_forcefields
    {"97a7ee62-75ea-4791-b0b1-110227d04092", 0xC0C82ED0},  // 07_Over_Stonehenge
    {"98f103c6-926c-42c3-a766-9ebc65bf79c2", 0x4DD133C8},  // 07_Over_Stonehenge
    {"99bcbed9-5c9b-4317-9623-33eee5d5a683", 0x549475DE},  // 12_Mines_eliteboss
    {"9a15707d-25ba-424b-a6b1-04a52616986e", 0xE93B85DC},  // 07_Over_Stonehenge
    {"9b0876c3-317b-480d-8672-998785401aa0", 0x338F4B8A},  // 04_Intro_Specimen_Chamber
    {"9dd83fc0-8563-42a0-97cf-e5790fce54d1", 0x042EADAC},  // 11_Ice_Observatory
    {"9f3c5719-2e4d-4aa4-bf0b-ca51fd77d50f", 0x83A1450F},  // 00d_Intro_connect
    {"a178f834-68b9-4285-8fb2-73cb172319f0", 0x85DBF2BC},  // 0p_connect_tunnel
    {"a1a0b5e0-f612-459d-bb64-5144c869ab25", 0x6E238E65},  // 03a_crater
    {"a54ddf94-8f35-48c1-bafa-87ca737636c9", 0x00FB9A4D},  // 07_Over_Stonehenge
    {"a5b20f7a-8fa5-40c1-aa41-3ec2a4b688a9", 0x934BAFB8},  // 04_Intro_Specimen_Chamber
    {"a8475a7e-1e0b-4800-9d2c-91731c855dd4", 0xB15B2B5E},  // 07_Over_Stonehenge
    {"a8857d04-09f1-4532-9eeb-5e957355f31d", 0xFA41CC07},  // 05_Mines_forcefields
    {"a926c501-3277-4f76-b0a6-11d4264aa0d5", 0x59BA29DB},  // 07_Mines_electric
    {"ab2037b6-c0ac-4dfb-8bea-98c8b6910a07", 0x87817423},  // 5_bathhall
    {"ab210e1b-9617-4fd6-9c8c-98efb66568c1", 0xABAB33BA},  // 07_Over_Stonehenge
    {"aca74c4a-dd5e-4fe3-89c1-7de257330ee0", 0x277CABEC},  // 12_Mines_eliteboss
    {"af780373-de26-4349-8c27-dc49df6a9f10", 0xDE06865F},  // 05_Zoo
    {"b2b70ba4-0585-4cc9-a2f4-b737cb81b02a", 0xCB271679},  // 00b_Intro_Connect
    {"b3c9de27-f456-4de9-806f-3bf74a99eca7", 0x715DBDF0},  // 02_Intro_Epodroom
    {"b570f388-7be8-4baa-bac4-88771b2ba7f9", 0xA4F230FD},  // 00o_over_hall
    {"b8c3841e-b81b-427a-9b57-f7f970b48c2d", 0xD2967C1D},  // 09_Ice_Lobby
    {"b8cc7d68-1a15-472b-8942-ef68e2a392ef", 0x73483913},  // 07_Intro_Reactor
    {"b9ae3073-22db-4b17-aabe-a660ba6a311b", 0x8214BFCD},  // 04_Intro_Specimen_Chamber
    {"b9c4fbdb-d7b4-4dd9-a3db-13aaa2401c91", 0x8964D1BA},  // 12_Mines_eliteboss
    {"bb850e5d-5d64-4069-ba73-cdd0631054a4", 0x96993798},  // 07_Over_Stonehenge
    {"bb8d240b-e688-48ac-a17c-cc4b4b87240d", 0x4F5FC020},  // 02_Intro_Elevator
    {"be62630f-0d27-49d8-b9a0-3abce40b7f6c", 0x9CE89362},  // 11_Ice_Observatory
    {"c457accd-60a1-4d34-9304-e905b3ca10ef", 0x6097F60F},  // 11_Ice_Observatory
    {"c618ce1f-9579-486c-9f5f-73d9d00d7406", 0x531390D6},  // 0q_connect_tunnel
    {"c65d8956-5d6b-4064-9e62-656d43c5c767", 0x3FEEF398},  // 12_Mines_eliteboss
    {"ca64283e-7b49-47d6-892d-49fcabee3085", 0x46B0CE60},  // 00g_over_hall
    {"cce706c7-4299-4791-974e-4bdc3ed2539f", 0x0C0B11AC},  // 12_Mines_eliteboss
    {"cd873798-b325-4394-a574-caee0bdaeadd", 0x9C1920EF},  // 08_Ice_Ridley
    {"cebbb395-8928-48c4-aeca-c5e9fea5abc5", 0x2509A992},  // 04_Intro_Specimen_Chamber +2
    {"cef0b80d-20b2-4208-877b-03cdf5153b64", 0xA5ED941C},  // 1a_morphball_shrine
    {"cf619c68-33fb-4a55-aafc-4e5fda58b40c", 0xB372929B},  // 05_Over_xrayroom
    {"d1d6d9d2-cf72-4b28-82c4-22c2945789da", 0x2784DC47},  // 22_flaahgraChamber
    {"d2a93799-1173-44fe-a879-2d9895102097", 0x9644A054},  // 00d_Intro_connect
    {"d46bcc2c-6a03-4dd7-8222-f1f1f28c1fc5", 0x5AECC3E3},  // 05_Zoo
    {"d493f9f8-71be-4828-8b56-04a1c81cc22e", 0xA8439D72},  // 12_Ice_Research_B
    {"d5f23771-8d84-4aa3-ba0c-7467d3bd7436", 0xAF33BBB6},  // 3_monkey_upper
    {"d7a66600-615d-4b12-afbc-6e2f460249dd", 0x980D6664},  // 08_Mines
    {"d8b8986c-b46f-4ab9-9fed-9ebdb54c1f98", 0x9EE17789},  // 14_tl_base01
    {"d906ed85-4164-4e35-a8bb-069b806b15a9", 0xC88122F7},  // 07_Over_Stonehenge +5
    {"e2cee018-55b4-4d63-a593-4530a82be1cc", 0x70DB5FE4},  // 04_Ice_Boost_canyon +1
    {"e2da9a12-f6d7-40b6-9fd0-9a456545cd2e", 0x168D4892},  // 07_Mines_electric
    {"e4a7db3e-1eb5-4a32-bbcd-d71b9b3bab6a", 0x5E4647AB},  // 00k_ice_connect +7
    {"ea4d62a3-bbc5-425f-8996-c0eb9ad220e0", 0x61F0823E},  // 0c_connect_tunnel +8
    {"ea839ff8-3cac-418e-bf54-198792e7d250", 0x701B0D4E},  // 15_energycore
    {"eb806cff-4a11-4f21-88b2-ea96888c144f", 0xCD322DD4},  // 00g_ice_connect +8
    {"ecaa1a14-aa8f-49b1-afac-476a58d5390c", 0x0A7DFE25},  // 22_flaahgraChamber
    {"ecc9240a-da0d-409a-8d47-d39bf0d8a283", 0xACC3441A},  // 00F_intro_begin
    {"eea313db-233d-4305-9462-b9c7745debf4", 0xD9B81D49},  // 22_flaahgraChamber
    {"eea883f6-056c-41f9-8615-a3bffc8269a9", 0xD9BA0365},  // 5_bathhall
    {"f2ca3a48-8ddd-498f-bb2a-46cc08da54e7", 0xFA6EC61A},  // 12_Mines_eliteboss
    {"f66294d5-6b7d-40a8-aee4-74c24ce6884f", 0xA35E94DA},  // 12_Mines_eliteboss
    {"fc6996fc-6d6a-4f84-bb98-ec28e9909c1b", 0x807437B3},  // 07_Over_Stonehenge
    {"fdaff8f6-b669-4506-8838-286e7298f7cb", 0x5CF7E943},  // 0p_connect_tunnel
    {"fec1dea9-b8c3-4911-8579-1a38413c537a", 0x877E8A36},  // 15_energycore
    {"#chpr-set", 0},  // method of the entries below
    // By character: a CHPR's id is its retail ANCS's, and its dependency list
    // names its effects. Where only one of those is still unpaired, and the
    // ANCS (with its EVNTs) has only one PART not paired yet, they pair, if no
    // other CHPR naming the effect lacks that PART. Hiding a known pair, the
    // rule re-derives 9 of 9. The comment is the ANCS.
    {"217b7ae7-4536-4476-9a36-bbceae9ca9f8", 0x09884086},  // 52A3B1A4
    {"30f5e9c1-71ac-48db-8ae2-93afcafed315", 0x28630E4B},  // 3F21A526
    {"65528b78-0039-4001-8e66-d27ab86bec9e", 0xF02F1B9A},  // DEEE73AB
    {"76259eb8-f02b-4373-b489-f46b97d99817", 0x33000DC9},  // 09881302
    {"7a8fd475-8378-41f4-aa1b-0bd0fd57a9da", 0xE584FD20},  // 6397CC1B
    {"840ecc38-3260-44bb-997a-20b899b74d63", 0xFFB8ED2F},  // 23C00D8A
    {"85ab9b03-e4e1-4e8a-9f70-2e272414fb9b", 0xBD08A010},  // 8DC8052E
    {"cb9eb31d-2732-4ac2-970f-24e03a0adbff", 0xC6CBF848},  // 3C1C8CC1
    {"d33dbc9b-dede-45a5-b0c0-28120cb26266", 0xCE057D76},  // CBD06AA1
    {"f23d06b0-03de-46a1-8305-af2bebd69e7d", 0x9805E2E8},  // F19131AD
    {"#chpr-event", 0},  // method of the entries below
    // By event: a CHPR's action streams name the bone each effect spawns at,
    // and its event streams when (1/480 s ticks; build/fx-evt/NOTES.md). An
    // effect pairs with the one ANCS PART event at the same bone and frame, if
    // every CHPR naming it agrees and no other effect claims that PART. Hiding
    // a known pair, the rule re-derives 7 of 7 and gets none wrong. The comment
    // is the ANCS and the bone.
    {"098e846d-f9b3-45d9-b2f3-eac32c5e15dd", 0x977F7DE1},  // 76C35773 Skeleton_Root
    {"126ebc84-ac55-4919-8968-7b91a6ca8546", 0x140C923D},  // 76C35773 root
    {"1912ff84-5673-400f-98f3-5487e8667524", 0x0980B37E},  // 1E14B003 Head
    {"1b693d6f-1bcb-4fe6-b03a-0af9a2ab6683", 0xD6F1A5BD},  // 53171F8F L_leg_LCTR
    {"2113e818-e42a-4e9d-9988-b0051db4c4f5", 0xCD86BE35},  // 07BEED38 breastPlate_LCTR
    {"216d00f6-a404-4a75-8f0d-1f0861a3df1b", 0x162A9D46},  // BF5F05CD L_CLAW_LCTR
    {"24181da3-e82f-419b-8f9c-a246cc02406e", 0xB04B566B},  // EAD9FE87 Jaw_1
    {"2bdf9369-7313-43af-9ace-75b4fa2bcdb7", 0x56A8E002},  // 17C86CF2 breastPlate_LCTR
    {"2e89cfb5-b4a2-440d-938b-200191f9ce09", 0x359A520D},  // 17C86CF2 Jaw_1
    {"3357da26-0e84-41d2-a0c3-6f1f8d84239c", 0xC39164D8},  // 16DBF2CC Skeleton_Root
    {"34a51e8e-3dbd-4952-a8d1-aa9da87c3e7d", 0xC7F493C2},  // C98C5E5F LCTR_WARTAIL
    {"35530792-2e08-4a16-bef0-aa8e4fd61325", 0xB987E471},  // 7E4ABB02 Glow_LCTR
    {"392a5151-bf6b-45f3-afa8-ab7042a2d44a", 0xAF6EB124},  // BF5F05CD Head_1
    {"39be7331-230d-473f-ad43-23edc44b65b8", 0x6FD3877B},  // C98C5E5F Head_1
    {"3e472d60-cb07-4570-bd9e-5f84bcce5271", 0x02F2CABC},  // 16DBF2CC Skeleton_Root
    {"40a27ec3-4f81-4554-bef6-5392084e573b", 0x9727BC73},  // BF5F05CD Head_Box1_LCTR
    {"40ce2d7f-bfc6-4cdc-9435-c099f2cd2aae", 0x8AEF0FF3},  // 0FA35B51 L_ball
    {"44a0b713-5244-4662-b951-9794fac1300b", 0xD4147D06},  // 53171F8F Jaw_1
    {"4548980b-8000-4491-8383-fb8a0d2c5d97", 0x6911E609},  // 53171F8F F_R_leg_LCTR
    {"458582ad-255c-4eee-b47a-e4b0550f5edd", 0x3E1BEF77},  // 569523DF Front_Phazon_LCTR
    {"45a78f13-f8e7-4e8a-b9cf-ab6f88b41c0d", 0xE5AB0B1F},  // BF5F05CD R_Flap_LCTR
    {"4726a054-06bd-43d1-bf64-991b4a35523c", 0x1A4C9FAC},  // 020B6226 root
    {"4afe5c19-6f44-4fa1-90e8-15b0d4833fd7", 0x216E6BE3},  // 32FD91B0 breastPlate_LCTR
    {"4bd02360-01f1-4a41-991b-6ff3ba12b23a", 0x6580D0A3},  // 07BEED38 breastPlate_LCTR
    {"569ba50a-e0cb-4b86-9281-e2987ec68dd4", 0x5B69D65F},  // 17C86CF2 L_wingShoulder
    {"58488d63-ed74-4355-abb0-d453c7a787af", 0xFD58521A},  // 72E3722E root
    {"5f47c5e2-b993-4b3d-b798-afe88f6fb5a9", 0xB160F280},  // 3CB05DFE root
    {"60b7ece2-115c-4aa3-abe2-253c21ba657a", 0x68DEDED7},  // 16DBF2CC Skeleton_Root
    {"62e7afba-a1aa-4a49-a080-8174cb1f83d3", 0x01E9F7BA},  // 53171F8F L_eye_3
    {"63391b1e-2431-4c86-94cc-5598671d03ad", 0xAA56E290},  // 17C86CF2 L_wingElbow
    {"633c258f-d975-4af7-894f-209092e89795", 0x573E0000},  // 72E3722E rock_SDK
    {"6c322b7e-6078-44fa-8589-ef2017041511", 0x361CF076},  // 0FA35B51 Skeleton_Root
    {"6e3b0832-ab90-4a43-9520-3c382dd4c52e", 0x2ED5AF10},  // BF5F05CD Head_1
    {"6e839072-c8a4-4bd6-abc9-0db7f7280e49", 0xF69E5EF0},  // 72E3722E rock_SDK
    {"7433b396-8dc1-4c1c-916b-8325f65632c0", 0xDBF151EE},  // 76C35773 Skeleton_Root
    {"7f753ff7-8540-4f51-87c6-973a29380995", 0xEC3089F9},  // 3AD6D2ED Skeleton_Root
    {"85e42530-9710-45e8-aa91-86a67a3ad5fa", 0x44C87F1E},  // 3CB05DFE L_Bottom_Arm_09
    {"8945a196-7078-4342-80b4-1a48b1baffcd", 0x2633DD2A},  // 17C86CF2 L_wingShoulder
    {"896c4450-c3f7-4597-9f7b-02bd937fa898", 0xAE5D913C},  // 76C35773 root
    {"918facc0-4777-4ff3-a495-e148e8ef4d0a", 0x10D72CAE},  // 3CB05DFE root
    {"91e9ed9f-c0ef-4ace-ba77-44e197cfd548", 0xD724DD5A},  // 76C35773 root
    {"95126b13-84df-4b05-aa7d-6c058ce1d057", 0xC9437FAC},  // 7CB9D0AA Head_1
    {"9718be90-ce20-495e-98bf-432976b400f5", 0xE606E080},  // 06B034E2 L_shoulder
    {"99636462-3886-4adb-8f13-547fde3072c9", 0xC735ACF6},  // 3CB05DFE root
    {"9a109eb0-d801-4a2f-850d-7198d1eab2ff", 0xB870F59E},  // 76C35773 L_Hand_Collision_LCTR
    {"a1a4b921-c191-4107-b71f-e1677bb932e3", 0x31D17F5E},  // FAC657CC R_wingBone2_2
    {"#loose-events", 0},  // method of the entries below
    // Looser, with PARTs already paired taken out (repeated until nothing
    // changes): the same bones and at least half the frames (within one), or the
    // same frames on other bones, or shared bones and half the frames. Hiding a
    // pair from the two blocks above, each rule alone re-derives 46 to 67 of 83
    // and gets none wrong.
    {"0711875a-b194-4954-be1a-f9b4a6e04548", 0x1748E6C9},  // 53171F8F F_L_leg_LCTR
    {"163efe59-8886-4ea0-a538-c39315572139", 0xEDFC9872},  // BF5F05CD Jaw_1
    {"177ebe66-a31f-41c2-91f8-f71b52c773c2", 0xED5915A7},  // 17C86CF2 Jaw_1
    {"20a4bb26-32c4-4f77-8350-bf19667da9f3", 0x56A963B3},  // BF5F05CD Head_1
    {"2a1ec1c4-dc2f-4670-b4c7-2541a535d9e5", 0xF4148DCD},  // FAC657CC Jaw_1
    {"34f97b0c-bcdb-4728-85c9-635dbe22b53c", 0xC08B2FB1},  // A7C039CB L_Index_2
    {"3b01a3bf-b592-43b1-ba31-133ef2bd0760", 0x5196A2FC},  // 17C86CF2 root
    {"3b74644c-62df-4608-bdd1-c547f6b33b8a", 0x2BD6F224},  // 020B6226 head
    {"4d110001-1e20-49f6-8574-0d9f806c65e6", 0x54679833},  // 2D38C74A L_top_aft_engine_LCTR
    {"4d82e203-8d12-489a-85a7-bdbb68d7036f", 0xF3CF5B37},  // 32FD91B0 Head_1
    {"4ed39df9-6e90-46b4-9825-5242b9f4cd60", 0x0FCC468E},  // 76C35773 L_Foot_Collision_LCTR
    {"57bcbbde-e59d-408e-affc-3661111d6411", 0xD3CF9BEF},  // 3AD6D2ED Skeleton_Root
    {"5d048cd6-12d8-4765-95f9-b251b10d8a71", 0xDA8B9CA3},  // 76C35773 L_Hand_Collision_LCTR
    {"6e01119f-c953-4cf3-971a-7c336ddf1826", 0x208E6EAE},  // 0C6B791C root
    {"6edf8ea2-4c6c-4a72-8ec6-0f1771ec4901", 0xB696B078},  // 7E4ABB02 Light_LCTR
    {"731e1664-082c-458a-9012-5a5ece6bf247", 0x808F6894},  // 17C86CF2 L_index_1
    {"76ef45c3-9176-4e94-90a5-12f29a843cab", 0x89884F8F},  // EAD9FE87 Jaw_1
    {"7dd0805c-a6ec-4e87-af25-4aa6bae7aad5", 0x8AA772D7},  // BF5F05CD Jaw_1
    {"7e86ba34-f901-463a-895c-a50fe7ed06f2", 0x844AC944},  // FD49BDDE root
    {"86e1c8df-8609-407f-82f8-a19080cfa612", 0x0D8B6D9E},  // 76C35773 Collar
    {"92838da2-77c1-4588-af6e-2d9e288d85a1", 0xD7D47189},  // 76C35773 R_ankle
    {"955fde05-805b-45ed-aa02-521667e14bb8", 0xDC30BC0E},  // 06B034E2 Skeleton_Root
    {"97da18f8-a03d-4a45-baaa-8ce4d3195129", 0x50761EF5},  // 76C35773 L_wrist
    {"9d5bc064-4635-4410-bb8d-68953850c96f", 0x13273A3D},  // 0C6B791C root
    {"a486e31d-35bd-4e59-ac2e-c2cab060cbe0", 0x18D4E9BF},  // BF5F05CD Collar
    {"b3d39690-fe32-4cd5-b28b-6e5b25f8fb60", 0x2E1D99A6},  // 020B6226 LCTR_MAGMOUTH
    {"bbbfd5db-46e1-45d2-80a3-13b5cfbb1465", 0x24CAA0EC},  // 3AD6D2ED Skeleton_Root
    {"be0d651a-7b1e-4549-8a20-334e6b5bf2c4", 0x693740BA},  // 3AD6D2ED Skeleton_Root
    {"be10e48b-0395-4c71-bfdb-b61d9a09f008", 0x183E047C},  // 0E49E5F0 L_top_aft_engine_LCTR
    {"c737177a-5871-45e0-b643-866ea8ad623b", 0x4183C102},  // 17C86CF2 L_index_2
    {"cab0dc81-1a71-4e4c-94be-d5d3e3e3d069", 0x783C4BC6},  // A7C039CB root
    {"daa2f6aa-a874-4cbe-bed9-9054b158dd8d", 0xBF706DC5},  // 17C86CF2 L_wingBone1_1
    {"db9668b3-7ec5-45b8-93d0-ad9ff3781d87", 0x07EC5669},  // 3CB05DFE Skeleton_Root
    {"e44b68bf-456f-48fd-a684-ee82250f2c0d", 0x96C4A451},  // 3CB05DFE L_Bottom_Arm_01
    {"eaee1ff7-09b4-474f-880a-9fcb98dca3f9", 0xD73ECA76},  // 4082E602 root
    {"f2f77041-f8d9-4a52-bfea-876f681127df", 0xB7A03C19},  // 06B034E2 blendspace
    {"f4abd869-8dc7-409e-9758-3a1e1ea9eb85", 0x828D2134},  // 591F073D root
    {"faf8ecdd-5a7f-4be2-b561-bd8b771dfe7f", 0x583E2851},  // FD49BDDE Head_1
    {"a499159b-e326-4265-aaf9-f6d3a209a328", 0xA320529F},  // 16DBF2CC Skeleton_Root
    {"abb0fdb6-5171-4843-8053-f8fa21156247", 0xBBC7F86C},  // BF5F05CD L_Flap_LCTR
    {"ae27f67c-7095-4084-8cbc-c4ac1c426da2", 0x29247705},  // 3CB05DFE L_Bottom_Arm_09
    {"b0cf6c66-892a-43b9-88d7-e86a93a85b38", 0x950979BD},  // 17C86CF2 L_wingFlesh1_2
    {"bb71674f-9d98-4147-b55c-0c711bdc0045", 0x21A5ABB9},  // E3CBC3F3 Skeleton_Root
    {"bdffbab9-8dad-4d0b-83e2-f290f9f98619", 0xD4BD794E},  // 3CB05DFE Head
    {"bf0ea930-1f0a-41ec-a49c-ebfa86146974", 0xB0DF0A7F},  // 17C86CF2 L_wingFlesh2_2
    {"c78576e0-3e6e-49f2-a5c3-16cbbd6f9b41", 0xDEC9BD3D},  // 17C86CF2 breastPlate_LCTR
    {"caec1b20-9db4-439e-8fef-290c636538bc", 0xD95A367F},  // BF5F05CD root
    {"cbc5c158-4491-41e4-9132-d32c6e205f67", 0xCFB53F07},  // 28EACD5F Head
    {"cf1b8b42-e84e-491d-9cd8-72a992ad3801", 0xA6776019},  // 17C86CF2 breastPlate_LCTR
    {"d02728d2-7f66-4737-9f4d-086e550bb576", 0x372FA0B3},  // 76C35773 root
    {"d7513480-f790-41d5-9b99-a40d827c7f13", 0x564B1A1F},  // 16DBF2CC Skeleton_Root
    {"e20a8faf-b54b-4101-8499-d0d6447288ee", 0x71966562},  // 17C86CF2 Jaw_1
    {"e3aa9295-61af-415e-961d-53d4def7a294", 0xFE92DE4D},  // 17C86CF2 breastPlate_LCTR
    {"f1e2ad10-ea95-4afb-a70f-d3296ba9ab1b", 0x4E9DD9E4},  // 3AD6D2ED Skeleton_Root
    {"f2da15ca-65ba-492e-8d2f-511d3c909272", 0xAF8F6D95},  // 3CB05DFE L_Bottom_Arm_09
    {"f5be78e2-4ad1-4f8f-8c15-770935e35511", 0x5029FF1E},  // 020B6226 root
    {"fbfee463-1dec-4d67-bc51-c528a125043d", 0xD7FFF8D6},  // 020B6226 LCTR_MAGMOUTH
    {"fc6a3e0d-eb0d-4b81-99a9-e208ec1b80e4", 0x599B4D00},  // 76C35773 Collar
    {"fe324176-fcbe-480b-8437-fd19c3369f9e", 0x88460B49},  // 76C35773 root
    {"#script-slot", 0},  // method of the entries below
    // By script slot: Remastered keeps retail's property order, so a ROOM
    // object and the retail object it stands for name their assets in the same
    // order. Assets both sides share (retail ids, effects already paired) split
    // the lists; where a stretch holds as many fresh effects as PARTs, they pair
    // in order, if every placement agrees and no other effect claims the PART.
    // Hiding a known pair, the rule re-derives 169 of 193 and gets none wrong.
    // The comment is the object type (Effect: its first room).
    {"093b10b4-8369-4e5b-862c-e4940f0ec2e8", 0xD5FFFBEF},  // 08e_Intro_ventshaft
    {"15cb8a2d-4c60-4739-beb2-86e7a5efa1e6", 0xB4A9629E},  // 08c_IntroUnderwater_ventshaft
    {"18b63b6d-5d44-470e-a75f-30d46993ec9e", 0x6CD67D9A},  // 07_Intro_Reactor
    {"1dfb29c7-3f33-4342-858f-c47e14c084a8", 0x7A3C5665},  // 08c_IntroUnderwater_ventshaft
    {"2170745a-3842-44c7-8ece-993e05f7a1b0", 0xEBC23CB3},  // 00e_lava_connect
    {"30141f7b-2eb0-4ce8-83a0-ed444b63ea4e", 0xCDCBDF04},  // 00a_over_hall
    {"3aa2baaf-af8e-4034-a5a6-36494f8a64b8", 0x1F6DA1EA},  // 00F_intro_begin
    {"3b5f4d13-669e-4a52-8fb1-1d724e4d660a", 0x08471460},  // 04_Intro_Specimen_Chamber
    {"3bde3c65-1492-4a92-a4af-c3d0c5d29469", 0x5DA54EA1},  // 00o_over_hall
    {"48fd54ed-e1be-42c6-b1cd-e2fec746d599", 0xE5B4FF51},  // 08_Ice_Ridley
    {"495ebe20-a60b-4b46-bc21-423d4150ef98", 0x20CD21C0},  // 01_Crater_dental
    {"4c4a1627-df34-4c99-b64b-b9df0e7398a8", 0x91EB19CC},  // 19_hivetotem
    {"5f9cb3fa-ffd0-431b-aff0-8d5549ed0be6", 0xB1B6930D},  // 14_tl_base01
    {"606d4dbe-d811-4c58-80a0-4695cecd36d1", 0x29D7B22A},  // 03_IntroUnderwater_elevator
    {"6444187e-5fbb-4fb0-8b6e-0a145c2c4f05", 0x3F79ACD5},  // 0q_connect_tunnel
    {"68fc60a0-28cb-43a4-acc4-48b0b54c57f0", 0x7E31DDE4},  // 03f_Crater
    {"69226532-c70a-4012-9fff-08087660ad41", 0xA99706A8},  // 3_monkey_lower
    {"698470f0-5f62-480b-87a5-83f278a2fada", 0x26FE44B5},  // 00a_Intro_Connect
    {"6b8869b0-b4c8-4b85-b54a-4eb2b282d7b9", 0xC66B5353},  // 02_Intro_Epodroom
    {"6ba00c6c-2ff2-4306-bc84-18b9b15492cf", 0xE9F96B4F},  // 13_Ice_Vault
    {"6bc9d0b6-0db4-488a-9133-09411f20d2e9", 0x0640CE97},  // 06_IntroUnderwater_Freight_Lifts
    {"70495e24-5ec6-44d0-920c-3cff00f342e5", 0x72319B4A},  // 07_Mines_electric
    {"76b82718-fac1-46a3-9cf5-3451cf5f4157", 0x17037C39},  // 14_Over_magdolitepits
    {"776e68f9-97c1-4c21-844a-5176fa01c0ac", 0xF9F6487B},  // 04_Over_treeroom
    {"79aaa80c-3c65-4823-9d21-327d2ba9a31f", 0x46DB3BDD},  // 22_flaahgraChamber
    {"80e531c4-2a64-4d90-878f-03541a6d9a52", 0x368B5FFC},  // 11_Ice_Observatory
    {"82096cf3-de4e-41fd-978b-33117aad1d1c", 0xCE7E2B47},  // 01_Over_mainplaza
    {"84c1ba45-0c2b-4abb-9dcb-97d760de159b", 0xDAA45201},  // 02_Intro_Elevator
    {"9073b592-b836-4009-955e-c9f735b0dec1", 0xE7638EFC},  // 01_intro_hanger_connect
    {"90a43b46-810f-4904-8f61-f9ccfa757b12", 0x13E411AC},  // 00g_intro_end
    {"92278041-32a2-42ad-91c7-f8b8ee21f51c", 0x8FACD7B9},  // 00F_intro_end
    {"92728588-08d7-4d01-b712-2b42c57ea893", 0x318D6DF5},  // 02_Intro_Elevator
    {"a3535eeb-de3e-4054-bbaf-3777921025b2", 0xC4FFA551},  // 06_grapgallery
    {"a46dd9cb-261a-4705-9202-14a76dbdce15", 0x18F7A232},  // 01_intro_hanger
    {"a6212fb4-58ae-4735-80df-eaf4d0133e8d", 0xD7E33AFE},  // 02_Intro_Epodroom
    {"a91aeb69-9259-4bd4-870a-7cacea3c74f8", 0x5E8B5085},  // 13_Ice_Vault
    {"aebf8946-c086-42d0-9d45-bc487450694a", 0xDAD8ECC6},  // 07_IntroUnderwater_Reactor
    {"b2324517-6414-4523-8519-6f91d3b5b38d", 0x7EE18FB9},  // 02_Intro_Epod_connect
    {"b9a78207-6322-4eb4-ba99-106194b49eed", 0x9767EFBF},  // 02_Intro_Elevator
    {"ba39a268-64a3-473b-88bb-381bcec20374", 0x09431AEF},  // 00b_mines_connect
    {"bd005b65-6d40-4efe-9336-cd65730e66ba", 0xA5E77805},  // 02_Intro_Elevator
    {"bea21872-4a0c-46a8-8a29-7cf9cd0cff22", 0x8969EF5D},  // 00c_Intro_Connect
    {"c05c4d6b-8c7d-45a9-87ba-4c3fa23ce4f9", 0xCB8BEF0C},  // 08e_Intro_ventshaft
    {"c4f9d9f8-04bc-4d96-9181-186cdecafb5f", 0x645F1E29},  // pickup04
    {"c83812f1-7991-467f-ae9e-76f7830fdf48", 0x241B5F33},  // 12_Over_fieryshores
    {"da9caca5-3d33-4e14-a4d5-aa64862b7cfb", 0x7F51AB6E},  // 07_Intro_Reactor
    {"db9dd587-d5c7-4103-862e-7c1e33c16a59", 0xD0376F71},  // 01_Over_mainplaza
    {"e3493189-f7c8-4e27-8962-26cb53fc5451", 0xC4FFD4B2},  // 00a_Crater_connect
    {"eb976725-96cf-4fd2-a6d4-a3d474f9bc7f", 0x714E1050},  // 12_Ice_Research_B
    {"ec45a291-189d-46b6-9a04-2bb7dafb4f31", 0x50CDE26C},  // 03f_Crater
    {"f00a3f8c-e59f-4e01-9a2d-5dc71dbfc24e", 0x6A115193},  // 06_Ice_Temple
    {"f8d99f9a-8867-43c5-83f8-a0fba8714ea4", 0x9070DE4F},  // 08c_IntroUnderwater_ventshaft
    {"fd7e0746-5a5b-4a78-a475-2f5d436e278e", 0x333E7C73},  // 07_Over_Stonehenge
    {"d3d15f97-828e-47ca-ab66-c669a2b2ab1a", 0x42E15243},  // AtomicAlpha
    {"e5101ab6-aa7e-4eb7-ae71-cdf127457a15", 0x5921663E},  // AtomicAlpha
    {"324dabf4-eb4a-4104-a55e-161384437add", 0x6EC0042E},  // AtomicBeta
    {"23b454fc-a5b2-4571-bdba-20c0b11f4e0e", 0x31759A3C},  // Babygoth
    {"846e8db3-2064-4610-b8ef-7b5195c704b7", 0x5233BA60},  // Babygoth
    {"a2475797-c931-44b6-82fd-49a8bb2943c7", 0x691923FD},  // Babygoth
    {"bf610e68-de87-446d-bd2c-ecfbdba1ddb5", 0xA897FC3D},  // Babygoth
    {"8c294167-2cda-43c9-b8cf-57f071a9f363", 0xC051DFFF},  // BloodFlower
    {"d5795442-e5e6-42f4-8eb1-f237438e2124", 0xB266FF83},  // BloodFlower
    {"28e5da3b-c31d-47d0-b29b-52c5ca54c872", 0xE0501C06},  // Burrower
    {"454c8b6c-f132-4eba-81d8-6994044fa050", 0xACEF3F5D},  // Burrower
    {"52a91fc5-99ab-4962-868d-400f368f64ab", 0x01398949},  // Burrower
    {"735a6dd6-bee9-4e25-a993-4740a2d3f85f", 0x1A9D0DEA},  // Burrower
    {"93140a5b-f6da-4c0d-9270-35888dc36665", 0x0CFB676F},  // Burrower
    {"e8f976ad-cedf-4831-9e31-13239b4c8771", 0x847D852A},  // Burrower
    {"ef011137-f532-49ba-a7df-b58e420ec12d", 0x203A88F9},  // Burrower
    {"85acb4a6-6c52-45d9-962c-12bd87dd470c", 0x4AA0DF6C},  // ChozoGhost
    {"e53d7568-5879-435a-b81d-8a61e3bfb0b4", 0xC2C41C9B},  // ElectroMagneticPulse
    {"1f9ef401-3430-407a-8bc7-fe6637ed4d9c", 0x480427B0},  // ElitePirate
    {"615fc3ec-0d1e-4847-8bcf-9c58f8f6a78c", 0x206B6B9F},  // ElitePirate
    {"74702085-cded-4e98-8bed-21b1060c0ca4", 0x99C61631},  // ElitePirate
    {"b523a2e8-80d8-4acf-a72c-56f2964dd233", 0x817AD984},  // FishCloud
    {"69b4352b-cc1f-4320-85b6-cb703f5c607a", 0x340608AF},  // FlyingPirate
    {"7d9334ea-d2ec-4152-8130-333db7981444", 0x811E5BE1},  // FlyingPirate
    {"dddd60c9-14fb-4f80-9895-7aebed29045a", 0xF4474106},  // FlyingPirate
    {"f6f9a9b3-4e2f-4bbb-9221-093573cb80a7", 0x3990C442},  // FlyingPirate
    {"ed4013f4-51af-40f1-a21e-efcfc20eb8d9", 0xC337CB04},  // Geemer
    {"00eb9ff4-1027-4b50-9fed-8f236d3439ab", 0xD7FEA8FF},  // IceSheegoth
    {"2c88216b-00b6-4b5f-bf92-6493360de921", 0x6936FA7E},  // IceSheegoth
    {"56550a5b-a6a2-4de2-816d-e399f2657c5c", 0xA0607A0F},  // IceSheegoth
    {"a7a12365-0c19-4bd4-9752-2314f9870cd4", 0x8DD53B50},  // IceSheegoth
    {"62b018ec-4194-451d-a08f-81feb584dd9f", 0x0A7F48B0},  // IntroBoss
    {"672861f2-9808-4699-b230-59616dca69fd", 0x8F32DAC3},  // IntroBoss
    {"00da40bc-6792-4438-a6e5-85c87a964a8a", 0xC51599EA},  // Metaree
    {"1c8f937b-efb0-40b8-8ba4-08ec2f8d4c00", 0xCF575C49},  // MetroidBeta
    {"2d0d889f-bfd9-4f2d-99e6-d19a064f98da", 0x99E75743},  // MetroidBeta
    {"5cd3b522-958d-420e-847c-e75aaf3e80cf", 0x50138D00},  // MetroidBeta
    {"dd72d2c5-1b1f-409f-8c08-b40fdd88c5b3", 0x762675DC},  // MetroidBeta
    {"0b2b2873-5d4d-48e4-9f03-d5fc02add085", 0x0901E15E},  // MetroidPrimeStage2
    {"df506540-33a5-4785-a266-d4648d5434a0", 0xEA7CE6D8},  // MetroidPrimeStage2
    {"c14800ac-a1d1-4dd6-a339-217381b67090", 0xCDBD7E5F},  // Oculus
    {"12973d8f-c07f-4572-8b7b-204ee684d827", 0x2B351F55},  // OmegaPirate
    {"1338e07e-c795-48f5-a8c7-88640d4efa77", 0xDFA91316},  // OmegaPirate
    {"1f0d26b5-a484-4fce-9801-ced3f4235f20", 0xDCF9D2BE},  // OmegaPirate
    {"7d5daf2e-cd35-415e-8b53-d811df153a84", 0xF5024DEE},  // OmegaPirate
    {"38e0274f-bcc5-410f-9b80-961479454634", 0x0DEB9456},  // Pickup
    {"25a394c4-c6c2-449c-b378-4ab36005c7b6", 0x2015A7AC},  // Ridley
    {"3aa6b3fc-ed3c-4c1e-b60a-acc7e5eaf43f", 0x32F1D071},  // Ridley
    {"a1bda6a0-194f-4932-89e2-7dd925ff2898", 0xF801A6EB},  // Ridley
    {"fa528a35-867f-41b8-97fb-0b7087b7788a", 0x5DFD0030},  // Seedling
    {"11f74dd3-563d-46b6-b693-da2227e4d6c5", 0x0170B882},  // Thardus
    {"42140849-c385-4785-b082-42937ee555a3", 0xC92D76F7},  // Thardus
    {"4678cde9-cea7-4154-a73b-13ddc4732ac9", 0xC6B0A48F},  // Thardus
    {"a94e6e0c-bf4a-47a2-a49e-9ae099942a63", 0x14BAF4D9},  // Thardus
    {"af722e2f-c65e-4215-af10-441b8d0cafed", 0xBEC30635},  // Thardus
    {"f1925c77-c4ea-4d13-a260-5d5f01681613", 0xED72DE0E},  // Thardus
    {"17ca73ea-250a-4252-a169-53f37c5f64db", 0x5415B157},  // VisorGoo
    {"11fb9d0d-1d3e-4b0a-b440-4952bdb196ef", 0x50B4A51A},  // Water
    {"98eee214-a6f1-43f3-82a9-6b9e4859fd79", 0x69B9387A},  // Water
    {"afa4cef5-24e4-4133-b62b-5e3e52068b25", 0x147A85EF},  // Water
    {"d911be03-8d3a-4a07-bcd6-9158fd9fad40", 0xE981C0AD},  // Water
    {"#event-bones-per-character", 0},  // method of the entries below
    // By event bones, one character at a time: Remastered reuses an effect for
    // different retail PARTs in different characters, so an effect may pair
    // more than once. In a CHPR, an unpaired effect and an unpaired PART of its
    // ANCS pair when their event bone sets are the same and no other effect or
    // PART there has that set (or, among several, their frames match within one
    // and only each other), any frames on both sides meet within one, and every
    // ANCS using the PART pairs it the same way (repeated until nothing changes).
    // Hiding a known pair, the rule re-derives 75 of 135 and gets none wrong.
    // The comment is the ANCS and the bones.
    {"17ea72c7-6430-4895-a668-ebf5b8886ae5", 0x65FA796C},  // 28EACD5F L_eye_LCTR_SDK
    {"1cc665ba-8b5d-4fb5-9b53-a96b1fe1317a", 0x939E8643},  // 569523DF Head_1
    {"3ec1f3bd-e45d-4fac-99ed-488537af39c3", 0x07C89D11},  // BE756DF9 loc*_special_LCTR
    {"3ec1f3bd-e45d-4fac-99ed-488537af39c3", 0x0E8A8E7B},  // 61F99B76 Rock_01_*
    {"3ec1f3bd-e45d-4fac-99ed-488537af39c3", 0x8E9224AB},  // 32FD91B0 Jaw_1
    {"3ec1f3bd-e45d-4fac-99ed-488537af39c3", 0xC3C364E6},  // 591F073D Head_LCTR (+1)
    {"3ec1f3bd-e45d-4fac-99ed-488537af39c3", 0xF05FA09E},  // FAC657CC L/R_wingBone2_2
    {"969b554a-81f2-41fb-98ac-351f5bab1cd8", 0xD8EFB226},  // 7814DC79 lockon_target_LCTR
    {"a56432d1-e257-444b-ab2b-abba3c2f9ebd", 0x045912BB},  // EAD9FE87 GillR_LCTR
    {"a6dfc131-b63c-4c62-909c-6485d73315cc", 0xA33FDBCB},  // 1E14B003 Skeleton_Root
    {"a9c3b54e-3fa6-4b90-ae65-ddad3a0c4409", 0x4CCE514A},  // 7E4ABB02 Glow_LCTR
    {"#event-frames-per-character", 0},  // method of the entries below
    // By event frames alone, whatever the bones, one character at a time: an
    // effect and a PART whose frames meet (within one) at least twice on both
    // sides, covering half of either, pair if neither has another such partner
    // there and every ANCS naming both agrees (repeated until nothing changes).
    // Hiding a known pair, the rule re-derives 33 of 135 and gets none wrong.
    // The comment is the ANCS and the effect's frames.
    {"1b169291-8359-43bb-ab36-e88341344f7a", 0xEE013D1B},  // 020B6226 10 12 43 75
    {"4c8ffbcd-ec17-4438-9210-317fbb8afa3a", 0x15856C7B},  // 76C35773 1 24 246 626
    {"6bd19865-335b-4d8d-baaf-70522d2e17e7", 0x879D77F9},  // 76C35773 68 96 ... 258
    {"6dfd9cba-a857-4f6d-ac49-13a16ec3decb", 0x26379D78},  // 591F073D 4 6 7 ... (+1)
    {"7f32b829-f973-453a-8391-9d4a5c40d38d", 0x1362BBA1},  // 76C35773 60 234 380
    {"c6b710b7-641a-48b6-8e2d-6a64b375c0e1", 0x66916F48},  // 0E49E5F0 14 19 28 39
    {"ffda5e7f-1a9f-4acb-a5b8-dbe83791dd2e", 0x898E2B24},  // FD49BDDE 18 23 976 981
    {"#action-name", 0},  // method of the entries below
    // The six CHPRs that carry a CActionData names block name each effect
    // slot (retail ANCS names them the same way); the slot's bone is a
    // retail locator, so the name and bone pick the PART. Re-derives the 15
    // known slots with no wrong one (build/fx-name/vote.py). 74d36fa3 also
    // sits on D3BD37FC in one slot, where 4d818e87 holds it in four: left out.
    {"24a6477f-84e7-4183-9fe4-7c939d430e67", 0xDE82C4CF},  // 2ECB9FBF ScubaBubbles
    {"53941a1a-f87b-422c-a4c3-b4a891d4c730", 0xCF07A1F9},  // 2ECB9FBF ScubaGear
    {"3bf15154-3b64-43af-9a09-9d407e186ebe", 0x66CD9E79},  // 2ECB9FBF Eyes
    {"45f196c2-0073-42bb-abde-13d7231aff66", 0x235E23AD},  // 2ECB9FBF JetPack
    {"2aef03af-ea67-4e3d-9fff-e5936472ba91", 0x7579513D},  // 2ECB9FBF LandingSmoke
    {"cdf30852-2632-4596-9dd1-e85e2908daea", 0x4A5F51FD},  // 65177C3A Fire1/Fire2
    {"4d818e87-ce9b-4581-aeba-e8155b813082", 0xD3BD37FC},  // 65177C3A Fire2/Fire3
    {"74d36fa3-690a-456e-9469-09b97fcc554a", 0x1233E83C},  // 65177C3A Fire3
    {"00aadc3b-c393-4e1b-b379-2b114bc1bbe8", 0x9884632F},  // 6B45F06D Eyes
    {"cc86c15d-0f2a-4807-a727-eda4723f3a9c", 0x273375E3},  // 8169653A TwoEyes
    {"#by-hand", 0},  // method of the entries below
    // By hand, from the events (build/fx-hand/view.py) and textures (sheet.py).
    // Ridley's jaw effect: the same frames as 4B55EA17 in 07BEED38 (26 42),
    // 32FD91B0 (22) and 17C86CF2; FA049A5D's 4c971de3 also matches it (169
    // 240), but a PART takes one effect, and three ANCS agree on this one.
    {"500b48b1-0aff-4c0d-a1da-0fa8697f45d1", 0x4B55EA17},  // 07BEED38 Jaw_1 (+2)
    // 0C6B791C's ankle effect fires at 40, 77, 214, 252: both footstep PARTs
    // (root, at 40 and at 77).
    {"180da4fe-b8f9-46ab-baf7-ac2cf4b0ff2e", 0x1E9EC128},  // 0C6B791C 40
    {"180da4fe-b8f9-46ab-baf7-ac2cf4b0ff2e", 0x2FC54239},  // 0C6B791C 77
    // The gunships' Smoke1-4 effect: the only one left on those bones whose
    // frames fit, in both ships.
    {"6a356076-36ce-4954-bd69-2d752a33a8ee", 0xF7DCC380},  // 0E49E5F0 Smoke1-4 (+1)
    // Thardus: an electric-arc effect at 135, 450, 712, 904; the two arc PARTs
    // fire at 135 450 and 712 904 (the third PART at 134 450 is dust).
    {"948965e4-288a-4aec-a1ff-be3349f813e6", 0x0BBA6CF8},  // 76C35773 135 450
    {"948965e4-288a-4aec-a1ff-be3349f813e6", 0x2281797B},  // 76C35773 712 904
    // Ridley's breastplate effects, by animation: one fires c3a53327 with the
    // jaw effect at 0, as retail EVNT BA4796AE fires 2D16014C with 4B55EA17;
    // another fires 95964af3 and d701c231 with it, as D640D413 fires D61A362F
    // and FD17C2C9. d701c231's five model generators are FD17C2C9's five model
    // children; 95964af3 is a stub (it stands for 53861B29 too), so D61A362F
    // keeps the disc's.
    {"c3a53327-731d-4870-8f1e-fdc755bccf13", 0x2D16014C},  // 07BEED38 breastPlate_LCTR
    {"d701c231-cf56-4451-b177-107724371c42", 0xFD17C2C9},  // 07BEED38 breastPlate_LCTR
};

// Retail PARTs whose Remastered effect is a stub because Remastered draws them
// elsewhere: the stub replaces them rather than the disc's being kept. Retail's
// Artifact Temple laser hit spawns the same five PARTs as Ridley's 2D16014C
// (which carries them now, as c3a53327), so retail draws them twice.
// Remastered shows no particle flash on a plasma shot: PlasmaMuzzle and
// PlasmaAuxMuzzle are stubs and no other code draws one (kb
// topic/nil-texture-placeholder).
const std::set<uint32_t> kStubReplaces = {0x53861B29, 0x8D7BBFB2, 0xB0F9DBE6};

// A retail PART an effect replaces, and the rule that paired them.
struct Pairing {
  uint32_t retail;
  std::string method;
};

// The retail PARTs an effect replaces: the id it carried over, else its
// matches' (one effect can stand for several PARTs).
std::vector<Pairing> RetailEffects(const EffectGuid& id, const std::vector<EffectPairing>& found) {
  if (const std::optional<uint32_t> retail = EffectRetailId(Swap(id))) {
    return {{*retail, "carried-over"}};
  }
  const std::string text = EffectGuidString(Swap(id));
  std::vector<Pairing> out;
  std::string method;
  for (const MatchedEffect& matched : kMatchedEffects) {
    if (matched.id[0] == '#') {
      method = matched.id + 1;
    } else if (text == matched.id) {
      out.push_back({matched.retail, method});
    }
  }
  for (const EffectPairing& pairing : found) {
    if (pairing.id != id) {
      continue;
    }
    const bool known = std::any_of(out.begin(), out.end(), [&](const Pairing& have) { return have.retail == pairing.retail; });
    if (!known) {
      out.push_back({pairing.retail, pairing.method});
    }
  }
  return out;
}

class Importer {
public:
  explicit Importer(const EffectImportIO& io) : m_io(io) {}

  void Log(const std::string& line) const {
    if (m_io.log) {
      m_io.log(line);
    }
  }

  // The id a Remastered texture (pak order) is written under, converting it
  // the first time; 0 when it cannot be.
  // Writes a texture under id. One over kStubSide is written as a native .dds,
  // with a small TXTR standing in for it on the game's heap (as the HUD import
  // does): a 2048 atlas is 22 MB as an RGBA8 TXTR, and the gun loads every
  // beam's effects at once.
  bool WriteTexture(uint32_t id, const Image& image, MapKind kind = MapKind::Colour) {
    const int edge = std::max(image.width, image.height);
    if (edge <= kStubSide) {
      return m_io.write(Hex(id) + ".TXTR", EncodeTxtrRgba8(image));
    }
    const Image stub = Resize(image, std::max(8, RoundUp4(image.width * kStubSide / edge)),
                              std::max(8, RoundUp4(image.height * kStubSide / edge)), kind);
    return m_io.write(Hex(id) + ".dds", EncodeDds(image, ColourDdsFormat(), false, kind)) &&
           m_io.write(Hex(id) + ".TXTR", EncodeTxtrRgba8(stub));
  }

  // `vfx`: the texture is sampled by a VFX material, whose shader reads it raw. Remastered's
  // sRGB textures reach its shaders decoded, so those are written as linear values (under an
  // id of their own, the legacy path wants the sRGB bytes) and filtered as data.
  uint32_t Texture(const EffectGuid& id, bool vfx = false) {
    vfx = vfx && m_io.textureSrgb && m_io.textureSrgb(id);
    auto& cache = vfx ? m_vfxTextures : m_textures;
    const auto known = cache.find(id);
    if (known != cache.end()) {
      return known->second;
    }
    uint32_t out = 0;
    Image image;
    std::string error;
    if (!m_io.texture || !m_io.texture(id, image.width, image.height, image.rgba, error)) {
      Log("effect texture " + EffectGuidString(Swap(id)) + ": " + error);
    } else {
      int width = image.width;
      int height = image.height;
      while (width > kMaxTextureSide || height > kMaxTextureSide) {
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
      }
      width = RoundUp4(width);
      height = RoundUp4(height);
      if (width != image.width || height != image.height) {
        image = Resize(image, width, height);
      }
      if (vfx) {
        for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
          for (size_t c = 0; c < 3; ++c) {
            image.rgba[i + c] = SrgbToLinearByte(image.rgba[i + c]);
          }
        }
      }
      out = m_io.freshId(Hash(id, vfx ? kTxtr ^ 0x5F1Du : kTxtr));
      if (WriteTexture(out, image, vfx ? MapKind::Data : MapKind::Colour)) {
        ++m_result.textures;
      } else {
        out = 0;
      }
    }
    cache.emplace(id, out);
    return out;
  }

  // An array texture's layers packed into one atlas TXTR, row-major from the
  // top: a power-of-two column count, and frames halved only while an edge is
  // over kMaxAtlasSide. Id 0 when it cannot be.
  // `vfx`: as for Texture(), a linear-light copy (under an id of its own) for a VFX material.
  FlipbookAtlas Flipbook(const EffectGuid& id, bool vfx = false) {
    vfx = vfx && m_io.textureSrgb && m_io.textureSrgb(id);
    auto& cache = vfx ? m_vfxFlipbooks : m_flipbooks;
    const auto known = cache.find(id);
    if (known != cache.end()) {
      return known->second;
    }
    FlipbookAtlas out;
    int width = 0, height = 0, layers = 0;
    std::vector<uint8_t> rgba;
    std::string error;
    if (!m_io.layers || !m_io.layers(id, width, height, layers, rgba, error)) {
      Log("effect flipbook " + EffectGuidString(Swap(id)) + ": " + (m_io.layers ? error : "no layer reader"));
    } else if (layers < 1 || width < 1 || height < 1 || rgba.size() != size_t(width) * height * layers * 4) {
      Log("effect flipbook " + EffectGuidString(Swap(id)) + ": no layers");
    } else {
      int cols = 1;
      while (cols * cols < layers) {
        cols *= 2;
      }
      const int rows = (layers + cols - 1) / cols;
      int frameW = width;
      int frameH = height;
      while (cols * frameW > kMaxAtlasSide || rows * frameH > kMaxAtlasSide) {
        frameW = RoundUp4(frameW / 2);
        frameH = RoundUp4(frameH / 2);
      }
      Image atlas;
      atlas.width = cols * frameW;
      atlas.height = rows * frameH;
      atlas.rgba.assign(size_t(atlas.width) * atlas.height * 4, 0);
      for (int k = 0; k < layers; ++k) {
        Image frame;
        frame.width = width;
        frame.height = height;
        const uint8_t* src = rgba.data() + size_t(k) * width * height * 4;
        frame.rgba.assign(src, src + size_t(width) * height * 4);
        if (frameW != width || frameH != height) {
          frame = Resize(frame, frameW, frameH);
        }
        const int x0 = (k % cols) * frameW;
        const int y0 = (k / cols) * frameH;
        for (int y = 0; y < frameH; ++y) {
          std::memcpy(atlas.rgba.data() + (size_t(y0 + y) * atlas.width + x0) * 4,
                      frame.rgba.data() + size_t(y) * frameW * 4, size_t(frameW) * 4);
        }
      }
      if (vfx) {
        for (size_t i = 0; i + 3 < atlas.rgba.size(); i += 4) {
          for (size_t c = 0; c < 3; ++c) {
            atlas.rgba[i + c] = SrgbToLinearByte(atlas.rgba[i + c]);
          }
        }
      }
      const uint32_t fresh = m_io.freshId(Hash(id, kTxtr ^ (vfx ? 0xF11Cu : 0xF11Bu)));
      if (WriteTexture(fresh, atlas, vfx ? MapKind::Data : MapKind::Colour)) {
        ++m_result.textures;
        ++m_result.flipbooks;
        out = FlipbookAtlas{fresh, cols, rows, layers};
      }
    }
    cache.emplace(id, out);
    return out;
  }

  // The id a Remastered-only model (pak order) is written under, converting
  // it the first time; 0 when it cannot be.
  uint32_t Model(const EffectGuid& id) {
    const auto known = m_models.find(id);
    if (known != m_models.end()) {
      return known->second;
    }
    uint32_t out = 0;
    std::string error;
    if (m_io.model) {
      out = m_io.freshId(Hash(id, kCmdl));
      if (m_io.model(id, out, error)) {
        ++m_result.models;
      } else {
        Log("effect model " + EffectGuidString(Swap(id)) + ": " + error);
        out = 0;
      }
    }
    m_models.emplace(id, out);
    return out;
  }

  // The retail id for an id as an effect stores it: the disc's own when it
  // was carried over from retail, else a converted texture's or model's.
  uint32_t Stored(const EffectGuid& stored, uint32_t type) {
    const std::optional<uint32_t> retail = EffectRetailId(stored);
    if (retail && m_io.retailId(*retail)) {
      return *retail;
    }
    const EffectGuid id = Swap(stored);
    if (type == kTxtr && m_io.typeOf(id) == kTxtr) {
      return Texture(id);
    }
    if (type == kCmdl && m_io.typeOf(id) == kCmdl) {
      return Model(id);
    }
    return 0;
  }

  // The first texture a material instance names that converts.
  uint32_t Material(const EffectGuid& stored) {
    std::vector<uint8_t> data;
    std::string error;
    if (!m_io.read(kMati, Swap(stored), data, error)) {
      return 0;
    }
    for (size_t at = 0; at + 16 <= data.size(); ++at) {
      EffectGuid id;
      std::memcpy(id.data(), data.data() + at, 16);
      if (m_io.typeOf(Swap(id)) == kTxtr) {
        if (const uint32_t texture = Stored(id, kTxtr)) {
          return texture;
        }
      }
    }
    // One whose colour texture is nil draws Remastered's black texture (VfxTexture()).
    static constexpr uint8_t kNilColour[20] = {'B', 'C', 'L', 'R'};
    if (std::search(data.begin(), data.end(), std::begin(kNilColour), std::end(kNilColour)) != data.end()) {
      return VfxTexture(EffectGuid{}).id;
    }
    return 0;
  }

  // A texture a VMAT slot draws, as the id of a TXTR the import writes (the disc's
  // own when it was carried over), with its atlas layout. An array texture is
  // packed like a flipbook; any other is a single tile.
  // A nil id is Remastered's black texture: its MATI loader binds
  // CGraphicsUtilCache::GetBlackTexture(), opaque black (0, 0, 0, 1), for a nil
  // texture (VFX_Color_Unlit's BCLR in 64 MATIs, PlasmaCharge's among them).
  FlipbookAtlas VfxTexture(const EffectGuid& stored) {
    if (stored == EffectGuid{}) {
      if (m_black == 0) {
        Image black;
        black.width = black.height = 4;
        black.rgba.assign(4 * 4 * 4, 0);
        for (size_t i = 3; i < black.rgba.size(); i += 4) {
          black.rgba[i] = 0xFF;
        }
        const uint32_t fresh = m_io.freshId(Hash(stored, kTxtr ^ 0xB1ACu));
        if (WriteTexture(fresh, black, MapKind::Data)) {
          ++m_result.textures;
          m_black = fresh;
        }
      }
      return m_black != 0 ? FlipbookAtlas{m_black, 1, 1, 1} : FlipbookAtlas{};
    }
    const std::optional<uint32_t> retail = EffectRetailId(stored);
    if (retail && m_io.retailId(*retail)) {
      return FlipbookAtlas{*retail, 1, 1, 1};
    }
    const EffectGuid id = Swap(stored);
    if (m_io.typeOf(id) != kTxtr) {
      return {};
    }
    int width = 0, height = 0, layers = 0;
    std::vector<uint8_t> rgba;
    std::string error;
    if (m_io.layers && m_io.layers(id, width, height, layers, rgba, error) && layers > 1) {
      return Flipbook(id, true);
    }
    const uint32_t texture = Texture(id, true);
    return texture != 0 ? FlipbookAtlas{texture, 1, 1, 1} : FlipbookAtlas{};
  }

  // A material instance's MATI file, or empty.
  std::vector<uint8_t> MaterialData(const EffectGuid& stored) {
    std::vector<uint8_t> data;
    std::string error;
    if (!m_io.read(kMati, Swap(stored), data, error)) {
      data.clear();
    }
    return data;
  }

  // Each embedded child's fresh id, and the retail type it converts to (0 for
  // a form that is not converted, so nothing resolves to it).
  struct Child {
    uint32_t id;
    uint32_t type;
  };

  void Children(const EffectNode& node, uint32_t root, std::map<EffectGuid, Child>& out) {
    for (const EffectNode& child : node.children) {
      out.emplace(child.id, Child{m_io.freshId(Hash(child.id, root)), EffectRetailType(child.form)});
      Children(child, root, out);
    }
  }

  // Hands one finished row to the report callback.
  void Report(EffectReportRow& row, const char* result, const std::string& reason = {}) {
    row.result = result;
    row.reason = reason;
    if (m_io.report) {
      m_io.report(row);
    }
  }

  void Effect(const EffectGuid& id, const Pairing& pairing) {
    const uint32_t retail = pairing.retail;
    EffectReportRow row;
    row.genp = EffectGuidString(Swap(id));
    row.retail = retail;
    row.method = pairing.method;
    if (!m_io.retailId(retail)) {
      Report(row, "no-disc-part");
      return;
    }
    ++m_result.candidates;
    // A standalone swoosh (SWSH) replaces a retail SWHC, a GENP a PART.
    const bool swoosh = m_io.typeOf && m_io.typeOf(id) == kSwsh;
    const uint32_t rootType = swoosh ? EffectFourCC("SWHC") : kPart;
    const std::string name = Hex(retail) + "." + EffectFourCCString(rootType);
    std::vector<uint8_t> data;
    std::string error;
    EffectNode effect;
    if (!m_io.read(swoosh ? kSwsh : kGenp, id, data, error) || !ParseEffect(data.data(), data.size(), effect, error)) {
      ++m_result.failed;
      Log(name + ": " + error);
      Report(row, "failed", "parse: " + error);
      return;
    }
    std::map<EffectGuid, Child> children;
    Children(effect, retail, children);
    EffectConvertIO io;
    io.assetId = [&](const EffectGuid& stored, uint32_t type) -> uint32_t {
      const auto child = children.find(stored);
      if (child != children.end() && child->second.type == type) {
        return child->second.id;
      }
      return Stored(stored, type);
    };
    io.materialTexture = [&](const EffectGuid& material) { return Material(material); };
    io.flipbook = [&](const EffectGuid& stored) { return Flipbook(Swap(stored)); };
    io.materialData = [&](const EffectGuid& material) { return MaterialData(material); };
    io.vfxTexture = [&](const EffectGuid& stored) { return VfxTexture(stored); };
    if (m_io.modelMesh) {
      io.modelMesh = [&](uint32_t model) { return m_io.modelMesh(model); };
    }
    const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
    std::vector<RetailPartProperty> check;
    if (parts.empty() || parts[0].type != rootType ||
        !SplitRetailEffect(rootType, parts[0].part.data(), parts[0].part.size(), check, error)) {
      ++m_result.failed;
      const std::string why = "the converted effect does not read as a " + EffectFourCCString(rootType) +
                              (error.empty() ? "" : " (" + error + ")");
      Log(name + ": " + why);
      Report(row, "failed", why);
      return;
    }
    for (const ConvertedPart& part : parts) {
      const std::string kind = EffectFourCCString(part.type);
      for (const std::string& line : part.dropped) {
        row.droppedList.push_back(line);
      }
      for (const std::string& line : part.approximated) {
        row.approximatedList.push_back(line);
      }
      row.dropped += part.droppedRetail;
      const size_t at = row.kinds.find(kind + ":");
      if (at == std::string::npos) {
        row.kinds += (row.kinds.empty() ? "" : ",") + kind + ":1";
      } else {
        const size_t end = row.kinds.find(',', at);
        const int count = std::atoi(row.kinds.c_str() + at + 5);
        row.kinds.replace(at + 5, (end == std::string::npos ? row.kinds.size() : end) - (at + 5),
                          std::to_string(count + 1));
      }
    }
    // A part that lost its texture or model would replace the disc's textured
    // effect with an invisible one: keep the disc's, before anything is written.
    for (const ConvertedPart& part : parts) {
      const EffectNode* node = part.root ? &effect : FindNode(effect, part.id);
      if (node != nullptr && !part.placeholder &&
          SplitRetailEffect(part.root ? rootType : kPart, part.part.data(), part.part.size(), check, error) &&
          LostLook(*node, check)) {
        ++m_result.failed;
        Log(name + ": " + (part.root ? std::string("the root") : "child " + EffectGuidString(part.id)) +
            " has no texture, the disc's is kept");
        Report(row, "failed", "no texture: " + std::string(part.root ? "root" : "child " + EffectGuidString(part.id)));
        return;
      }
    }
    // An effect drawn only by placeholder materials would replace the disc's with
    // an invisible one: keep the disc's unless Remastered draws nothing there too.
    if (kStubReplaces.count(retail) == 0 && std::any_of(parts.begin(), parts.end(), [](const ConvertedPart& part) { return part.placeholder; }) &&
        std::all_of(parts.begin(), parts.end(), [](const ConvertedPart& part) { return part.drawsNothing; })) {
      ++m_result.failed;
      Log(name + ": only placeholder materials draw, the disc's is kept");
      Report(row, "failed", "draws nothing: its materials are placeholders");
      return;
    }
    // Children first, so the root never names a child that was not written.
    for (size_t i = 1; i < parts.size(); ++i) {
      const auto child = children.find(parts[i].id);
      if (child == children.end() ||
          !SplitRetailEffect(parts[i].type, parts[i].part.data(), parts[i].part.size(), check, error) ||
          !m_io.write(Hex(child->second.id) + "." + EffectFourCCString(parts[i].type), parts[i].part)) {
        // The root would name a missing child: leave the disc's PART in place.
        ++m_result.failed;
        Log(name + ": child " + EffectGuidString(parts[i].id) + " not written");
        Report(row, "failed", "child " + EffectGuidString(parts[i].id) + " not written");
        return;
      }
      ++m_result.parts;
      m_result.dropped += parts[i].droppedRetail;
    }
    std::vector<uint8_t> root = parts[0].part;
    std::vector<RetailPartProperty> converted;
    std::vector<RetailPartProperty> disc;
    std::vector<uint8_t> discData;
    if (!swoosh && m_io.retail && SplitRetailPart(root.data(), root.size(), converted, error) && HasLight(converted) &&
        m_io.retail(kPart, retail, discData) &&
        SplitRetailPart(discData.data(), discData.size(), disc, error) && HasLight(disc)) {
      root = WithDiscLight(converted, disc);
      Log(name + ": light from the disc");
    }
    if (!m_io.write(name, root)) {
      ++m_result.failed;
      Log(name + ": could not write it");
      Report(row, "failed", "could not write it");
      return;
    }
    ++m_result.parts;
    ++m_result.written;
    m_result.dropped += parts[0].droppedRetail;
    Report(row, "imported");
  }

  EffectImportResult Run() {
    for (const EffectGuid& id : m_io.effects) {
      const std::vector<Pairing> pairings = RetailEffects(id, m_io.pairings);
      if (pairings.empty() && m_io.report) {
        EffectReportRow row;
        row.genp = EffectGuidString(Swap(id));
        row.method = "none";
        Report(row, "unpaired");
      }
      for (const Pairing& pairing : pairings) {
        Effect(id, pairing);
      }
    }
    return m_result;
  }

private:
  const EffectImportIO& m_io;
  std::map<EffectGuid, uint32_t> m_vfxTextures;  // the same, linear-light copies for VFX materials
  std::map<EffectGuid, uint32_t> m_textures;  // by Remastered id, 0 for one that failed
  std::map<EffectGuid, FlipbookAtlas> m_flipbooks;
  std::map<EffectGuid, FlipbookAtlas> m_vfxFlipbooks; // linear-light copies for VFX materials
  uint32_t m_black = 0;                                // VfxTexture()'s black texture, once written
  std::map<EffectGuid, uint32_t> m_models;
  EffectImportResult m_result;
};

}  // namespace

namespace {
std::atomic<bool> sEffects{false};
}  // namespace

bool WantsRemasteredEffects() {
  return port::EnvFlag("MP_REMASTERED_EFFECTS", sEffects.load());
}

void SetImportEffects(bool on) { sEffects = on; }

EffectImportResult ImportEffects(const EffectImportIO& io) { return Importer(io).Run(); }

}  // namespace PortRemastered
