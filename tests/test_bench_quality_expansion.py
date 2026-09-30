import unittest

try:
    import mpmath as mp
except ImportError:
    mp = None

if mp is not None:
    from bench_quality_expansion import compare, evaluate, make_cases, profile_args, quality


@unittest.skipIf(mp is None, 'Optional quality benchmark requires mpmath')
class QualityExpansionTests(unittest.TestCase):
    def test_frozen_groups(self):
        cases = make_cases()
        self.assertEqual(len(cases), 48)
        self.assertEqual(len({v['id'] for v in cases}), 48)
        self.assertEqual(cases, make_cases())
        self.assertTrue(all(mp.isfinite(mp.mpf(v['target'])) for v in cases))

    def test_sine_identity(self):
        difference = evaluate('cos(sin(1))+tan(ln(2))') - evaluate('tan(ln(2))+sin(asin(1)+sin(1))')
        self.assertLess(abs(difference), mp.mpf('1e-75'))

    def test_parser(self):
        self.assertEqual(evaluate('2×3+2^3'), 14)
        self.assertEqual(evaluate('inv(4)+(-2)^3'), mp.mpf('-7.75'))
        self.assertEqual(evaluate('0.123456789012345678901234567890'), mp.mpf('0.123456789012345678901234567890'))
        for expression in ('__import__("os")', 'pi.real', 'sqrt(-1)'):
            with self.assertRaises(ValueError):
                evaluate(expression)

    def test_independent_precision(self):
        result = quality({'results': [{'expression': '1+1/10000000000000000', 'cost': 5, 'absolute_error': 0}]}, '1')
        self.assertLess(abs(mp.mpf(result['best']['mp_error']) - mp.mpf('1e-16')), mp.mpf('1e-75'))

    def test_comparison_floor_and_direction(self):
        def sample(error):
            return {'best': {'mp_normalized_error': error}, 'verified': [], 'hits': {'1e-14': 0}}
        self.assertEqual(compare(sample('1e-8'), sample('1e-10'))['status'], 'improved')
        self.assertEqual(compare(sample('1e-10'), sample('1e-8'))['status'], 'regressed')
        self.assertEqual(compare(sample('1e-16'), sample('1e-30'))['status'], 'similar')

    def test_profiles_disable_cos(self):
        for profile in ('screen', 'original-budget'):
            arguments = profile_args(profile, 16)
            self.assertNotIn('cos', arguments[arguments.index('--ops')+1].split(','))
            self.assertEqual(arguments[arguments.index('--results')+1], '100')


if __name__ == '__main__':
    unittest.main()
