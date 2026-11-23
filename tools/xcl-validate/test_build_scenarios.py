#!/usr/bin/env python3
"""Tests of the xcl-corpus code lookup used by build_scenarios.py."""
import unittest

from build_scenarios import xcl_program_name


class XclProgramNameTest(unittest.TestCase):
    CODES = {
        "ambient_map:CS_AmbientMapSkyCB32_c5465f20_Wrapped": "aabb",
        "indirect_diffuse_lighting:CS_ScreenProbes_Wrapped": "ccdd",
    }

    def test_leading_stage_code_is_optional(self):
        self.assertEqual(
            xcl_program_name("2ambient_map:CS_AmbientMapSkyCB32_c5465f20", self.CODES),
            "ambient_map:CS_AmbientMapSkyCB32_c5465f20_Wrapped",
        )
        self.assertEqual(
            xcl_program_name("indirect_diffuse_lighting:CS_ScreenProbes", self.CODES),
            "indirect_diffuse_lighting:CS_ScreenProbes_Wrapped",
        )

    def test_missing_name_is_none(self):
        self.assertIsNone(xcl_program_name("other:CS_Other", self.CODES))


if __name__ == "__main__":
    unittest.main()
