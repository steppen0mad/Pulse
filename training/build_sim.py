"""
Compile the C simulation + perception into a Python extension (_pulse_sim) via
cffi API mode.

API mode (set_source + compile) makes the C compiler #include the real headers,
so our cdef transcription is checked against world.h / agent.h at build time --
there is no hand-maintained ABI that could silently drift. After building we also
assert ffi.sizeof("World") == the C sizeof(World), catching any layout mismatch
loudly (no fallback).

Run from anywhere:
    python training/build_sim.py
The extension is emitted into training/ so env.py (also in training/) can import it.
"""
import os
import sys
from pathlib import Path

from cffi import FFI

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import _sim_cdef as C  # noqa: E402


def build():
    ffibuilder = FFI()
    ffibuilder.cdef(C.CDEF)
    ffibuilder.set_source(
        C.MODULE_NAME,
        '#include "world.h"\n'
        '#include "agent.h"\n'
        "int world_struct_size(void) { return (int)sizeof(World); }\n",
        sources=[str(ROOT / s) for s in C.SOURCES],
        include_dirs=[str(ROOT / d) for d in C.INCLUDE_DIRS],
        extra_compile_args=C.EXTRA_COMPILE_ARGS,
    )
    # Emit the .c/.o/.so into training/ so the import below and env.py find it.
    ffibuilder.compile(tmpdir=str(HERE), verbose=True)


def verify():
    import importlib

    if str(HERE) not in sys.path:
        sys.path.insert(0, str(HERE))
    mod = importlib.import_module(C.MODULE_NAME)
    ffi, lib = mod.ffi, mod.lib

    py_size = ffi.sizeof("World")
    c_size = lib.world_struct_size()
    if py_size != c_size:
        raise SystemExit(
            f"FATAL: World struct size mismatch -- cffi cdef {py_size} bytes vs "
            f"C sizeof(World) {c_size} bytes. Fix the [32] array sizes in "
            f"_sim_cdef.py to match MAX_CLIENTS."
        )

    head_sizes = [lib.HEAD_SIZES[i] for i in range(lib.NUM_HEADS)]
    print("[build_sim] _pulse_sim built and verified:")
    print(f"  sizeof(World) = {c_size} bytes (cffi and C agree)")
    print(f"  OBS_DIM={lib.OBS_DIM}  NUM_HEADS={lib.NUM_HEADS}  HEAD_SIZES={head_sizes}")
    print(f"  OBS_VERSION={lib.OBS_VERSION}  POLICY_HIDDEN={lib.POLICY_HIDDEN}  "
          f"MAX_CLIENTS={lib.MAX_CLIENTS}")
    print(f"  ARENA_HALF_EXTENT={lib.ARENA_HALF_EXTENT}  EYE_HEIGHT={lib.EYE_HEIGHT}  "
          f"MOVE_SPEED={lib.MOVE_SPEED}  TICK_RATE={lib.TICK_RATE}")


if __name__ == "__main__":
    os.chdir(ROOT)  # so relative source paths in any warning are readable
    build()
    verify()
