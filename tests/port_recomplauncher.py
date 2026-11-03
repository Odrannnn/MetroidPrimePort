#!/usr/bin/env python3
"""RecompLauncher release profiles, using synthetic installs only."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("write_recomplauncher", ROOT / "tools/write_recomplauncher.py")
writer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(writer)


class Profiles(unittest.TestCase):
    def resolve(self, profile, existing):
        # RecompLauncher profile::place: first existing 'if', else fallback.
        with tempfile.TemporaryDirectory() as temp:
            tokens = {name: Path(temp) / name for name in ("install", "config", "local", "home")}

            def fill(value):
                for name, path in tokens.items():
                    value = value.replace("{" + name + "}", str(path))
                return Path(value)

            for path in existing:
                file = fill(path)
                file.parent.mkdir(parents=True, exist_ok=True)
                file.touch()
            for place in profile["config"][0]["file"]:
                if "if" not in place or fill(place["if"]).exists():
                    return place["path"]
        self.fail("No settings fallback")

    def test_windows_release_is_the_owned_manifest(self):
        self.assertEqual(writer.profile("windows"), json.loads((ROOT / "recomplauncher.json").read_text()))

    def test_fresh_archive_and_legacy_path_priority(self):
        for platform, legacy in (("windows", "{config}"), ("linux", "{local}")):
            with self.subTest(platform=platform):
                p = writer.profile(platform)
                user = "{install}/user/port_settings.ini"
                loose = "{install}/port_settings.ini"
                old = legacy + "/Metroid Prime/port_settings.ini"
                self.assertEqual(self.resolve(p, []), user)
                self.assertEqual(self.resolve(p, [old]), old)
                self.assertEqual(self.resolve(p, [old, loose]), loose)
                self.assertEqual(self.resolve(p, [old, loose, user]), user)
                self.assertIn("{install}/user/USA", p["saves"])

    def test_appimage_uses_sibling_data_and_matching_architecture(self):
        for arch in ("x86_64", "aarch64"):
            with self.subTest(arch=arch):
                p = writer.profile("linux", arch)
                self.assertEqual(p["start"]["program"], [f"MetroidPrime-{arch}.AppImage"])
                self.assertEqual(self.resolve(p, []), "{install}/port_settings.ini")
                self.assertEqual(self.resolve(p, ["{local}/Metroid Prime/port_settings.ini"]),
                                 "{local}/Metroid Prime/port_settings.ini")
                self.assertEqual(self.resolve(p, ["{local}/Metroid Prime/port_settings.ini",
                                                 "{install}/port_settings.ini"]),
                                 "{install}/port_settings.ini")

    def test_macos_keeps_settings_and_saves_outside_signed_bundle(self):
        p = writer.profile("macos")
        self.assertEqual(self.resolve(p, []),
                         "{home}/Library/Application Support/Metroid Prime/port_settings.ini")
        self.assertTrue(all(".app/" not in path for path in p["saves"]))
        vsync = next(s for s in p["settings"] if s["id"] == "vsync")
        self.assertEqual(vsync["defaults"], {"vsync": "1"})

    def test_manifest_uses_real_settings(self):
        source = (ROOT / "platform/debug_ui/settings.cpp").read_text()
        for p in (writer.profile("windows"), writer.profile("linux"), writer.profile("macos"),
                  writer.profile("linux", "x86_64")):
            for setting in p["settings"]:
                for choice in setting["choices"]:
                    for key in choice["set"]:
                        self.assertIn('"' + key + '"', source)
            self.assertEqual(p["start"]["env"]["MP_DISC"], "{game}")
            self.assertEqual(p["config"][0]["game_path"], "disc_path")
            # These formats do not describe GameCube/Wii filesystems.
            self.assertNotIn("check", p["game"])
            self.assertNotIn("prepare", p)

    def test_packaged_manifest_selects_an_existing_program(self):
        # These paths are the package layouts, independent of the generator.
        packages = (
            ("windows", None, "metroid_prime_port.exe"),
            ("linux", None, "metroid_prime_port"),
            ("macos", None, "Metroid Prime Port.app/Contents/MacOS/metroid_prime_port"),
            ("linux", "x86_64", "MetroidPrime-x86_64.AppImage"),
            ("linux", "aarch64", "MetroidPrime-aarch64.AppImage"),
        )
        for platform, arch, packaged_program in packages:
            with self.subTest(platform=platform, arch=arch), tempfile.TemporaryDirectory() as temp:
                dest = Path(temp) / "Package with spaces"
                command = [sys.executable, "-I", str(ROOT / "tools/write_recomplauncher.py"),
                           "--platform", platform]
                if arch:
                    command += ["--appimage-arch", arch]
                subprocess.run(command + [str(dest / "recomplauncher.json")], check=True)
                p = json.loads((dest / "recomplauncher.json").read_text())
                executable = dest / packaged_program
                executable.parent.mkdir(parents=True, exist_ok=True)
                executable.touch()
                self.assertTrue(any((dest / program).is_file() for program in p["start"]["program"]))
                self.assertFalse(any(dest.rglob("port_settings.ini")))

    def test_invalid_appimage_platform_does_not_write_a_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "recomplauncher.json"
            result = subprocess.run(
                [sys.executable, "-I", str(ROOT / "tools/write_recomplauncher.py"),
                 "--platform", "windows", "--appimage-arch", "x86_64", str(output)],
                capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--appimage-arch requires --platform linux", result.stderr)
            self.assertFalse(output.exists())

    def test_release_workflows_ship_manifests(self):
        windows = (ROOT / ".github/workflows/windows.yml").read_text()
        linux = (ROOT / ".github/workflows/linux-release.yml").read_text()
        macos = (ROOT / ".github/workflows/macos.yml").read_text()
        appimage = (ROOT / "tools/make_appimage.sh").read_text()
        self.assertIn("Copy-Item recomplauncher.json dist/", windows)
        self.assertIn('--platform linux "$d/recomplauncher.json"', linux)
        self.assertIn("--platform macos dist/recomplauncher.json", macos)
        self.assertIn('--appimage-arch "$ARCH"', appimage)
        self.assertIn("appimage.tar.gz", linux)
        self.assertIn('tar -C "$OUT"', appimage)
        self.assertIn('"$APPDIR/recomplauncher.json"', appimage)
        self.assertIn('install -m644 "$APPDIR/recomplauncher.json" "$OUT/$PACKAGE/recomplauncher.json"', appimage)


class SettingsContract(unittest.TestCase):
    # Reference evaluator for RecompLauncher profile.rs: same(), current(),
    # default_of() and pick(). This tests manifest behavior against that
    # reviewed contract; it does not execute the Rust backend or the game.
    # https://github.com/elripalda/RecompLauncher/blob/main/src-tauri/src/profile.rs

    @staticmethod
    def same(a, b):
        a, b = (str(v).strip().strip("\"'").strip() for v in (a, b))
        try:
            return abs(float(a) - float(b)) < 1e-6
        except ValueError:
            return a.casefold() == b.casefold()

    def current(self, profile, setting_id, values):
        setting = next(s for s in profile["settings"] if s["id"] == setting_id)
        defaults = setting.get("defaults", {})
        value = lambda key: values.get(key, defaults.get(key, ""))

        def matches(pairs):
            return all(self.same(value(k), v) for k, v in pairs.items())

        choice = next((c for c in setting["choices"] if matches(c.get("set", {}))), None)
        if choice is None:
            choice = next((c for c in setting["choices"] if all(
                k in defaults and self.same(defaults[k], v) for k, v in c.get("set", {}).items())),
                setting["choices"][0])

        def condition(key, wanted):
            if self.same(value(key), wanted):
                return True
            fallback = next((s["defaults"][key] for s in profile["settings"]
                             if key in s.get("defaults", {})), "")
            return key not in values and self.same(fallback, wanted)

        enabled = all(condition(k, v) for k, v in setting.get("when", {}).items())
        return choice, enabled

    def profiles(self):
        return [writer.profile("windows"), writer.profile("linux"), writer.profile("macos"),
                writer.profile("linux", "x86_64"), writer.profile("linux", "aarch64")]

    def test_fresh_install_uses_game_defaults_and_disables_secondary_cap(self):
        for p in self.profiles():
            self.assertEqual(self.current(p, "fps", {})[0]["label"], "On")
            self.assertFalse(self.current(p, "fps_cap", {})[1])
            for key in ("original", "resolution", "aspect", "msaa", "window", "vsync"):
                self.assertTrue(self.current(p, key, {})[1])

    def test_custom_caps_do_not_fall_back_to_sixty_or_unlimited(self):
        for p in self.profiles():
            for cap in range(30, 501):
                with self.subTest(program=p["start"]["program"], cap=cap):
                    values = {"frame_limit": "0", "fps_cap": str(cap)}
                    original, enabled = self.current(p, "fps", values)
                    self.assertEqual(original["label"], "Off")
                    self.assertTrue(enabled)
                    custom, enabled = self.current(p, "fps_cap", values)
                    self.assertTrue(enabled)
                    if custom["set"]:
                        self.assertEqual(custom["set"]["fps_cap"], str(cap))
                    else:
                        self.assertEqual(custom["label"], "Custom (in-game)")
            self.assertEqual(self.current(p, "fps_cap", {"frame_limit": "0", "fps_cap": "144"})[0]["label"],
                             "144 fps")
            self.assertEqual(self.current(p, "fps_cap", {"frame_limit": "0", "fps_cap": "143"})[0]["label"],
                             "Custom (in-game)")

    def test_original_experience_disables_only_overridden_controls(self):
        for p in self.profiles():
            values = {"original_experience": "1", "frame_limit": "0", "fps_cap": "144",
                      "render_scale": "2", "msaa": "4"}
            for key in ("fps", "fps_cap", "resolution", "aspect", "msaa"):
                self.assertFalse(self.current(p, key, values)[1], key)
            for key in ("original", "window", "vsync"):
                self.assertTrue(self.current(p, key, values)[1], key)
            values["original_experience"] = "0"
            for key in ("fps", "fps_cap", "resolution", "aspect", "msaa"):
                self.assertTrue(self.current(p, key, values)[1], key)
            self.assertEqual(self.current(p, "fps_cap", values)[0]["label"], "144 fps")

    def test_toggling_original_cap_and_custom_fallback_preserves_values(self):
        for p in self.profiles():
            values = {"frame_limit": "0", "fps_cap": "143", "music_boost": "150"}
            toggle = next(s for s in p["settings"] if s["id"] == "fps")
            values.update(next(c["set"] for c in toggle["choices"] if c["label"] == "On"))
            self.assertFalse(self.current(p, "fps_cap", values)[1])
            values.update(next(c["set"] for c in toggle["choices"] if c["label"] == "Off"))
            custom, enabled = self.current(p, "fps_cap", values)
            self.assertTrue(enabled)
            values.update(custom["set"])
            self.assertEqual(values, {"frame_limit": "0", "fps_cap": "143", "music_boost": "150"})

    def test_unlimited_changes_only_secondary_cap(self):
        for p in self.profiles():
            values = {"original_experience": "0", "frame_limit": "0", "fps_cap": "144",
                      "vsync": "1"}
            cap = next(s for s in p["settings"] if s["id"] == "fps_cap")
            values.update(next(c["set"] for c in cap["choices"] if c["label"] == "Unlimited"))
            self.assertEqual(values, {"original_experience": "0", "frame_limit": "0",
                                     "fps_cap": "0", "vsync": "1"})
            self.assertEqual(self.current(p, "fps_cap", values)[0]["label"], "Unlimited")

    def test_fractional_render_scales_are_preserved_and_whole_scales_match(self):
        for p in self.profiles():
            values = {"render_scale": "1.25"}
            choice, enabled = self.current(p, "resolution", values)
            self.assertTrue(enabled)
            self.assertEqual(choice["label"], "Custom (in-game)")
            values.update(choice["set"])
            self.assertEqual(values, {"render_scale": "1.25"})
            self.assertEqual(self.current(p, "resolution", {"render_scale": "1.0"})[0]["label"],
                             "1x (original)")


if __name__ == "__main__":
    unittest.main()
