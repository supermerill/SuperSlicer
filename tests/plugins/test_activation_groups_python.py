"""Check the Python solidarity wrapper against the generated C call contract.

Run with --generated-dir pointing to the build directory containing
slic3r_api_generated.py. Native registry semantics are covered by C++ tests.
"""

import argparse
import ctypes
from pathlib import Path
import sys
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--generated-dir", required=True)
options = parser.parse_args()
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "src/libslic3r/Api/plugin/python"))
sys.path.insert(0, options.generated_dir)

import slic3r_api
from slic3r_api_generated import C_FUNCTION_SIGNATURES, ConstStrings


class RecordingHost:
    def __init__(self, result=1):
        self.result = result
        self.calls = []

    def orchestrator_register_activation_group(self, orchestrator, group_id, members):
        self.calls.append((orchestrator.value, group_id, [ctypes.string_at(members.items[i]) for i in range(members.size)]))
        return self.result


class ActivationGroupTests(unittest.TestCase):
    def make_api(self, result=1):
        api = object.__new__(slic3r_api.Slic3rAPI)
        api.orchestrator = ctypes.c_void_p(123)
        api.host = RecordingHost(result)
        return api

    def test_generated_signature(self):
        signature = next(s for s in C_FUNCTION_SIGNATURES if s[0] == "orchestrator_register_activation_group")
        self.assertEqual(signature[1], ctypes.c_int32)
        self.assertEqual(signature[2][-1], ConstStrings)

    def test_strings_and_generator_remain_alive_during_call(self):
        api = self.make_api()
        self.assertIsNone(api.register_activation_group("solid", (name for name in ["a", "b"])))
        self.assertEqual(api.host.calls, [(123, b"solid", [b"a", b"b"])])

    def test_repeated_declarations_are_forwarded_not_overwritten(self):
        api = self.make_api()
        api.register_activation_group("solid", ["a", "b"])
        api.register_activation_group("solid", ["b", "c"])
        self.assertEqual(len(api.host.calls), 2)
        self.assertEqual(api.host.calls[1][2], [b"b", b"c"])

    def test_host_rejection_raises(self):
        api = self.make_api(0)
        with self.assertRaisesRegex(ValueError, "solid"):
            api.register_activation_group("solid", ["a", "a"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
