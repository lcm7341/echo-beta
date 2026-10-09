"""Compile the production button pipeline against a deterministic game stub.

Tests command ordering and routing without substituting a copy of the pipeline.
The stub does not simulate Geometry Dash physics or other mods.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/Logic/play.cpp").read_text()
prepare = source[source.index("void Logic::prepareButtons("):source.index("void Logic::rememberMove(")]
pump = source[source.index("void runOrbClicks("):source.index("void Logic::offsetInputs(")]
fixture = (root / "tests/button_pipeline_fixture.hpp").read_text()
with tempfile.TemporaryDirectory(prefix="echo-button-tests-") as directory:
    cpp = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    cpp.write_text(fixture.replace("// PRODUCTION_PIPELINE", prepare + pump))
    subprocess.run(["c++", "-std=c++23", "-DGEODE_IS_WINDOWS", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-Isrc", str(cpp), "-o", str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], check=True)
