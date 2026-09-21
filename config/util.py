#    Copyright 2023 The ChampSim Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import itertools
import functools
import operator
import collections

def collect(iterable, key_func, join_func):
    ''' Perform the "sort->groupby" idiom on an iterable, grouping according to the join_func. '''
    intern_iterable = sorted(iterable, key=key_func)
    intern_iterable = itertools.groupby(intern_iterable, key=key_func)
    return (join_func(it[1]) for it in intern_iterable)
def chain(*dicts):
    '''
    Combine two or more dictionaries.
    Values that are dictionaries are merged recursively.
    Values that are lists are joined.

    Dictionaries given earlier in the parameter list have priority.

    >>> chain({ 'a': 1 }, { 'b': 2 })
    { 'a': 1, 'b': 2 }
    >>> chain({ 'a': 1 }, { 'a': 2 })
    { 'a': 1 }
    >>> chain({ 'd': { 'a': 1 } }, { 'd': { 'b': 2 } })
    { 'd': { 'a': 1, 'b': 2 } }

    :param dicts: the sequence to be chained
    '''
    def merge(merger, tname, lhs, rhs):
        return {k:merger(v, rhs[k]) for k,v in lhs.items() if isinstance(v, tname) and isinstance(rhs.get(k), tname)}

    def merge_dicts(lhs,rhs):
        dict_merges = merge(merge_dicts, dict, lhs, rhs)
        list_merges = merge(operator.concat, list, lhs, rhs)
        return dict(itertools.chain(rhs.items(), lhs.items(), dict_merges.items(), list_merges.items()))

    return functools.reduce(merge_dicts, dicts, {})
def cut(iterable, n=-1):
    '''
    Split an iterable into a head and a tail. The head should be completely consumed before the tail is accesssed.

    :param iterable: An iterable
    :param n: The length of the head or, if the value is negative, the length of the tail.
    '''
    it = iter(iterable)
    if n >= 0:
        return itertools.islice(it, n), it

    tail = collections.deque(itertools.islice(it, -1*n))
    def head_iterator():
        for elem in it:
            yield tail.popleft()
            tail.append(elem)
    def tail_iterator():
        yield from tail

    return head_iterator(), tail_iterator()
def append_except_last(iterable, suffix):
    ''' Concatenate a suffix to each element of the iterable except the last one. '''
    head, tail = cut(iterable, n=-1)
    yield from map(operator.concat, head, itertools.repeat(suffix))
    yield from tail
def batch(it, n):
    ''' A backport of itertools.batch(). '''
    it = iter(it)
    val = tuple(itertools.islice(it, n))
    while val:
        yield val
        val = tuple(itertools.islice(it, n))
def multiline(long_line, length=1, indent=0, line_end=None):
    ''' Split a long string into lines with n words '''
    grouped = map(' '.join, batch(long_line, length))
    lines = append_except_last(grouped, line_end or '')
    indentation = itertools.chain(('',), itertools.repeat('  '*indent))
    yield from (i+l for i,l in zip(indentation,lines))
