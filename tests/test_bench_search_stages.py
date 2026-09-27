"""Regression checks for the stage benchmark's equivalence checks."""

import copy
import json
import unittest

from bench_search_stages import fingerprint, live_summary


class EquivalenceTests(unittest.TestCase):
    def test_only_time_and_thread_count_are_ignored(self):
        payload = {"results": [{"expression": "pi"}],
                   "stats": {"attempted": 3, "valid": 2, "kept": 1,
                             "seconds": 1.0, "stage_seconds": {"deep": 0.5}, "threads": 1}}
        changed = copy.deepcopy(payload)
        changed["stats"].update(seconds=5.0, stage_seconds={"deep": 4.0}, threads=16)
        self.assertEqual(fingerprint(payload), fingerprint(changed))
        for key in ("attempted", "valid", "kept"):
            different = copy.deepcopy(changed)
            different["stats"][key] += 1
            self.assertNotEqual(fingerprint(payload), fingerprint(different))
        changed["results"][0]["expression"] = "e"
        self.assertNotEqual(fingerprint(payload), fingerprint(changed))

    @staticmethod
    def frame(elapsed, expression="pi"):
        return "[fates-live] " + json.dumps({
            "type": "top", "elapsed": elapsed, "cost": 16, "capacity": 20,
            "results": [{"rank": 1, "expression": expression}],
        })

    def test_live_ignores_timing_and_intermediate_frame_count(self):
        first = live_summary(self.frame(1.0))
        repeated = live_summary("progress log\n" + self.frame(2.0, "e") + "\n" + self.frame(3.0))
        self.assertEqual(first["fingerprint"], repeated["fingerprint"])
        self.assertEqual(repeated["frame_count"], 2)
        self.assertNotEqual(first["fingerprint"], live_summary(self.frame(1.0, "e"))["fingerprint"])

    def test_missing_malformed_and_out_of_order_live_frames_fail(self):
        for stderr in ("no live frames", "[fates-live] not JSON",
                       '[fates-live] {"type":"top"}',
                       self.frame(2.0) + "\n" + self.frame(1.0)):
            with self.subTest(stderr=stderr):
                with self.assertRaises((RuntimeError, ValueError)):
                    live_summary(stderr)


if __name__ == "__main__":
    unittest.main()
