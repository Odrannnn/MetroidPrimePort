"""Production wiring guard for the BarrierShield feature (aurora::gfx::vfx::BarrierShield, 1u << 29).

The barrier's refraction is one bit that has to mean the same thing in four places: the aurora enum
(and its WGSL branch), the converter's recipe table, the platform's mesh feed and readiness check, and
the vertex layout the fragment's mesh colour arrives on. A feature wired into only some of them draws
nothing, draws the wrong perm, or feeds a zero mesh colour, and none of that fails the C++ tests: they
pin the converter's word for one material and the scalar maths, not the plumbing between them.

This reads the actual sources (no engine code runs) and asserts the pieces are all present, that the
bit is 29 and unused by any other Feature or converter recipe, and that the recipe requires the
material flag MFC0 and is the only row carrying kBarrierShield. build/s-queenbarrier-20261024/
SHADER-IMPLEMENTATION.md is the contract; tests/port_vfx_barrier.cpp is the scalar reference.
"""
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, braced, count, find, root_argument  # noqa: E402

root = root_argument()
AURORA = root / "extern/aurora/include/aurora/vfx.hpp"
VFX = root / "extern/aurora/lib/gfx/vfx.cpp"
CONVERT = root / "platform/port_remastered_effect_convert.cpp"
PARTICLES_H = root / "platform/include/port_vfx_particles.h"
PARTICLES = root / "platform/port_vfx_particles.cpp"

BARRIER = 1 << 29
SHADER = 0xF197EC38


def word_of(text):
    """`1u << 29`, `2048` or `8192` as an int."""
    text = re.sub(r"//[^\n]*", "", text).strip().rstrip("uU")
    if "<<" in text:
        base, shift = text.split("<<")
        return int(base.strip().rstrip("uU")) << int(shift.strip())
    return int(text, 0)


def feature_bits(text):
    """The `Name = value` entries of the `enum Feature : uint32_t` block."""
    start = text.index("enum Feature : uint32_t {")
    body = text[start:text.index("\n};", start)]
    body = re.sub(r"//[^\n]*", "", body)
    out = {}
    for name, value in re.findall(r"(\w+)\s*=\s*([^,}]+)", body):
        out[name] = word_of(value)
    return out


class BarrierShieldWiring(WiringCase):
    def setUp(self):
        self.aurora = AURORA.read_text()
        self.vfx = VFX.read_text()
        self.convert = CONVERT.read_text()
        self.header = PARTICLES_H.read_text()
        self.particles = PARTICLES.read_text()

    def test_the_bit_is_29_and_unique(self):
        bits = feature_bits(self.aurora)
        self.assertEqual(bits.get("BarrierShield"), BARRIER)
        self.assertEqual(sorted(name for name, value in bits.items() if value == BARRIER), ["BarrierShield"])
        self.assertEqual(bits["AlreadyExposed"], 1 << 31)

    def test_the_converter_agrees_on_the_bit(self):
        self.assertIn("kBarrierShield = 1u << 29", self.convert)

    def test_the_fragment_branch_is_the_contract(self):
        self.assertIn("} else if ((FEAT & 536870912u) != 0u) {", self.vfx)
        for piece in [
            "let a = sample_raw(S_COLOR, vec2f(-2.0 * uvs.x, 2.0 * uvs.y), lay);",
            "let p = fract(tm * 0.375 - a.z);",
            "let q = fract(tm * -0.0225 - a.z);",
            "let r = fract(tm * 0.3 - a.z);",
            "let s = fract(tm * 0.03 - a.z);",
            "let mask = a.w * sat(n1 - 0.1) * sat(n0 - 0.1);",
            "let edge = sat(mask * 10.0);",
            "let fade = sat(edge + 1.0 - 2.0 * c.x);",
            "if (fade < 0.33) { discard; }",
            "x = fade * (1.0 + in.mesh.y * (c.y - 1.0)) * 0.75;",
            "let wx = (mx - floor(mx * 2.22222233) * 0.45) * 0.008;",
            "vec2f(suv.x + wx - 0.002, suv.y - (wy - 0.002))",
            "vec2f(suv.x + wx + 0.002, suv.y - (wy + 0.002))",
            "sc = vec3f(third.x * 1.165, first.x + 0.33 * (1.5 * first.y - first.x),",
            "let gain = 10.0 - 9.0 * fade;",
            "5.95000029 * in.mesh.z",
        ]:
            self.assertIn(piece, self.vfx)

    def test_both_fog_exclusions_carry_the_bit(self):
        # The tone path and the HDR tail each mix the fog: perm 002 is unfogged, so both skip it.
        self.assertEqual(count(self.vfx, "(FEAT & 536870912u) == 0u"), 2)

    def test_the_two_slots_and_the_scene_gate_are_read(self):
        self.assertIn("VisorRain | BarrierShield | BarrierGlow |\n           FrameBufferColorErosion | VisorSteam,", self.vfx)  # the colour row
        self.assertIn("Indirect | SceneWarp | VisorSplat | BarrierShield | VisorSteam, desc.indirectSlot", self.vfx)
        # The HDR scene gate, the CCH-row block and the row-count selection all name the feature.
        self.assertIn("VisorRunOffLit | VisorRain |\n                   BarrierShield | FrameBufferColorErosion)) != 0) {", self.vfx)
        self.assertIn("(features & (VisorRain | BarrierShield | BarrierGlow | FrameBufferColorErosion)) != 0) {", self.vfx)
        # The glow shares the two rows lists (its CCH0 and its colour slot) but not the scene gate: it reads
        # no scene copy. Both barrier bits are in exactly the CCH-row block and the row-count selection.
        self.assertEqual(count(self.vfx, "VisorRain | BarrierShield | BarrierGlow | FrameBufferColorErosion)) != 0)") +
                         count(self.vfx, "VisorRain | BarrierShield | BarrierGlow | FrameBufferColorErosion | VisorSteam)) != 0)"), 2)

    def test_the_vertex_layout_carries_the_mesh_colour(self):
        self.assertIn("float mesh[4];", self.aurora)
        self.assertIn("static_assert(sizeof(Vertex) == 172);", self.aurora)
        self.assertIn("std::array<wgpu::VertexAttribute, 12> attributes", self.vfx)
        self.assertIn("offsetof(Vertex, mesh), .shaderLocation = 11", self.vfx)
        self.assertIn("@location(11) mesh: vec4f,", self.vfx)
        self.assertIn("out.mesh = in.mesh;", self.vfx)

    def test_the_recipe_is_f197ec38_and_requires_mfc0(self):
        rows = [m.start() for m in re.finditer(re.escape("kBarrierShield"), self.convert)]
        self.assertEqual(len(rows), 2, "the feature constant and one recipe row")
        start = self.convert.index(f"{{0x{SHADER:08x}, kBarrierShield,")
        row = self.convert[start:start + 400]
        self.assertIn('{"TCH0", Role::Color, -1, -1}', row)
        self.assertIn('{"TCH1", Role::Indirect, -1, -1}', row)
        self.assertIn("false, false, true},", row)  # ... params 1, no fog perm, no always-unfogged, MFC0 only
        self.assertIn("bool mfc0Refraction = false;", self.convert)
        self.assertIn('if (recipe->mfc0Refraction && (!HasMatiFlag(mati, "MFC0") || HasMatiFlag(mati, "MFC1")))',
                      self.convert)
        self.assertIn("bool HasMatiFlag(const Mati& mati, const char* tag)", self.convert)
        self.assertIn("out.flags.emplace_back(", self.convert)

    def test_the_mesh_feed_and_readiness(self):
        self.assertIn("inline bool PortVfxBarrierReady(const CPortVfxData& vfx)", self.header)
        ready = braced(self.header, "inline bool PortVfxBarrierReady(const CPortVfxData& vfx)", "the readiness check")
        for piece in ["(vfx.mat.features & aurora::gfx::vfx::BarrierShield) == 0", "vfx.meshStride == 16",
                      "(vfx.meshFlags & 2) == 2", "vfx.mat.colorSlot >= 0", "vfx.mat.indirectSlot >= 0",
                      "vfx.paramCount >= 1"]:
            self.assertIn(piece, ready)
        self.assertIn("PortVfxBarrierReady(*desc.xPortVfx)",
                      braced(self.header, "inline bool PortVfxHasMesh(const CGenDescription& desc)", "the mesh gate"))
        self.assertIn("const bool barrier = (vfx.mat.features & aurora::gfx::vfx::BarrierShield) != 0;",
                      self.particles)
        self.assertIn("} else if (barrier) {", self.particles)
        self.assertIn("AddBarrierShield(vfx, model, p, tm, color, world);", self.particles)
        self.assertIn("for (u32 i = 0; !ice && !barrier && !glow && i < nv; ++i)", self.particles)
        feed = braced(self.particles, "static void AddBarrierShield(", "the feed")
        self.assertIn("m3.InverseTranspose() : Mat3{}", feed)  # includes negative determinant sign
        self.assertIn("v.mesh[k] = m[12 + k];", feed)            # VMSH v2's colour words
        self.assertIn("v.color[k] = color[k];", feed)            # the instance colour stays separate

    def test_the_mesh_draw_feeds_time_and_cch0(self):
        draw = braced(self.particles, "void CElementGen::PortRenderMeshesVfx(CPortVfxMeshBatch& batch)",
                      "the mesh draw")
        self.assertIn(
            "} else if ((features & (aurora::gfx::vfx::BarrierShield | aurora::gfx::vfx::BarrierGlow)) != 0) {", draw)
        self.assertIn("desc.time = CGraphics::GetSimTime();", draw)
        self.assertIn(
            "aurora::gfx::vfx::VisorRain | aurora::gfx::vfx::BarrierShield | "
            "aurora::gfx::vfx::BarrierGlow | aurora::gfx::vfx::VisorSteam)) != 0",
            draw)
        self.assertIn("PortVfxEvalParams(vfx, x74_curFrame, desc.params);", draw)

    def test_the_tests_exist(self):
        self.assertIn("tests/port_vfx_barrier.cpp", (root / "CMakeLists.txt").read_text())
        self.assertIn("port_vfx_barrier_wiring", (root / "CMakeLists.txt").read_text())


BARRIER_GLOW = 1 << 30
GLOW_SHADER = 0xB1EB2B67


class BarrierGlowWiring(WiringCase):
    """VFX_BarrierGlow (b1eb2b67, perm 000): the same barrier's one-variable material, its own bit 30.
    The mesh feed and readiness are BarrierShield's without the second map and without the scene copy;
    only the vertex displacement is its own. GLOW-IMPLEMENTATION.md is the contract, and
    tests/port_vfx_barrier.cpp is the scalar reference for it.
    """

    def setUp(self):
        self.aurora = AURORA.read_text()
        self.vfx = VFX.read_text()
        self.convert = CONVERT.read_text()
        self.header = PARTICLES_H.read_text()
        self.particles = PARTICLES.read_text()

    def test_the_bit_is_30_and_unique(self):
        bits = feature_bits(self.aurora)
        self.assertEqual(bits.get("BarrierGlow"), BARRIER_GLOW)
        self.assertEqual(sorted(name for name, value in bits.items() if value == BARRIER_GLOW), ["BarrierGlow"])
        self.assertEqual(bits["BarrierShield"], 1 << 29)
        self.assertEqual(bits["AlreadyExposed"], 1 << 31)
        self.assertIn("kBarrierGlow = 1u << 30", self.convert)

    def test_the_fragment_branch_is_the_contract(self):
        self.assertIn("} else if ((FEAT & 1073741824u) != 0u) {", self.vfx)
        for piece in [
            "let a = sample_raw(S_COLOR, vec2f(-uvs.x, uvs.y), lay).r;",
            "let phase = tm - floor(tm * 0.555555582) * 1.79999995;",
            "let b = sample_raw(S_COLOR, vec2f(-uvs.x - 0.200000003, uvs.y - phase + 0.550000012), lay).b;",
            "let d = sample_raw(S_COLOR, vec2f(-uvs.x, uvs.y + a + 0.5 * tm + a), lay).a;",
            "let k = d * (ceil(in.mesh.x) * 0.200000018 + 0.100000001);",
            "let f = 1.0 - dot(V, -N);",
            "let p = sat(b * (1.0 - 0.5 * phase));",
            "let fade = sat(d + 1.0 - 2.0 * c.x);",
            "let rim = sat(f * f) * fade;",
            "let gain = 10.0 - 9.0 * fade;",
            "k * 7.5 - 0.5 * in.mesh.x + 1.4 * p + 0.5",
            "k * 30.0 - 0.899999976 * in.mesh.x + 3.0 * p + 0.9",
            "vc.rgb * M",
        ]:
            self.assertIn(piece, self.vfx)

    def test_both_fog_exclusions_carry_the_bit(self):
        self.assertEqual(count(self.vfx, "(FEAT & 1073741824u) == 0u"), 2)

    def test_the_colour_slot_and_cch_rows_are_shared_but_not_the_scene(self):
        # The colour row, the CCH-row block and the row-count selection name the bit...
        self.assertIn("VisorRain | BarrierShield | BarrierGlow |\n           FrameBufferColorErosion | VisorSteam,", self.vfx)
        self.assertEqual(count(self.vfx, "VisorRain | BarrierShield | BarrierGlow | FrameBufferColorErosion)) != 0)") +
                         count(self.vfx, "VisorRain | BarrierShield | BarrierGlow | FrameBufferColorErosion | VisorSteam)) != 0)"), 2)
        # ... but the HDR scene gate does not: this perm reads no scene copy, so the only list that still
        # ends at the shield is the gate's.
        self.assertEqual(count(self.vfx, "BarrierShield | FrameBufferColorErosion)) != 0)"), 1)
        # And the indirect slot is not read (no TCH1 in the recipe).
        self.assertNotIn("VisorSplat | BarrierShield | BarrierGlow, desc.indirectSlot", self.vfx)

    def test_the_recipe_is_b1eb2b67_and_needs_no_shield_flag(self):
        rows = [m.start() for m in re.finditer(re.escape("kBarrierGlow"), self.convert)]
        self.assertEqual(len(rows), 2, "the feature constant and one recipe row")
        start = self.convert.index(f"{{0x{GLOW_SHADER:08x}, kBarrierGlow,")
        row = self.convert[start:start + 500]
        self.assertIn('{"TCH0", Role::Color, -1, -1}', row)
        self.assertNotIn("Indirect", row)
        self.assertIn("false, true},", row)  # ... params 1, no fog perm, no always-unfogged, no MFC0, no flags
        self.assertIn("bool noRefractionFlags = false;", self.convert)
        self.assertIn('if (recipe->noRefractionFlags && (HasMatiFlag(mati, "MFC0") || HasMatiFlag(mati, "MFC1")))',
                      self.convert)

    def test_the_readiness_is_the_shields_without_the_second_map(self):
        self.assertIn("inline bool PortVfxBarrierGlowReady(const CPortVfxData& vfx)", self.header)
        ready = braced(self.header, "inline bool PortVfxBarrierGlowReady(const CPortVfxData& vfx)",
                       "the glow readiness check")
        for piece in ["(vfx.mat.features & aurora::gfx::vfx::BarrierGlow) == 0", "vfx.meshStride == 16",
                      "(vfx.meshFlags & 2) == 2", "vfx.mat.colorSlot >= 0", "vfx.paramCount >= 1"]:
            self.assertIn(piece, ready)
        self.assertNotIn("indirectSlot", ready)
        self.assertIn("PortVfxBarrierGlowReady(*desc.xPortVfx)",
                      braced(self.header, "inline bool PortVfxHasMesh(const CGenDescription& desc)", "the mesh gate"))

    def test_the_feed_shares_the_shields_and_adds_the_displacement(self):
        self.assertIn("const bool glow = (vfx.mat.features & aurora::gfx::vfx::BarrierGlow) != 0;", self.particles)
        self.assertIn("} else if (glow) {", self.particles)
        self.assertIn("AddBarrierGlow(vfx, model, p, tm, color, world);", self.particles)
        self.assertIn("for (u32 i = 0; !ice && !barrier && !glow && i < nv; ++i)", self.particles)
        feed = braced(self.particles, "static void AddBarrierGlow(", "the glow feed")
        # The mesh colour and the inverse-transpose normal feed are the shield's; only the VS's
        # displacement is the glow's own, and only it moves the vertices.
        self.assertIn("AddBarrierShield(vfx, model, p, tm, color, world);", feed)
        self.assertIn("const float phase = time - 10.f * m[14];", feed)  # phase = time - 10 mesh.b
        self.assertIn("0.0649999976f", feed)  # the VS's own amplitude base, not 1
        self.assertIn("-0.33f", feed)         # the VS's own clamp, not -1
        self.assertIn("0.0350000001f", feed)  # Remastered +Y on the port's model Z (the mesh data is R2G)
        self.assertIn("m[2] + m[5] * amp + liftY", feed)
        self.assertIn("CGraphics::GetSimTime()", feed)

    def test_the_mesh_draw_feeds_time_and_cch0(self):
        draw = braced(self.particles, "void CElementGen::PortRenderMeshesVfx(CPortVfxMeshBatch& batch)",
                      "the mesh draw")
        self.assertIn(
            "} else if ((features & (aurora::gfx::vfx::BarrierShield | aurora::gfx::vfx::BarrierGlow)) != 0) {", draw)
        self.assertIn("desc.time = CGraphics::GetSimTime();", draw)
        self.assertIn(
            "aurora::gfx::vfx::VisorRain | aurora::gfx::vfx::BarrierShield | "
            "aurora::gfx::vfx::BarrierGlow | aurora::gfx::vfx::VisorSteam)) != 0",
            draw)
        self.assertIn("PortVfxEvalParams(vfx, x74_curFrame, desc.params);", draw)


if __name__ == "__main__":
    unittest.main(verbosity=2)
