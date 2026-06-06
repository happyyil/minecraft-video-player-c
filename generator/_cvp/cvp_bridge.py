"""
cvp_bridge.py — Bridge between Python and the C frame_tiler program.

When frame_tiler.exe is compiled and available, this module uses it for fast
frame extraction, tiling, and PNG encoding. Otherwise, it falls back to the
pure Python pipeline.
"""

import json
import os
import subprocess
from typing import Optional, Tuple

# Try to find the compiled frame_tiler executable
_FRAME_TILER_PATH = None


def _find_frame_tiler() -> Optional[str]:
    """Find the compiled frame_tiler executable."""
    global _FRAME_TILER_PATH
    if _FRAME_TILER_PATH is not None:
        return _FRAME_TILER_PATH

    this_dir = os.path.dirname(os.path.abspath(__file__))

    # Try common locations
    candidates = [
        os.path.join(this_dir, "frame_tiler.exe"),   # Windows
        os.path.join(this_dir, "frame_tiler"),        # Linux/macOS
        os.path.join(this_dir, "build", "frame_tiler.exe"),
        os.path.join(this_dir, "build", "frame_tiler"),
    ]

    for path in candidates:
        if os.path.isfile(path) and os.access(path, os.X_OK):
            _FRAME_TILER_PATH = path
            return path

    # Try PATH
    for name in ["frame_tiler.exe", "frame_tiler"]:
        import shutil
        found = shutil.which(name)
        if found:
            _FRAME_TILER_PATH = found
            return found

    _FRAME_TILER_PATH = None  # Not found
    return None


def _check_cvp_available() -> bool:
    """Check if the C frame_tiler is available."""
    return _find_frame_tiler() is not None


def process_frames_with_cvp(
    video_path: str,
    output_dir: str,
    output_size: Optional[Tuple[int, int]],
    output_fps: Optional[float],
    tile_size: int = 256,
    max_workers: int = 16,
    ffmpeg_exec_path: Optional[str] = None,
) -> Optional[dict]:
    """
    Process video using the C frame_tiler program.
    Returns metadata dict or None if failed.
    """
    tiler_path = _find_frame_tiler()
    if not tiler_path:
        return None

    target_w = output_size[0] if output_size else 0
    target_h = output_size[1] if output_size else 0
    target_fps = output_fps or 0.0

    # CLI args must match C main() expectation:
    # argv[1]=video_path, [2]=output_dir, [3]=width, [4]=height,
    # [5]=fps, [6]=tile_size, [7]=workers, [8]=ffmpeg_path
    cmd = [
        tiler_path,
        video_path,
        output_dir,
        str(target_w),
        str(target_h),
        str(target_fps),
        str(tile_size),
        str(max_workers),
    ]

    if ffmpeg_exec_path:
        cmd.append(ffmpeg_exec_path)

    print(f"[CVP] Running frame_tiler: {' '.join(cmd)}")

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=3600,  # 1 hour max
        )

        # stderr has progress output
        if result.stderr:
            for line in result.stderr.strip().split('\n'):
                print(f"[CVP] {line}")

        if result.returncode != 0:
            print(f"[CVP] frame_tiler failed with code {result.returncode}")
            if result.stderr:
                print(f"[CVP] stderr: {result.stderr}")
            return None

        # stdout has JSON metadata
        meta_line = result.stdout.strip()
        if meta_line:
            meta = json.loads(meta_line)
            meta["width"] = output_size[0] if output_size else 0
            meta["height"] = output_size[1] if output_size else 0
            meta["fps"] = output_fps or 0.0
            meta["path"] = video_path
            return meta
        return None

    except subprocess.TimeoutExpired:
        print("[CVP] frame_tiler timed out")
        return None
    except Exception as e:
        print(f"[CVP] frame_tiler error: {e}")
        return None


def is_cvp_available() -> bool:
    """Check if the C frame_tiler is compiled and available."""
    return _check_cvp_available()
