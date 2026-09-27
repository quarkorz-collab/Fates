"""Positive/negative controls for the independent Decimal root oracle."""
from decimal import Decimal, localcontext
import math
import unittest

from check_equation_regressions import PI, sincos, verify_root


class EquationOracleTests(unittest.TestCase):
    def test_basic_and_tangential_roots(self):
        verify_root("x×x = 2", math.sqrt(2))
        verify_root("cos(x) = x", 0.7390851332151607)
        verify_root("x = x^x", 1.0)

    def test_large_common_term_requires_the_actual_root(self):
        equation = "inv(x×pi)-x = sin(x)/x-x"
        with self.assertRaises(AssertionError):
            verify_root(equation, 777777.0)
        root = verify_root(equation, 777776.9835659464)
        self.assertLess(abs(root - Decimal("777776.98356594640319987045392487027583")), Decimal("1e-30"))

    def test_impossible_equation_and_identity_are_rejected(self):
        for equation, root in (("x+gamma/x^phi = inv(gamma+inv(x)-gamma)", 77777.0),
                               ("x = x", 3.0), ("x×x+1e-18 = 0", 0.0)):
            with self.subTest(equation=equation), self.assertRaises(AssertionError):
                verify_root(equation, root)

    def test_high_precision_trigonometry(self):
        with localcontext() as context:
            context.prec = 80
            sine, cosine = sincos(PI)
            self.assertLess(abs(sine), Decimal("1e-75"))
            self.assertLess(abs(cosine + 1), Decimal("1e-75"))
            sine, cosine = sincos(PI / 2)
            self.assertLess(abs(sine - 1), Decimal("1e-75"))
            self.assertLess(abs(cosine), Decimal("1e-75"))


if __name__ == "__main__":
    unittest.main()
