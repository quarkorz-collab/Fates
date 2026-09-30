"""Guard the policy report's compatibility comparison against false passes."""
import copy
import unittest

try:
    from summarize_completion_policy import legacy_payload
except ModuleNotFoundError as error:
    if error.name != 'mpmath':
        raise
    legacy_payload = None


@unittest.skipIf(legacy_payload is None, 'Optional quality reports require mpmath')
class PolicyReportTests(unittest.TestCase):
    def setUp(self):
        self.old = {'completed_cost': 20, 'results': [{'expression': '1+2', 'cost': 3}],
                    'stats': {'attempted': 100, 'kept': 20, 'seconds': 1, 'threads': 1,
                              'stage_seconds': {'deep': 0.1}}}

    def test_only_new_diagnostics_and_time_are_ignored(self):
        current = copy.deepcopy(self.old)
        current['completion_mode'] = 'full'
        current['stats'].update(completion_work=50, completion_limit=0, inverse_requests_scored=5,
                                seconds=2, threads=16, stage_seconds={'deep': 0.2})
        self.assertEqual(legacy_payload(current), legacy_payload(self.old))

    def test_old_counters_and_expression_changes_are_not_ignored(self):
        for mutate in (lambda value: value['stats'].update(attempted=101),
                       lambda value: value['results'][0].update(cost=4),
                       lambda value: value['results'][0].update(expression='2+1')):
            current = copy.deepcopy(self.old)
            mutate(current)
            self.assertNotEqual(legacy_payload(current), legacy_payload(self.old))


if __name__ == '__main__':
    unittest.main()
