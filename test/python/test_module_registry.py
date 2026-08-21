import unittest

import config.module_registry


MODULE_INFO = {
    'branch': {
        # Deliberately NOT in sorted order: the emitter must sort, not inherit
        # dict-insertion order, or the generated file churns between runs.
        'branchDgshare': {'name': 'branchDgshare', 'path': 'branch/gshare', 'legacy': False, 'class': 'gshare'},
        'branchDbimodal': {'name': 'branchDbimodal', 'path': 'branch/bimodal', 'legacy': False, 'class': 'bimodal'},
    },
    'btb': {
        'btbDbasic_btb': {'name': 'btbDbasic_btb', 'path': 'btb/basic_btb', 'legacy': False, 'class': 'basic_btb'},
    },
    'pref': {
        'prefetcherDno': {'name': 'prefetcherDno', 'path': 'prefetcher/no', 'legacy': False, 'class': 'no'},
        'prefetcherDold': {'name': 'prefetcherDold', 'path': 'prefetcher/old', 'legacy': True, 'class': 'old'},
    },
    'repl': {
        'replacementDlru': {'name': 'replacementDlru', 'path': 'replacement/lru', 'legacy': False, 'class': 'lru'},
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

    def test_a_legacy_module_is_not_registered(self):
        # Legacy modules go through generated free-function shims, not the
        # class-based model the registry instantiates.
        text = ' '.join(self.lines())
        self.assertNotIn('"old"', text)


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
        self.assertIn('compose multiple modules at config.sh time', text)

    def test_module_headers_are_included(self):
        text = ' '.join(self.lines())
        self.assertIn('branch/bimodal', text)
        self.assertIn('#include', text)

    def test_legacy_modules_are_skipped_entirely(self):
        text = ' '.join(self.lines())
        self.assertNotIn('prefetcher/old', text)


class SelectionEmissionTest(unittest.TestCase):
    def cpu_lines(self, cpu):
        return list(config.module_registry.module_selection_lines(
            'cores', 0, 'ooo_cpu.cpu0',
            [('branch_predictor', 'install_branch_module', 'make_branch', cpu['_branch_predictor_data']),
             ('btb', 'install_btb_module', 'make_btb', cpu['_btb_data'])]))

    def test_single_module_emits_lookup_swap_and_configure(self):
        cpu = {'_branch_predictor_data': [{'class': 'bimodal', 'legacy': False}],
               '_btb_data': [{'class': 'basic_btb', 'legacy': False}]}
        text = ' '.join(self.cpu_lines(cpu))
        self.assertIn('cfg.value<std::string>("ooo_cpu.cpu0.branch_predictor", "bimodal")', text)
        self.assertIn('cores.at(0).install_branch_module(champsim::configured::module_registry::make_branch(sel, &cores.at(0)))', text)
        # configure runs for the SELECTED module -- baked or swapped -- with the
        # module-name table as its prefix.
        self.assertIn('impl_configure(cfg, std::string{"ooo_cpu.cpu0."} + sel)', text)

    def test_the_swap_fires_only_when_the_selection_differs_from_the_baked_pack(self):
        # Pins the guard's DIRECTION: inverting != to == would swap on every
        # default run and skip every override.
        cpu = {'_branch_predictor_data': [{'class': 'bimodal', 'legacy': False}],
               '_btb_data': [{'class': 'basic_btb', 'legacy': False}]}
        text = ' '.join(self.cpu_lines(cpu))
        self.assertIn('if (sel != "bimodal") {', text)
        self.assertIn('if (sel != "basic_btb") {', text)

    def test_a_multi_module_pack_keeps_its_joined_default(self):
        cpu = {'_branch_predictor_data': [{'class': 'a', 'legacy': False}, {'class': 'b', 'legacy': False}],
               '_btb_data': [{'class': 'basic_btb', 'legacy': False}]}
        text = ' '.join(self.cpu_lines(cpu))
        self.assertIn('cfg.value<std::string>("ooo_cpu.cpu0.branch_predictor", "a,b")', text)

    def test_a_composed_pack_gets_configure_only_after_a_swap(self):
        # A standing composed pack has no knob table (a shared prefix would
        # collide), but a runtime override installs a SINGLE module, which must
        # then receive its table -- otherwise its knobs are fatally unconsumed.
        cpu = {'_branch_predictor_data': [{'class': 'a', 'legacy': False}, {'class': 'b', 'legacy': False}],
               '_btb_data': [{'class': 'basic_btb', 'legacy': False}]}
        lines = self.cpu_lines(cpu)
        text = '\n'.join(lines)
        bp_block = text[:text.index('btb')]
        self.assertIn('impl_configure', bp_block)
        # ... but only inside the swap branch, never for the standing pack:
        configure_at = bp_block.index('impl_configure')
        guard_at = bp_block.index('if (sel != "a,b")')
        close_at = bp_block.index('}', bp_block.index('install_branch_module'))
        self.assertTrue(guard_at < configure_at < close_at + len('}'))


class JoinSafetyTest(unittest.TestCase):
    def test_the_header_is_guarded_and_the_impl_includes_it(self):
        # Include order must not matter: clang-format sorts includes within a
        # block, so the definitions cannot rely on a declaration included
        # before them by hand.
        header = '\n'.join(config.module_registry.registry_class_lines(MODULE_INFO))
        self.assertIn('#ifndef CHAMPSIM_GENERATED_REGISTRY_INC', header)
        self.assertIn('#endif', header)
        impl = '\n'.join(config.module_registry.registry_impl_lines(MODULE_INFO))
        self.assertIn('#include "registry.inc"', impl)

    def test_the_impl_fragment_does_not_include_core_inst(self):
        # Regression: a multi-executable configure joins one registry fragment
        # per build id into one file; core_inst.inc has no include guard, so a
        # per-fragment include is a redefinition error. The fixed TU includes
        # it exactly once instead.
        text = ' '.join(config.module_registry.registry_impl_lines(MODULE_INFO))
        self.assertNotIn('core_inst.inc', text)
