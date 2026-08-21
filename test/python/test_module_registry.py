import unittest
import config.modules
import tempfile
import os

import config.module_registry


MODULE_INFO = {
    'branch': {
        # Deliberately NOT in sorted order: the emitter must sort, not inherit
        # dict-insertion order, or the generated file churns between runs.
        'branchDgshare': {'name': 'branchDgshare', 'path': 'branch/gshare', 'class': 'gshare'},
        'branchDbimodal': {'name': 'branchDbimodal', 'path': 'branch/bimodal', 'class': 'bimodal'},
    },
    'btb': {
        'btbDbasic_btb': {'name': 'btbDbasic_btb', 'path': 'btb/basic_btb', 'class': 'basic_btb'},
    },
    'pref': {
        'prefetcherDno': {'name': 'prefetcherDno', 'path': 'prefetcher/no', 'class': 'no'},
    },
    'repl': {
        'replacementDlru': {'name': 'replacementDlru', 'path': 'replacement/lru', 'class': 'lru'},
    },
}
class RegistryClassTest(unittest.TestCase):
    def lines(self):
        return list(config.module_registry.registry_class_lines(MODULE_INFO))

    def test_the_registry_is_a_plain_struct(self):
        text = '\n'.join(self.lines())
        self.assertIn('struct champsim::configured::module_registry', text)
        self.assertNotIn('<0x', text)

    def test_the_header_is_self_contained(self):
        # It is included on its own, so it cannot lean on another generated
        # file's includes.
        text = '\n'.join(self.lines())
        for header in ('<array>', '<memory>', '<string_view>', '"environment.h"', '"cache.h"', '"ooo_cpu.h"'):
            with self.subTest(header=header):
                self.assertIn(f'#include {header}', text)

    def test_names_are_sorted_per_kind(self):
        text = ' '.join(self.lines())
        self.assertIn('"bimodal"', text)
        self.assertIn('"gshare"', text)
        self.assertLess(text.index('"bimodal"'), text.index('"gshare"'))

    def test_four_factories_are_declared(self):
        text = ' '.join(self.lines())
        for factory in ('make_branch', 'make_btb', 'make_prefetcher', 'make_replacement'):
            self.assertIn(factory, text)

class RegistryImplTest(unittest.TestCase):
    def lines(self):
        return list(config.module_registry.registry_impl_lines(MODULE_INFO))

    def test_each_module_maps_to_its_model(self):
        text = ' '.join(self.lines())
        self.assertIn('std::make_unique<O3_CPU::branch_module_model<class bimodal>>(owner)', text)
        self.assertIn('std::make_unique<CACHE::replacement_module_model<class lru>>(owner)', text)

    def test_an_unknown_name_throws_listing_the_valid_names(self):
        text = ' '.join(self.lines())
        self.assertIn('throw std::runtime_error', text)
        self.assertIn('valid branch_predictor modules', text)

    def test_a_comma_is_rejected_as_multi_module(self):
        # Runtime selection is single-module (composition stays configure-time);
        # the error must say so rather than reporting an unknown name. Pin the
        # user-visible message, not the implementation of the check.
        text = ' '.join(self.lines())
        self.assertIn('one module', text)
        # The advice must not name a capability that does not exist: config.sh
        # composed module packs once, and no longer does anything of the kind.
        self.assertIn('module composition is not supported', text)
        self.assertNotIn('config.sh time', text)

    def test_module_headers_are_included(self):
        text = ' '.join(self.lines())
        self.assertIn('branch/bimodal', text)
        self.assertIn('#include', text)

    def test_the_header_is_guarded_and_the_impl_includes_it(self):
        # Include order must not matter: clang-format sorts includes within a
        # block, so the definitions cannot rely on a declaration included
        # before them by hand.
        header = '\n'.join(config.module_registry.registry_class_lines(MODULE_INFO))
        self.assertIn('#ifndef CHAMPSIM_GENERATED_REGISTRY_INC', header)
        self.assertIn('#endif', header)
        impl = '\n'.join(config.module_registry.registry_impl_lines(MODULE_INFO))
        self.assertIn('#include "registry.inc"', impl)

    def test_the_impl_fragment_includes_only_its_own_declaration(self):
        # The fragment is compiled by a fixed TU (src/generated_registry.cc),
        # so the only generated header it may pull in is the guarded
        # registry.inc. An unguarded generated header included from here was a
        # redefinition error once already; both named below are now deleted,
        # which is exactly why nothing should reintroduce an include of one.
        text = ' '.join(config.module_registry.registry_impl_lines(MODULE_INFO))
        self.assertNotIn('core_inst.inc', text)
        self.assertNotIn('module_decl.inc', text)


class LegacyMarkerTest(unittest.TestCase):
    def test_a_legacy_marker_is_rejected_at_discovery(self):
        # The free-function module style needed config/legacy.py and Makefile
        # rules that are gone. Discovery has to refuse the marker: compiling
        # the sources and leaving the class unregistered would produce a module
        # that builds fine and can never be selected.
        with tempfile.TemporaryDirectory() as root:
            module = os.path.join(root, 'branch', 'oldpred')
            os.makedirs(module)
            open(os.path.join(module, '__legacy__'), 'w').close()
            open(os.path.join(module, 'oldpred.cc'), 'w').close()

            context = config.modules.ModuleSearchContext([os.path.join(root, 'branch')])
            with self.assertRaises(RuntimeError) as caught:
                context.find_all()
            self.assertIn('no longer supported', str(caught.exception))
