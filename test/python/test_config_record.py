import unittest
import tomllib

import config.config_record

# The [config] section records the parsed configuration that config.sh generated
# the binary from, so that a stats document says which machine produced it.
#
# It must obey the statistics document's own rules (see CLAUDE.md):
# lower_snake_case keys, NO ARRAYS -- every key holds a single scalar -- and a
# key that is not a bare TOML key is quoted, never rewritten. The quoting rules
# below mirror ::quote() and ::is_bare_key() in src/toml_printer.cc exactly; if
# the two ever disagree, one half of the document escapes differently from the
# other.


class TomlKeyTest(unittest.TestCase):
    def test_a_bare_key_is_left_alone(self):
        self.assertEqual(config.config_record.toml_key('cpu0_l1d'), 'cpu0_l1d')

    def test_digits_and_dashes_are_bare(self):
        self.assertEqual(config.config_record.toml_key('a-1_B'), 'a-1_B')

    def test_a_key_with_a_dot_is_quoted(self):
        self.assertEqual(config.config_record.toml_key('a.b'), '"a.b"')

    def test_a_key_with_a_space_is_quoted(self):
        self.assertEqual(config.config_record.toml_key('Channel 0'), '"Channel 0"')

    def test_an_empty_key_is_quoted(self):
        self.assertEqual(config.config_record.toml_key(''), '""')


class TomlValueTest(unittest.TestCase):
    def test_bool_is_lowercase(self):
        self.assertEqual(config.config_record.toml_value(True), 'true')
        self.assertEqual(config.config_record.toml_value(False), 'false')

    def test_bool_is_checked_before_int(self):
        # bool is a subclass of int in Python; testing int first would render
        # True as 1 and silently change the type a parser sees.
        self.assertEqual(config.config_record.toml_value(True), 'true')

    def test_int(self):
        self.assertEqual(config.config_record.toml_value(2048), '2048')

    def test_float_keeps_a_decimal_point(self):
        # pmem.frequency is 1600.0 in every shipped config. A bare "1600" would
        # be read back as an int.
        self.assertEqual(config.config_record.toml_value(1600.0), '1600.0')

    def test_string_is_a_basic_string(self):
        self.assertEqual(config.config_record.toml_value('lru'), '"lru"')

    def test_string_escapes_quote_and_backslash(self):
        self.assertEqual(config.config_record.toml_value('a"b\\c'), '"a\\"b\\\\c"')

    def test_string_escapes_the_named_control_characters(self):
        self.assertEqual(config.config_record.toml_value('\b\f\n\r\t'), '"\\b\\f\\n\\r\\t"')

    def test_other_control_characters_use_the_u_escape(self):
        self.assertEqual(config.config_record.toml_value('\x01'), '"\\u0001"')
        self.assertEqual(config.config_record.toml_value('\x7f'), '"\\u007F"')

    def test_a_list_of_scalars_becomes_a_joined_string(self):
        # The document forbids arrays outright, and a sub-table of flags would
        # make the key set depend on the values. A fixed-schema string does not.
        self.assertEqual(config.config_record.toml_value(['LOAD', 'PREFETCH']), '"LOAD,PREFETCH"')

    def test_an_empty_list_is_an_empty_string(self):
        self.assertEqual(config.config_record.toml_value([]), '""')


class ConfigRecordTest(unittest.TestCase):
    def render(self, executable_name='champsim', elements=None, config_file=None):
        elements = elements if elements is not None else {'cores': [], 'caches': [], 'ptws': [], 'pmem': {}, 'vmem': {}}
        config_file = config_file if config_file is not None else {}
        return '\n'.join(config.config_record.get_config_record_lines(executable_name, elements, config_file))

    def test_the_executable_name_is_recorded(self):
        doc = tomllib.loads(self.render(executable_name='cbp_blbp64t'))
        self.assertEqual(doc['config']['executable_name'], 'cbp_blbp64t')

    def test_globals_are_recorded(self):
        doc = tomllib.loads(self.render(config_file={'block_size': 64, 'num_cores': 2}))
        self.assertEqual(doc['config']['block_size'], 64)
        self.assertEqual(doc['config']['num_cores'], 2)

    def test_internal_keys_are_dropped(self):
        # _offset_bits is literally the C++ expression "champsim::lg2(64)" and
        # _defaults names a C++ object; neither is a machine parameter.
        elements = {'cores': [], 'ptws': [], 'pmem': {}, 'vmem': {},
                    'caches': [{'name': 'LLC', 'sets': 2048, '_offset_bits': 'champsim::lg2(64)',
                                '_defaults': 'champsim::defaults::default_llc'}]}
        doc = tomllib.loads(self.render(elements=elements))
        cache = doc['config']['cache']['llc']
        self.assertEqual(cache['sets'], 2048)
        self.assertNotIn('_offset_bits', cache)
        self.assertNotIn('_defaults', cache)

    def test_a_core_is_keyed_by_its_lowercased_name(self):
        elements = {'cores': [{'name': 'cpu0', 'rob_size': 352, 'branch_predictor': 'bimodal'}],
                    'caches': [], 'ptws': [], 'pmem': {}, 'vmem': {}}
        doc = tomllib.loads(self.render(elements=elements))
        self.assertEqual(doc['config']['ooo_cpu']['cpu0']['rob_size'], 352)
        self.assertEqual(doc['config']['ooo_cpu']['cpu0']['branch_predictor'], 'bimodal')

    def test_a_nested_dict_becomes_a_subtable(self):
        elements = {'cores': [{'name': 'cpu0', 'DIB': {'window_size': 16, 'sets': 32}}],
                    'caches': [], 'ptws': [], 'pmem': {}, 'vmem': {}}
        doc = tomllib.loads(self.render(elements=elements))
        self.assertEqual(doc['config']['ooo_cpu']['cpu0']['dib']['window_size'], 16)

    def test_cache_names_are_lowercased_to_match_the_statistics_tables(self):
        # [config.cache.cpu0_l1d] must sit at the same key as
        # [phase.<n>.roi.cache.cpu0_l1d], so the two can be joined.
        elements = {'cores': [], 'ptws': [], 'pmem': {}, 'vmem': {},
                    'caches': [{'name': 'cpu0_L1D', 'sets': 64}]}
        doc = tomllib.loads(self.render(elements=elements))
        self.assertIn('cpu0_l1d', doc['config']['cache'])

    def test_names_colliding_only_by_case_keep_their_exact_spelling(self):
        # Two tables with the same header is a hard TOML parse error, which would
        # make the WHOLE document unreadable -- not just this section.
        elements = {'cores': [], 'ptws': [], 'pmem': {}, 'vmem': {},
                    'caches': [{'name': 'Cache', 'sets': 1}, {'name': 'cache', 'sets': 2}]}
        doc = tomllib.loads(self.render(elements=elements))
        self.assertEqual(doc['config']['cache']['Cache']['sets'], 1)
        self.assertEqual(doc['config']['cache']['cache']['sets'], 2)

    def test_a_name_that_is_not_a_bare_key_is_quoted(self):
        elements = {'cores': [], 'ptws': [], 'pmem': {}, 'vmem': {},
                    'caches': [{'name': 'Channel 0', 'sets': 1}]}
        doc = tomllib.loads(self.render(elements=elements))
        self.assertIn('channel 0', doc['config']['cache'])

    def test_ptw_pmem_and_vmem_are_recorded(self):
        elements = {'cores': [], 'caches': [],
                    'ptws': [{'name': 'cpu0_PTW', 'pscl5_set': 1}],
                    'pmem': {'frequency': 1600.0, 'channels': 1},
                    'vmem': {'pte_page_size': 4096}}
        doc = tomllib.loads(self.render(elements=elements))
        self.assertEqual(doc['config']['ptw']['cpu0_ptw']['pscl5_set'], 1)
        self.assertEqual(doc['config']['pmem']['frequency'], 1600.0)
        self.assertEqual(doc['config']['vmem']['pte_page_size'], 4096)


class RealConfigTest(unittest.TestCase):
    def test_the_shipped_config_round_trips_through_tomllib(self):
        import json
        import config.parse
        with open('champsim_config.json') as rfp:
            name, elements, _, _, config_file = config.parse.parse_config(json.load(rfp))
        text = '\n'.join(config.config_record.get_config_record_lines(name, elements, config_file))
        doc = tomllib.loads(text)['config']

        self.assertEqual(doc['executable_name'], 'champsim')
        self.assertEqual(doc['block_size'], 64)
        self.assertEqual(doc['ooo_cpu']['cpu0']['rob_size'], 352)
        self.assertEqual(doc['ooo_cpu']['cpu0']['branch_predictor'], 'bimodal')
        self.assertEqual(doc['ooo_cpu']['cpu0']['btb'], 'basic_btb')
        self.assertEqual(doc['ooo_cpu']['cpu0']['dib']['window_size'], 16)
        self.assertEqual(doc['cache']['cpu0_l1i']['sets'], 64)
        self.assertEqual(doc['cache']['cpu0_l1i']['replacement'], 'lru')
        # The one array-valued key in every shipped config.
        self.assertEqual(doc['cache']['cpu0_l1i']['prefetch_activate'], 'LOAD,PREFETCH')


class CxxEmbeddingTest(unittest.TestCase):
    def test_the_record_is_a_specialization_keyed_by_build_id(self):
        lines = list(config.config_record.get_config_record_cxx('abc123', ['[config]', 'block_size = 64']))
        text = '\n'.join(lines)
        self.assertIn('champsim::configured::config_record<0xabc123>', text)
        self.assertIn('std::string_view toml', text)
        self.assertIn('[config]', text)
        self.assertIn('block_size = 64', text)

    def test_the_payload_is_not_indented(self):
        # The literal is emitted at namespace scope precisely so that
        # cxx.brace_wrap's two-space struct-body indent cannot leak into the
        # string and thence into every emitted statistics document.
        lines = list(config.config_record.get_config_record_cxx('abc123', ['[config]', 'block_size = 64']))
        self.assertIn('[config]', lines)
        self.assertIn('block_size = 64', lines)

    def test_a_payload_containing_the_delimiter_is_refused(self):
        # A raw string literal ends at its delimiter. If a config value could
        # contain it the generated C++ would not compile -- or worse, would
        # compile into something else. Fail at configure time instead.
        with self.assertRaises(ValueError):
            list(config.config_record.get_config_record_cxx('abc123', ['x = ")CHAMPSIM_CONFIG"']))


class ModuleNameTest(unittest.TestCase):
    def render(self, elements):
        return '\n'.join(config.config_record.get_config_record_lines('champsim', elements, {}))

    def test_a_module_name_is_recorded_even_when_only_the_internal_key_has_it(self):
        # cpu0_L1I in the shipped config has no 'replacement' key: the policy
        # comes from champsim::defaults::default_l1i, so _replacement_data is
        # the only record of it. Dropping internal keys blindly would lose the
        # single most useful thing in this section.
        elements = {'cores': [], 'ptws': [], 'pmem': {}, 'vmem': {},
                    'caches': [{'name': 'cpu0_L1I',
                                '_replacement_data': [{'path': 'replacement/lru', 'class': 'lru'}]}]}
        doc = tomllib.loads(self.render(elements))
        self.assertEqual(doc['config']['cache']['cpu0_l1i']['replacement'], 'lru')

    def test_the_module_name_is_the_directory_not_the_class(self):
        # The JSON names a directory; a module's class name need not match it.
        elements = {'cores': [{'name': 'cpu0',
                               '_btb_data': [{'path': 'btb/blbp_64kb_tuned', 'class': 'SomeOtherClass'}]}],
                    'caches': [], 'ptws': [], 'pmem': {}, 'vmem': {}}
        doc = tomllib.loads(self.render(elements))
        self.assertEqual(doc['config']['ooo_cpu']['cpu0']['btb'], 'blbp_64kb_tuned')

    def test_several_modules_of_one_kind_are_joined(self):
        elements = {'cores': [], 'ptws': [], 'pmem': {}, 'vmem': {},
                    'caches': [{'name': 'llc',
                                '_prefetcher_data': [{'path': 'prefetcher/next_line'}, {'path': 'prefetcher/ip_stride'}]}]}
        doc = tomllib.loads(self.render(elements))
        self.assertEqual(doc['config']['cache']['llc']['prefetcher'], 'next_line,ip_stride')

    def test_the_internal_key_itself_is_not_emitted(self):
        elements = {'cores': [], 'ptws': [], 'pmem': {}, 'vmem': {},
                    'caches': [{'name': 'llc', '_replacement_data': [{'path': 'replacement/lru'}]}]}
        doc = tomllib.loads(self.render(elements))
        self.assertNotIn('_replacement_data', doc['config']['cache']['llc'])


class KeyFoldingTest(unittest.TestCase):
    def render(self, elements):
        return '\n'.join(config.config_record.get_config_record_lines('champsim', elements, {}))

    def test_keys_are_lowercased_like_the_rest_of_the_document(self):
        elements = {'cores': [{'name': 'cpu0', 'L1I': 'cpu0_L1I', 'DTLB': 'cpu0_DTLB'}],
                    'caches': [], 'ptws': [], 'pmem': {}, 'vmem': {}}
        doc = tomllib.loads(self.render(elements))
        core = doc['config']['ooo_cpu']['cpu0']
        self.assertEqual(core['l1i'], 'cpu0_L1I')
        self.assertEqual(core['dtlb'], 'cpu0_DTLB')
        self.assertNotIn('L1I', core)

    def test_keys_colliding_only_by_case_keep_their_exact_spelling(self):
        elements = {'cores': [{'name': 'cpu0', 'Size': 1, 'size': 2}],
                    'caches': [], 'ptws': [], 'pmem': {}, 'vmem': {}}
        doc = tomllib.loads(self.render(elements))
        core = doc['config']['ooo_cpu']['cpu0']
        self.assertEqual(core['Size'], 1)
        self.assertEqual(core['size'], 2)

    def test_a_scalar_is_never_emitted_after_a_subtable(self):
        # A table header ends the table it appears in, so a key emitted after a
        # sub-table would silently land inside that sub-table.
        elements = {'cores': [{'name': 'cpu0', 'DIB': {'sets': 32}, 'rob_size': 352}],
                    'caches': [], 'ptws': [], 'pmem': {}, 'vmem': {}}
        doc = tomllib.loads(self.render(elements))
        core = doc['config']['ooo_cpu']['cpu0']
        self.assertEqual(core['rob_size'], 352)
        self.assertEqual(core['dib']['sets'], 32)
        self.assertNotIn('rob_size', core['dib'])


class RuntimeKeysTest(unittest.TestCase):
    def test_the_manifest_rides_with_the_config_record(self):
        lines = list(config.config_record.get_config_record_cxx('abc123', ['[config]'], runtime_keys=['b.two', 'a.one']))
        text = '\n'.join(lines)
        self.assertIn('champsim::configured::config_record<0xabc123>', text)
        # Sorted, so the emitted file is stable across dict orderings.
        self.assertIn('"a.one",', text)
        self.assertLess(text.index('"a.one"'), text.index('"b.two"'))
        self.assertIn('runtime_keys', text)

    def test_an_empty_manifest_is_still_declared(self):
        lines = list(config.config_record.get_config_record_cxx('abc123', ['[config]']))
        self.assertIn('runtime_keys', '\n'.join(lines))
