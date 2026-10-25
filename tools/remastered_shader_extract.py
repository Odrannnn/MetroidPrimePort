#!/usr/bin/env python3
"""Extract/decompile an owned MTRL from an extracted Remastered asset tree.

The root contains x/all (promoted assets) and rooms/<pak> (room-local assets).
No game data or external dependencies are downloaded by this tool.
"""
import argparse
import concurrent.futures
import hashlib
import os
from pathlib import Path
import re
import subprocess
import zlib


def resolve(root, shader):
    if not re.fullmatch(r"[0-9a-fA-F]{8}", shader):
        raise ValueError("shader must be eight hexadecimal digits")
    prefix = bytes.fromhex(shader)[::-1].hex() + "-"
    candidates = sorted(p for base in (root / "x", root / "rooms")
                        for p in base.rglob("*.MTRL")
                        if p.name.lower().startswith(prefix) and p.is_file())
    if not candidates:
        raise ValueError(f"no MTRL for {shader} under {root}/x or {root}/rooms")
    # Copies across paks are common. Never select an arbitrary conflicting
    # UUID/payload merely because its name sorts first.
    identities = {(p.name.lower(), hashlib.sha256(p.read_bytes()).digest()) for p in candidates}
    if len(identities) != 1:
        raise ValueError("conflicting MTRL candidates: " + ", ".join(map(str, candidates)))
    return candidates[0]


def extract(material, output):
    data = material.read_bytes()
    if data[:4] != b"RFRM" or data[20:24] != b"MTRL":
        raise ValueError("not an RFRM/MTRL asset")
    raw = zlib.decompress(data[32:])
    output.mkdir(parents=True, exist_ok=True)
    blobs, at, files = 0, 0, []
    while at < len(raw) - 2:
        if raw[at] == 0x78 and raw[at + 1] in (0xda, 0x9c, 0x01):
            try:
                stream = zlib.decompressobj()
                blob = stream.decompress(raw[at:])
            except zlib.error:
                at += 1
                continue
            if not stream.eof:
                at += 1
                continue
            if len(blob) > 64:
                for stage, part in enumerate(blob.split(bytes.fromhex("78563412"))[1:]):
                    code = part[44:]
                    if len(code) > 600:
                        path = output / f"{blobs:03d}_{stage}.bin"
                        path.write_bytes(code)
                        files.append(path)
                blobs += 1
            at += len(raw[at:]) - len(stream.unused_data)
        else:
            at += 1
    if not files:
        raise ValueError("no shader code found in MTRL")
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shader")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--resolve-only", action="store_true")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args()
    try:
        root = args.root.resolve()
        material = resolve(root, args.shader)
        print(material, flush=True)
        if args.resolve_only:
            return
        output = args.output or root / "mtrl-tr" / args.shader.lower()
        files = extract(material, output / "code")
        glsl = output / "glsl"
        glsl.mkdir(parents=True, exist_ok=True)
        tool = root / "dxtools/ryujinx/src/Ryujinx.ShaderTools/bin/Release/net10.0/Ryujinx.ShaderTools"
        env = dict(os.environ, DOTNET_ROOT=str(root / "dxtools/dotnet"), HOME=str(root / "dxtools/dothome"))

        def decompile(path):
            result = subprocess.run([str(tool), str(path.resolve())], env=env,
                                    check=True, capture_output=True)
            if b"void main" not in result.stdout:
                raise ValueError(f"no GLSL main for {path}")
            (glsl / (path.stem + ".glsl")).write_bytes(result.stdout)

        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            list(pool.map(decompile, files))
        print(f"decompiled {len(files)} stages into {glsl}")
    except (ValueError, OSError, zlib.error, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
