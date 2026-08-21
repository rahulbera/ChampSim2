'''
inc/defs.h names the module a component uses when no runtime key selects one.
src/static_environment.cc skips the registry entirely when a selection equals
that name, trusting that the builder in inc/defaults.hpp already baked it.

Nothing in C++ ties the two together: a mismatch compiles, runs, and reports
in [config] a module the run did not actually use. That is the bug this
guards -- it is a cross-file textual invariant, so it is checked here rather
than in the C++ suite.
'''

import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read(*parts):
    with open(os.path.join(ROOT, *parts)) as rfp:
        return rfp.read()


class TestDefaultsAgree(unittest.TestCase):
    def setUp(self):
        self.defs = read('inc', 'defs.h')
        self.defaults = read('inc', 'defaults.hpp')

    def defs_value(self, name):
        found = re.search(r'default_' + name + r'\s*=\s*"([^"]+)"', self.defs)
        self.assertIsNotNone(found, f'inc/defs.h declares no default_{name}')
        return found.group(1)

    def test_the_core_defaults_match_the_core_builder(self):
        holders = re.search(
            r'core_builder<champsim::core_builder_module_type_holder<(\w+)>,\s*'
            r'champsim::core_builder_module_type_holder<(\w+)>>',
            self.defaults)
        self.assertIsNotNone(holders, 'could not read default_core module holders')
        self.assertEqual(self.defs_value('branch_predictor'), holders.group(1))
        self.assertEqual(self.defs_value('btb'), holders.group(2))

    def test_the_cache_defaults_match_every_cache_builder(self):
        holders = re.findall(
            r'cache_builder<champsim::cache_builder_module_type_holder<(\w+)>,\s*'
            r'champsim::cache_builder_module_type_holder<(\w+)>>',
            self.defaults)
        # Every level, not just one: static_environment applies the same two
        # names to all of them, so a single divergent builder is a mismatch.
        self.assertGreaterEqual(len(holders), 7, holders)
        for prefetcher, replacement in holders:
            self.assertEqual(self.defs_value('prefetcher'), prefetcher)
            self.assertEqual(self.defs_value('replacement'), replacement)

    def test_each_default_names_a_module_that_exists(self):
        kinds = {'branch_predictor': 'branch', 'btb': 'btb',
                 'prefetcher': 'prefetcher', 'replacement': 'replacement'}
        for name, directory in kinds.items():
            with self.subTest(kind=name):
                self.assertTrue(
                    os.path.isdir(os.path.join(ROOT, directory, self.defs_value(name))),
                    f'default_{name} names no directory under {directory}/')


if __name__ == '__main__':
    unittest.main()
