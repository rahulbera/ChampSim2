import unittest
import operator
import itertools
import tempfile
import subprocess
import os

import config.util

class ChainTests(unittest.TestCase):

    def test_chain_flat(self):
        a = {'a': 1}
        b = {'b': 2}
        self.assertEqual(config.util.chain(a,b), {'a': 1, 'b': 2});
        self.assertEqual(config.util.chain(b,a), {'a': 1, 'b': 2});

    def test_chain_overwrite(self):
        a = {'a': 1}
        b = {'a': 2}
        self.assertEqual(config.util.chain(a,b), {'a': 1});
        self.assertEqual(config.util.chain(b,a), {'a': 2});

    def test_chain_lists(self):
        a = {'a': [1,2], 'b': 'test'}
        b = {'a': [3,4]}
        self.assertEqual(config.util.chain(a,b), {'a': [1,2,3,4], 'b': 'test'});
        self.assertEqual(config.util.chain(b,a), {'a': [3,4,1,2], 'b': 'test'});

    def test_chain_dicts(self):
        a = {'a': {'a.a': 2}, 'b': 'test'}
        b = {'a': {'a.a': 3}}
        self.assertEqual(config.util.chain(a,b), {'a': {'a.a': 2}, 'b': 'test'});
        self.assertEqual(config.util.chain(b,a), {'a': {'a.a': 3}, 'b': 'test'});

    def test_modified_result_leaves_priors_unmodified(self):
        a = {'a': 1}
        b = {'a': 2}
        c = config.util.chain(a,b)
        c.update(a=5000)
        self.assertEqual(c, {'a': 5000})
        self.assertEqual(a, {'a': 1})
        self.assertEqual(b, {'a': 2})
class CutTests(unittest.TestCase):
    def test_empty_gives_two_empty(self):
        for cutpoint in (1,-1):
            with self.subTest(n=cutpoint):
                testval = []
                head, tail = config.util.cut(testval, n=cutpoint)
                self.assertEqual(list(head), [])
                self.assertEqual(list(tail), [])

    def test_length_one_can_go_to_head(self):
        testval = ['teststring']
        head, tail = config.util.cut(testval, n=1)
        self.assertEqual(list(head), testval)
        self.assertEqual(list(tail), [])

    def test_length_one_can_go_to_tail(self):
        testval = ['teststring']
        head, tail = config.util.cut(testval, n=-1)
        self.assertEqual(list(head), [])
        self.assertEqual(list(tail), testval)

    def test_positive_count_that_exceeds_sends_all_to_head(self):
        testval = ['teststring']
        head, tail = config.util.cut(testval, n=2)
        self.assertEqual(list(head), testval)
        self.assertEqual(list(tail), [])

    def test_negative_count_that_exceeds_sends_all_to_tail(self):
        testval = ['teststring']
        head, tail = config.util.cut(testval, n=-2)
        self.assertEqual(list(head), [])
        self.assertEqual(list(tail), testval)

    def test_middle_splits(self):
        testval = ['teststringa', 'teststringb', 'teststringc']
        for cutpoint in (1,2,-1,-2):
            with self.subTest(n=cutpoint):
                head, tail = config.util.cut(testval, n=cutpoint)
                self.assertEqual(list(head), testval[:cutpoint])
                self.assertEqual(list(tail), testval[cutpoint:])
class AppendExceptLastTests(unittest.TestCase):
    def test_empty_does_not_append(self):
        testval = []
        result = list(config.util.append_except_last(testval, 'a'))
        self.assertEqual(result, testval)

    def test_length_one_does_not_append(self):
        testval = ['teststring']
        result = list(config.util.append_except_last(testval, 'a'))
        self.assertEqual(result, testval)

    def test_longer_length_appends(self):
        for length in (2,4,8,16):
            testval = ['teststring'] * length
            result = config.util.append_except_last(testval, 'a')
            expected = ['teststringa'] * (length-1) + ['teststring']
            for i,elem_pair in enumerate(itertools.zip_longest(result, expected, fillvalue=None)):
                with self.subTest(iterable_length=length, element_index=i):
                    self.assertEqual(*elem_pair)
class MultilineTests(unittest.TestCase):
    def test_empty_does_nothing(self):
        self.assertEqual(list(config.util.multiline([])), [])

    def test_shorter_than_length_yields_one_line(self):
        testval = ['test']
        self.assertEqual(list(config.util.multiline(testval, length=2)), ['test'])

    def test_longer_than_length_groups_elements(self):
        testval = ['testa', 'testb', 'testc']
        self.assertEqual(list(config.util.multiline(testval, length=2)), ['testa testb', 'testc'])

    def test_lines_can_indent(self):
        testval = ['testa', 'testb', 'testc']
        self.assertEqual(list(config.util.multiline(testval, length=2, indent=1)), ['testa testb', '  testc'])

    def test_line_terminators_are_added(self):
        testval = ['testa', 'testb', 'testc']
        self.assertEqual(list(config.util.multiline(testval, length=2, line_end='x')), ['testa testbx', 'testc'])
class BatchTests(unittest.TestCase):
    def test_empty(self):
        given = tuple()
        expected = tuple()
        evaluated = tuple(config.util.batch(given, 2))
        self.assertEqual(expected, evaluated)

    def test_single_batch(self):
        given = ('cat', 'dog')
        expected = (('cat', 'dog'),)
        evaluated = tuple(config.util.batch(given, 2))
        self.assertEqual(expected, evaluated)

    def test_multiple_batch(self):
        given = ('cat', 'dog', 'pig', 'cow')
        expected = (('cat', 'dog'), ('pig', 'cow'))
        evaluated = tuple(config.util.batch(given, 2))
        self.assertEqual(expected, evaluated)

    def test_nonuniform(self):
        given = ('cat', 'dog', 'pig')
        expected = (('cat', 'dog'), ('pig',))
        evaluated = tuple(config.util.batch(given, 2))
        self.assertEqual(expected, evaluated)
