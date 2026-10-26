#!/usr/bin/env python3
"""Link the parser regression against an already built Linux game's actual objects.

Usage: python3 tools/test_particle_loader.py build/port-ice-cull <evidence-dir> [PART]
Reuses compiler/link flags from Ninja, replacing only platform/main.cpp with the test.
No game/renderer startup, disc or graphics device is used by the test main.
"""
from pathlib import Path
import subprocess, shlex, sys
build=Path(sys.argv[1]).resolve();out=Path(sys.argv[2]).resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1]
main='CMakeFiles/metroid_prime_port.dir/platform/main.cpp.o'
def commands(target):
    return subprocess.check_output(['ninja','-t','commands',target],cwd=build,text=True).splitlines()
compile=next(shlex.split(s) for s in reversed(commands(main)) if ' -c ' in s and 'platform/main.cpp' in s)
compile[compile.index('-o')+1]=str(out/'test.o')
compile[compile.index('-c')+1]=str(root/'tests/port_particle_loader.cpp')
if '-MF' in compile:
    compile[compile.index('-MF')+1]=str(out/'test.d')
subprocess.run(compile,cwd=build,check=True)
link=next(s for s in reversed(commands('metroid_prime_port')) if main in s and ' -o metroid_prime_port ' in s)
link=next(s for s in link.split(' && ') if ' -o metroid_prime_port ' in s)
link=shlex.split(link.strip().lstrip(':').strip())
link[link.index(main)]=str(out/'test.o')
link[link.index('-o')+1]=str(out/'parser-test')
subprocess.run(link,cwd=build,check=True)
subprocess.run([str(out/'parser-test'),*sys.argv[3:]],cwd=root,check=True)
