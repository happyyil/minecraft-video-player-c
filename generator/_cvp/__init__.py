"""
_cvp package — C-optimized video frame processing for Minecraft Video Player.

When frame_tiler is compiled, it provides faster frame extraction, tiling,
and PNG encoding compared to the pure Python pipeline.

Usage:
    python -m generator._cvp build     # Compile the C program
    python -m generator._cvp test      # Check if it's available
"""
from .cvp_bridge import is_cvp_available, process_frames_with_cvp

__all__ = ["is_cvp_available", "process_frames_with_cvp"]
