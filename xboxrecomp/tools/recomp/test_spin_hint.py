import unittest

from tools.recomp.spin_hint import rewrite


def loop(body, cond="if (CMP_B(_fa, _fb)) goto loc_00001000; /* jb */"):
    return "loc_00001000: ;\n" + "".join("    " + b + "\n" for b in body) + "    " + cond + "\n"


class SpinHintTest(unittest.TestCase):
    def test_fence_poll_is_marked(self):
        code, n = rewrite(loop(["ecx = MEM32(edx);", "edi = esi;", "edi = edi - ecx;"]))
        self.assertEqual(n, 1)
        self.assertIn("{ RECOMP_SPIN_HINT(); goto loc_00001000; }", code)

    def test_mmio_poll_is_marked(self):
        _, n = rewrite(loop(["eax = MMIO_RD32(0xFE820010u);", "eax = eax & 0xFFFFFFFCu;"]))
        self.assertEqual(n, 1)

    def test_walking_loop_is_not(self):          # strlen: the address moves
        _, n = rewrite(loop(["eax = ZX8(MEM8(esi));", "esi++;"]))
        self.assertEqual(n, 0)

    def test_counter_loop_is_not(self):
        _, n = rewrite(loop(["eax = MEM32(ecx + 4);", "edx++;"]))
        self.assertEqual(n, 0)

    def test_store_is_not(self):
        _, n = rewrite(loop(["eax = MEM32(ecx);", "MEM32(ebx) = eax;"]))
        self.assertEqual(n, 0)

    def test_call_is_not(self):
        _, n = rewrite(loop(["eax = MEM32(ecx);", "sub_00401000();"]))
        self.assertEqual(n, 0)

    def test_register_only_loop_is_not(self):
        _, n = rewrite(loop(["ecx = ecx - 1;"]))
        self.assertEqual(n, 0)


if __name__ == "__main__":
    unittest.main()
