"""Shared paths for the profiling harness.

REPO  - the fallout2-ce checkout (this file lives in <repo>/tools/harness).
GAME  - the desktop test game directory (fallout2.cfg + master.dat etc.).
        Set FALLOUT_GAME_DIR to override; defaults to <repo>/../game.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
GAME = os.environ.get("FALLOUT_GAME_DIR", os.path.abspath(os.path.join(REPO, "..", "game")))
