"""Production wiring guard for the model-opacity key of platform/include/port_vfx_opacity.h.

A VMAT blob records a material's recipe as its feature word, not as its shader id, so the mesh
form of the "OpacityFresnel" recipes is selected by one exact word (Ramp|Indirect|DepthSoften|
OpacityFade, the exposure policy bit ignored). That word is a *shared* key: any other row of the
converter's recipe table carrying the same word silently inherits the mesh form. The word is not
unique in today's table in general (many rows repeat words), so the guard is a count, not a set.

The C++ tests cannot see the table: tests/port_remastered_effect_convert.cpp pins the word of the
one converted PowerBall material to the constant (it catches an edit of that recipe's own features)
and tests/port_vfx_opacity.cpp checks the selection against a hand copy of today's six other fade
words; a *new* row with the key's word fails neither. This reads the actual table (the same way
tests/port_air_mover_wiring.py reads port_remastered_movers.cpp: no engine code runs) and asserts exactly
one row carries the key, and that row is 0xe899b1df.

It parses the recipe table's first two fields per row (shader, features), resolving the feature
names against the converter's own constexpr bits, and the key against the aurora::gfx::vfx::Feature
enum the runtime's words come from: a row the parser cannot resolve fails the test rather than
being skipped.
"""
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, between, partition, root_argument  # noqa: E402

root = root_argument()
CONVERT = root / "platform/port_remastered_effect_convert.cpp"
OPACITY = root / "platform/include/port_vfx_opacity.h"
VFX = root / "extern/aurora/include/aurora/vfx.hpp"

# The pinned recipe, and the word it carries: Ramp (8) | Indirect (16) | DepthSoften (32) |
# OpacityFade (2048). Anchors the derivation below; the C++ test ties it to the converted material.
PINNED_SHADER = 0xE899B1DF
PINNED_WORD = 0x838
EXPOSED = 1 << 31


def word_of(text):
    """`1u << 31` or `2048` as an int."""
    text = re.sub(r"//[^\n]*", "", text).strip().rstrip("uU")
    if "<<" in text:
        base, shift = text.split("<<")
        return int(base.strip().rstrip("uU")) << int(shift.strip())
    return int(text, 0)


def declarations(text, anchor, first_name=None):
    """The `name = value, ...` list a `constexpr uint32_t` statement opens with `anchor`."""
    tail = between(text, anchor, ";", "constants")
    if first_name is not None:  # the anchor is a single constant's `name =`
        tail = f"{first_name} = {tail}"
    out = {}
    for part in tail.split(","):
        name, sep, value = part.partition("=")
        assert sep, f"constant {part.strip()!r} has no value"
        out[name.strip()] = word_of(value)
    return out


def feature_bits():
    """Every feature name the rows and the key can use, from both sides of the comparison."""
    vfx = VFX.read_text()
    enum = between(vfx, "enum Feature : uint32_t {", "};", str(VFX))
    aurora = {}
    for part in re.sub(r"//[^\n]*", "", enum).split(","):
        name, sep, value = part.partition("=")
        assert sep, f"enum entry {part.strip()!r} has no value"
        aurora[name.strip()] = word_of(value)

    convert = CONVERT.read_text()
    local = declarations(convert, "constexpr uint32_t kColorTex =", "kColorTex")
    local.update(declarations(convert, "constexpr uint32_t kAlreadyExposed =", "kAlreadyExposed"))
    # The rows are built from the converter's local copies of the enum's bits; the key is built
    # from the enum. A local bit that drifted would make the comparison miss the recipe.
    for name, value in local.items():
        self_name = name.lstrip("k")
        assert self_name in aurora, f"converter bit {name} has no aurora::gfx::vfx::Feature"
        assert aurora[self_name] == value, f"{name} = {value} != Feature::{self_name} = {aurora[self_name]}"
    return local


BITS = feature_bits()
CONVERT_TEXT = CONVERT.read_text()
OPACITY_TEXT = OPACITY.read_text()
# The key is kPortVfxModelOpacityFeatures: `uint32_t(aurora::gfx::vfx::X) | ...`.
KEY_TEXT = between(OPACITY_TEXT, "kPortVfxModelOpacityFeatures =", ";", str(OPACITY))


def resolve(expression, where):
    total = 0
    for token in expression.split("|"):
        token = token.strip().replace("uint32_t(", "").rstrip(")")
        if not token:
            continue
        if re.fullmatch(r"0[xX][0-9a-fA-F]+|\d+", token):
            total |= int(token, 0)
            continue
        name = token.split("::")[-1]
        for candidate in (name, name.lstrip("k"), "k" + name):
            if candidate in BITS:
                total |= BITS[candidate]
                break
        else:
            raise AssertionError(f"{where}: unknown feature name {token!r}")
    return total


def entries(text):
    """The top-level `{...}` entries of the recipe table (rows are multi-line)."""
    body = partition(text, "static const std::vector<Recipe> table = {")[1]
    depth, out, buffer = 0, [], None
    for character in body:
        if character == "{":
            depth += 1
            if depth == 1:  # the row's own braces; its fields start inside them
                buffer = []
                continue
        elif character == "}":
            depth -= 1
            if depth == 0:  # the row ends
                out.append("".join(buffer))
                buffer = None
                continue
            if depth < 0:  # the table ends
                break
        if depth >= 1:
            buffer.append(character)
    return out


def fields(entry):
    out, depth, buffer = [], 0, []
    for character in entry:
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
        if character == "," and depth == 0:
            out.append("".join(buffer).strip())
            buffer = []
        else:
            buffer.append(character)
    out.append("".join(buffer).strip())
    return out


def table_rows():
    rows = []
    for entry in entries(CONVERT_TEXT):
        fields_of = fields(entry)
        assert len(fields_of) >= 4, f"table row with {len(fields_of)} fields: {entry[:60]!r}"
        assert re.fullmatch(r"0x[0-9a-fA-F]{8}", fields_of[0]), f"row shader {fields_of[0]!r}"
        rows.append((int(fields_of[0], 16), resolve(fields_of[1], fields_of[0])))
    return rows


ROWS = table_rows()


class Wiring(WiringCase):
    def test_key_is_the_pinned_word(self):
        word = resolve(KEY_TEXT, "port_vfx_opacity.h")
        self.assertEqual(word, PINNED_WORD,
                         f"kPortVfxModelOpacityFeatures is {word:#x}, not the pinned {PINNED_WORD:#x}")

    def test_exactly_one_recipe_carries_the_key(self):
        key = resolve(KEY_TEXT, "port_vfx_opacity.h")
        matches = [(shader, word) for shader, word in ROWS if word & ~EXPOSED == key]
        self.assertEqual(len(matches), 1,
                         "the model-opacity key must be carried by exactly one recipe row; carries it: " +
                         ", ".join(f"{shader:#010x}" for shader, _ in matches))
        self.assertEqual(matches[0][0], PINNED_SHADER,
                         f"the key row is {matches[0][0]:#010x}, not {PINNED_SHADER:#010x}")
        # The key ignores the exposure policy bit, so a row with it inherits the form: the row
        # that matches must be the one carrying the plain word.
        self.assertEqual(matches[0][1], key,
                         f"the key row's word carries extra bits: {matches[0][1]:#010x}")
        self.assertEqual(len([word for _, word in ROWS if word == key]), 1,
                         "a second row carries the plain word")

    def test_table_is_parsed_whole(self):
        # A parser that stops early or skips a row would make the count above vacuous: an
        # independent line count of the table's rows must agree with the parse.
        # "\n  };\n" is the table's own terminator.
        body = between(CONVERT_TEXT, "static const std::vector<Recipe> table = {", "\n  };\n",
                       str(CONVERT))
        row_lines = re.findall(r"^ *\{0x[0-9a-fA-F]{8},", body, re.M)
        self.assertEqual(len(row_lines), len(ROWS), "the row parse and the row count disagree")
        self.assertGreaterEqual(len(ROWS), 50, "the recipe table shrank unexpectedly")
        self.assertEqual(len({shader for shader, _ in ROWS}), len(ROWS),
                         "two rows share a shader id; RecipeOf would only reach the first")

    def test_helper_masks_only_the_exposure_bit(self):
        # What this file mirrors: the helper's own comparison.
        self.assertIn("(features & ~uint32_t(aurora::gfx::vfx::AlreadyExposed))", OPACITY_TEXT)
        self.assertIn("== kPortVfxModelOpacityFeatures", OPACITY_TEXT)

    def test_header_points_at_this_guard(self):
        self.assertIn("tests/port_vfx_opacity_wiring.py", OPACITY_TEXT,
                       "port_vfx_opacity.h must name the guard that counts the key's rows")

    def test_vsmt_storage_and_bounds_use_shared_count(self):
        constants = (root / "include/Kyoto/Particles/CPortVfxConstants.hpp").read_text()
        self.assertIn("kPortVfxVsmtCount = 21", constants)
        self.assertIn("kPortVfxOpacityGainTarget = 19", constants)
        self.assertIn("kPortVfxOpacityBiasTarget = 20", constants)
        for file, array in [
            ("include/Kyoto/Particles/CElementGen.hpp", "xPortVsmt"),
            ("src/Kyoto/Particles/CParticleSwoosh.cpp", "vsmt"),
            ("src/Kyoto/Particles/CParticleElectric2Port.cpp", "vsmt"),
            ("platform/port_vfx_particles.cpp", "vsmt"),
        ]:
            text = (root / file).read_text()
            self.assertIn(array + "[kPortVfxVsmtCount]", text, file)
            self.assertNotRegex(text, r"(?:vsmt|Vsmt)\[19\]")
        runtime = (root / "platform/port_vfx_particles.cpp").read_text()
        self.assertIn("e.a >= kPortVfxVsmtCount", runtime)
        self.assertIn("desc.opacityGain = (vsmtMask & (1u << kPortVfxOpacityGainTarget))", runtime)
        self.assertIn("desc.opacityBias = (vsmtMask & (1u << kPortVfxOpacityBiasTarget))", runtime)

    def test_v3_is_only_the_audited_shader(self):
        self.assertIn("const bool opacityGainBias = mati.shader == kPowerBallShader;", CONVERT_TEXT)
        # v4 (a header material's alpha gamma) carries v3's gain/bias, the identity unless audited.
        self.assertIn("AppendBE32(blob, recipe->litPerVertex ? 5 : alphaGamma ? 4 : opacityGainBias ? 3 : 2);", CONVERT_TEXT)
        self.assertIn("AppendBE32(blob, FloatBits(1.0f));  // v3's identity gain/bias", CONVERT_TEXT)
        rows = [fields(row) for row in entries(CONVERT_TEXT)]
        pinned = next(row for row in rows if int(row[0], 16) == PINNED_SHADER)
        guarded = next(row for row in rows if int(row[0], 16) == 0x7D52BD6B)
        self.assertEqual(pinned[-1], "false")
        self.assertEqual(guarded[-1], "true")


if __name__ == "__main__":
    unittest.main(verbosity=2)
