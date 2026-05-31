# Copyright © 2024 Collabora Ltd. and Red Hat Inc.
# SPDX-License-Identifier: MIT

import unittest
from unittest.mock import MagicMock
import sys

# Mock mako before importing lat_rs_gen
sys.modules['mako'] = MagicMock()
sys.modules['mako.template'] = MagicMock()

import lat_rs_gen

class TestLatRsGen(unittest.TestCase):
    def test_to_camel(self):
        self.assertEqual(lat_rs_gen.to_camel("snake_case"), "SnakeCase")
        self.assertEqual(lat_rs_gen.to_camel("alreadyCamel"), "Alreadycamel")
        self.assertEqual(lat_rs_gen.to_camel("123_prefixed"), "_123Prefixed")
        self.assertEqual(lat_rs_gen.to_camel("foo_123"), "Foo123")
        self.assertEqual(lat_rs_gen.to_camel(""), "")

    def test_parse_csv(self):
        csv_data = [
            "header1,header2",
            "# comment",
            "val1,val2",
            "",
            "val3,val4"
        ]
        expected = [
            ["header1", "header2"],
            ["val1", "val2"],
            ["val3", "val4"]
        ]
        result = list(lat_rs_gen.parse_csv(csv_data))
        self.assertEqual(result, expected)

    def test_fld_parsing(self):
        # none case
        fld = lat_rs_gen.Fld("none")
        self.assertFalse(fld.valid)

        # pred case
        fld = lat_rs_gen.Fld("10+5")
        self.assertTrue(fld.valid)
        self.assertTrue(fld.pred)
        self.assertEqual(fld.value, "10")
        self.assertEqual(fld.pred_val, "5")

        # scoreboard case
        fld = lat_rs_gen.Fld("8 & sb")
        self.assertTrue(fld.valid)
        self.assertTrue(fld.scoreboard)
        self.assertEqual(fld.value, "8")

        # normal case
        fld = lat_rs_gen.Fld("15")
        self.assertTrue(fld.valid)
        self.assertFalse(fld.pred)
        self.assertFalse(fld.scoreboard)
        self.assertEqual(fld.value, "15")

    def test_header_parsing(self):
        # raw
        h = lat_rs_gen.Header(["raw", "cat1", "cat2"])
        self.assertEqual(h.latcat, "raw")
        self.assertEqual(h.cat0, "writer")
        self.assertEqual(h.cat1, "reader")
        self.assertEqual(h.cats, ["cat1", "cat2"])

        # war
        h = lat_rs_gen.Header(["war", "cat1", "cat2"])
        self.assertEqual(h.latcat, "war")
        self.assertEqual(h.cat0, "reader")
        self.assertEqual(h.cat1, "writer")

        # waw
        h = lat_rs_gen.Header(["waw", "cat1", "cat2"])
        self.assertEqual(h.latcat, "waw")
        self.assertEqual(h.cat0, "writer1")
        self.assertEqual(h.cat1, "writer2")

    def test_fields_parsing(self):
        h = lat_rs_gen.Header(["raw", "c1", "c2"])
        f = lat_rs_gen.Fields(h, ["row_cat", "10", "none"])
        self.assertEqual(f.fldcat, "row_cat")
        self.assertTrue(f.flds["c1"].valid)
        self.assertEqual(f.flds["c1"].value, "10")
        self.assertFalse(f.flds["c2"].valid)

if __name__ == '__main__':
    unittest.main()
