#!/usr/bin/env python3
"""Unit tests for kenshi2mde.py's seam-validation logic, on synthetic
data only (no Kenshi game files -- per TERRAIN_MASTER_PROMPT.md's legal
constraint: no game assets in the repo, only parser code + own test
data).

Run: python3 tools/kenshi_import/test_kenshi2mde.py
"""
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from kenshi2mde import check_seams  # noqa: E402


def make_watertight_atlas(zones=3, verts=5):
    """A small synthetic atlas where every zone's edge is constructed to
    exactly equal its neighbour's opposite edge, mimicking tif_to_r32.py's
    real "seam=0.0m" construction (each zone samples the shared boundary
    pixel identically, not independently)."""
    rng = np.random.default_rng(42)
    tiled = np.zeros((zones, zones, verts, verts), dtype=np.float32)
    for zy in range(zones):
        for zx in range(zones):
            tiled[zy, zx] = rng.uniform(0, 100, size=(verts, verts)).astype(np.float32)
    # Stitch: each zone's right edge = next zone's left edge.
    for zy in range(zones):
        for zx in range(zones - 1):
            tiled[zy, zx + 1, :, 0] = tiled[zy, zx, :, -1]
    # Stitch: each zone's bottom edge = next zone's top edge.
    for zy in range(zones - 1):
        for zx in range(zones):
            tiled[zy + 1, zx, 0, :] = tiled[zy, zx, -1, :]
    return tiled


class TestCheckSeams(unittest.TestCase):
    def test_watertight_atlas_has_zero_mismatch(self):
        tiled = make_watertight_atlas()
        max_mismatch, checked = check_seams(tiled)
        self.assertEqual(max_mismatch, 0.0)
        # 3x3 zones: 2 horizontal seams/row * 3 rows + 2 vertical seams/col * 3 cols = 12
        self.assertEqual(checked, 12)

    def test_single_zone_has_no_seams_to_check(self):
        tiled = make_watertight_atlas(zones=1)
        max_mismatch, checked = check_seams(tiled)
        self.assertEqual(checked, 0)
        self.assertEqual(max_mismatch, 0.0)

    def test_introduced_mismatch_is_detected(self):
        tiled = make_watertight_atlas()
        # Corrupt one shared edge by a known amount -- the function must
        # report a mismatch of at least that magnitude, not silently pass.
        tiled[1, 1, :, 0] += 5.0
        max_mismatch, checked = check_seams(tiled)
        self.assertGreaterEqual(max_mismatch, 5.0)

    def test_tolerance_respects_small_floating_point_noise(self):
        tiled = make_watertight_atlas()
        # Sub-tolerance noise (e.g. float32 round-trip through a uint16
        # quantization step, as the real world_hmap.r16 format uses) should
        # still be caught if it exceeds an exact (0.0) tolerance -- this
        # test documents that --validate's default tolerance is exact, and
        # any real quantization noise must be explicitly allowed via
        # --tolerance-m, not silently ignored.
        tiled[0, 0, :, -1] += 0.001
        max_mismatch, _ = check_seams(tiled)
        self.assertGreater(max_mismatch, 0.0)
        self.assertLess(max_mismatch, 0.01)


if __name__ == "__main__":
    unittest.main()
