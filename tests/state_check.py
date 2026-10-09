"""Build tests/state_check.cpp for Windows with the build's toolchain and run it under Wine."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
splat = Path.home() / '.local/share/Geode/cross-tools/splat'

with tempfile.TemporaryDirectory() as folder:
    exe = Path(folder) / 'state_check.exe'
    includes = [f'-isystem{splat / path}' for path in
                ('crt/include', 'sdk/include/ucrt', 'sdk/include/shared', 'sdk/include/um')]
    subprocess.run(['clang++', '--target=x86_64-pc-windows-msvc', '-fuse-ld=lld', '-std=c++23', '-O2',
                    '-fms-compatibility-version=19.37', '-fexceptions', '-fcxx-exceptions', '-D_DLL', '-D_MT',
                    '-Xclang', '--dependent-lib=msvcrt', *includes, '-I', str(root / 'src'),
                    str(root / 'tests/state_check.cpp'), '-o', str(exe),
                    f'-L{splat / "crt/lib/x86_64"}', f'-L{splat / "sdk/lib/ucrt/x86_64"}',
                    f'-L{splat / "sdk/lib/um/x86_64"}'], check=True)
    env = dict(os.environ, WINEDEBUG='-all')
    subprocess.run(['wine', str(exe)], check=True, env=env)
