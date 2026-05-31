"""
Build script for frame_tiler.c

Usage:
    python -m generator._cvp build     # Compile frame_tiler
    python -m generator._cvp run ...   # Run frame_tiler directly
    python -m generator._cvp test      # Test if it's compiled and works

No external dependencies needed! stb_image_write.h is a single-header PNG encoder.
Only needs a C compiler (gcc, clang, or MSVC).
"""
import os
import subprocess
import sys
import shutil


def get_compiler():
    """Find a C compiler."""
    # Try gcc first (MinGW on Windows)
    for cc in ["gcc", "cc", "clang"]:
        if shutil.which(cc):
            return cc
    # Try MSVC via cl.exe
    cl = shutil.which("cl")
    if cl:
        return "cl"
    return None


def build():
    """Compile frame_tiler.c to frame_tiler.exe (or frame_tiler on Unix)."""
    this_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(this_dir)

    src = "frame_tiler.c"
    out = "frame_tiler.exe" if sys.platform.startswith("win") else "frame_tiler"

    cc = get_compiler()
    if not cc:
        print("[CVP Build] ERROR: No C compiler found!")
        print("[CVP Build] Install MinGW (gcc), MSVC (cl), or clang.")
        return False

    print(f"[CVP Build] Using compiler: {cc}")

    if cc == "cl":
        # MSVC
        cmd = [cc, src, "/O2", "/Fe:" + out, "/W3"]
    else:
        # GCC / Clang
        cmd = [cc, src, "-O3", "-o", out, "-Wall", "-Wno-unused-function"]

    print(f"[CVP Build] Compiling: {' '.join(cmd)}")

    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode != 0:
        print(f"[CVP Build] Compilation FAILED:")
        if result.stderr:
            print(result.stderr)
        if result.stdout:
            print(result.stdout)
        return False

    out_path = os.path.join(this_dir, out)
    size = os.path.getsize(out_path)
    print(f"[CVP Build] SUCCESS: {out} ({size:,} bytes)")
    print(f"[CVP Build] Output: {out_path}")
    return True


def test():
    """Test if frame_tiler is compiled."""
    from .cvp_bridge import _find_frame_tiler

    path = _find_frame_tiler()
    if path:
        print(f"[CVP Test] frame_tiler found: {path}")
        print(f"[CVP Test] Size: {os.path.getsize(path):,} bytes")
        # Run with --help to verify it works
        result = subprocess.run([path], capture_output=True, text=True)
        if result.returncode != 0 and "Usage:" in (result.stderr or result.stdout):
            print("[CVP Test] frame_tiler runs correctly")
            return True
    else:
        print("[CVP Test] frame_tiler NOT found!")
        print("[CVP Test] Run 'python -m generator._cvp build' first")
        return False


if __name__ == "__main__":
    cmd = "build"
    if len(sys.argv) > 1:
        cmd = sys.argv[1]

    if cmd == "build":
        success = build()
        sys.exit(0 if success else 1)
    elif cmd == "test":
        success = test()
        sys.exit(0 if success else 1)
    elif cmd == "run":
        # Pass through args to frame_tiler
        from .cvp_bridge import _find_frame_tiler
        path = _find_frame_tiler()
        if not path:
            print("[CVP] frame_tiler not found. Run build first.")
            sys.exit(1)
        result = subprocess.run([path] + sys.argv[2:])
        sys.exit(result.returncode)
    else:
        print(f"Unknown command: {cmd}")
        print("Usage: python -m generator._cvp [build|test|run]")
        sys.exit(1)
