'''
Render the parsed configuration as a TOML fragment, for embedding into the
generated binary so that a statistics document can say which machine produced
it.

The output must obey the statistics document's own rules, which are documented
in CLAUDE.md and implemented in src/toml_printer.cc:

  * lower_snake_case keys, and component names lower-cased;
  * NO ARRAYS -- every key holds a single scalar;
  * a key that is not a bare TOML key is quoted, never rewritten, so two names
    can never collide onto one table.

The quoting here mirrors ::quote() and ::is_bare_key() in src/toml_printer.cc.
Those two halves of one document must escape identically, so a change to either
belongs in both.

This module is imported by config.sh, which runs on bare cluster login nodes.
It must therefore depend on nothing outside the standard library, and in
particular must not import a TOML writer -- the standard library has none
(tomllib is read-only). The emitter below is the reason.
'''

import itertools

# Delimiter for the C++ raw string literal that carries the record. Chosen to be
# something no configuration value can plausibly contain; get_config_record_cxx
# refuses a payload that contains it rather than emitting broken C++.
CXX_RAW_DELIMITER = 'CHAMPSIM_CONFIG'

# Keys the configuration layer uses for its own bookkeeping. They are not
# machine parameters: _offset_bits is a C++ expression ('champsim::lg2(64)') and
# _defaults names a C++ object ('champsim::defaults::default_llc').
def is_internal_key(key):
    return str(key).startswith('_')

# The _*_data keys are the exception: they are the AUTHORITATIVE record of which
# modules a component uses, and they are populated even when the corresponding
# plain key is absent because the module came from a C++ default. On the shipped
# config, for instance, cpu0_L1I has no 'replacement' key at all -- only
# _replacement_data says it is lru. So the plain key is derived from these
# rather than read directly, and the internal key itself is then dropped.
MODULE_DATA_KEYS = {
    '_branch_predictor_data': 'branch_predictor',
    '_btb_data': 'btb',
    '_prefetcher_data': 'prefetcher',
    '_replacement_data': 'replacement',
}

def module_basename(path):
    ''' The directory a module lives in, which is the name the JSON refers to. '''
    return str(path).replace('\\', '/').rstrip('/').rsplit('/', 1)[-1]

def with_module_names(component):
    '''
    Return the component with its module names filled in from the _*_data lists.

    A component may name more than one module of a kind, so the value follows
    the same rule as any other sequence: comma-joined into a single scalar.
    '''
    derived = {}
    for internal, plain in MODULE_DATA_KEYS.items():
        data = component.get(internal)
        if data:
            derived[plain] = ','.join(module_basename(entry['path']) for entry in data)
    return {**component, **derived} if derived else component

_SIMPLE_ESCAPES = {
    '"': '\\"',
    '\\': '\\\\',
    '\b': '\\b',
    '\f': '\\f',
    '\n': '\\n',
    '\r': '\\r',
    '\t': '\\t',
}

def toml_string(value):
    ''' Render a str as a TOML basic string, escaping exactly as ::quote() does. '''
    out = ['"']
    for char in str(value):
        if char in _SIMPLE_ESCAPES:
            out.append(_SIMPLE_ESCAPES[char])
        elif ord(char) < 0x20 or ord(char) == 0x7f:
            out.append(f'\\u{ord(char):04X}')
        else:
            out.append(char)
    out.append('"')
    return ''.join(out)

def is_bare_key(key):
    ''' A TOML bare key: one or more of [A-Za-z0-9_-]. '''
    key = str(key)
    return bool(key) and all(c.isascii() and (c.isalnum() or c in '_-') for c in key)

def toml_key(key):
    ''' A bare key if it can be one, otherwise a quoted key. Never rewritten. '''
    return str(key) if is_bare_key(key) else toml_string(key)

def toml_value(value):
    '''
    Render a configuration value as a TOML value.

    bool is tested before int deliberately: in Python bool subclasses int, so
    the reverse order would render True as 1 and change the type a parser sees.

    A list becomes a comma-joined string rather than a TOML array. The document
    forbids arrays, and the alternative -- a sub-table of flags -- would make
    the key set depend on the values, which is the thing the rule exists to
    prevent. In practice the only array-valued key is prefetch_activate.
    '''
    if isinstance(value, bool):
        return 'true' if value else 'false'
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, (list, tuple)):
        return toml_string(','.join(str(v) for v in value))
    return toml_string(value)

def folded_keys(names):
    '''
    Lower-case a series of names for use as TOML keys, which is what the rest of
    the document does ('lower_snake_case keys throughout', including lower-cased
    component names).

    Lower-casing is not injective. Two names differing only by case would emit
    the same key or the same table header twice -- and a duplicate table is a
    hard TOML parse error that would make the WHOLE document unreadable, not
    just this section. Where that would happen the exact spelling is used
    instead, so the result is always unique. This mirrors ::component_keys() in
    src/toml_printer.cc.
    '''
    names = list(names)
    folded = [str(n).lower() for n in names]
    return [toml_key(name if folded.count(fold) > 1 else fold) for name, fold in zip(names, folded)]

# Component table keys follow the same rule as any other key, and must, so that
# [config.cache.cpu0_l1d] sits at the same key as
# [phase.<n>.roi.cache.cpu0_l1d] and the two can be joined.
component_keys = folded_keys

def _table(path, entries):
    ''' Emit one table, then any nested tables its dict-valued keys imply. '''
    entries = with_module_names(entries)
    visible = [(k, v) for k, v in entries.items() if not is_internal_key(k)]
    keys = folded_keys(k for k, _ in visible)

    yield ''
    yield f'[{path}]'
    # Scalars first: a table header ends the table it appears in, so a key
    # emitted after a sub-table would silently land inside that sub-table.
    for key, (_, value) in zip(keys, visible):
        if not isinstance(value, dict):
            yield f'{key} = {toml_value(value)}'
    for key, (_, value) in zip(keys, visible):
        if isinstance(value, dict):
            yield from _table(f'{path}.{key}', value)

def _named_tables(path, components):
    components = list(components)
    for key, component in zip(component_keys(c['name'] for c in components), components):
        yield from _table(f'{path}.{key}', component)

def get_config_record_lines(executable_name, elements, config_file):
    '''
    Yield the [config] section describing one parsed configuration.

    :param executable_name: the binary this configuration builds
    :param elements: the 'elements' member of a parsed config (cores, caches, ptws, pmem, vmem)
    :param config_file: the global keys (block_size, page_size, ...)
    '''
    yield '[config]'
    yield f'executable_name = {toml_string(executable_name)}'
    globals_ = [(k, v) for k, v in config_file.items() if not is_internal_key(k)]
    for key, (_, value) in zip(folded_keys(k for k, _ in globals_), globals_):
        yield f'{key} = {toml_value(value)}'

    yield from _named_tables('config.ooo_cpu', elements.get('cores') or [])
    yield from _named_tables('config.cache', elements.get('caches') or [])
    yield from _named_tables('config.ptw', elements.get('ptws') or [])
    if elements.get('pmem'):
        yield from _table('config.pmem', elements['pmem'])
    if elements.get('vmem'):
        yield from _table('config.vmem', elements['vmem'])

def get_config_record_cxx(build_id, record_lines, runtime_keys=()):
    '''
    Yield the champsim::configured::config_record specialization carrying a
    rendered [config] section and the runtime-key manifest, for the given
    build id.

    The specialization is emitted at namespace scope rather than as a member of
    generated_environment, because cxx.brace_wrap indents every struct-body line
    by two spaces -- and inside a raw string literal that indentation would
    become part of the emitted document.

    :raises ValueError: if the payload contains the raw-string delimiter, which
                        would end the literal early and emit C++ that either
                        fails to compile or means something else.
    '''
    record_lines = list(record_lines)
    terminator = f'){CXX_RAW_DELIMITER}"'
    if any(terminator in line for line in record_lines):
        raise ValueError(f'configuration value contains the raw string delimiter {terminator!r}')

    runtime_keys = sorted(runtime_keys)
    yield '#include <array>'
    yield 'template <>'
    yield f'struct champsim::configured::config_record<0x{build_id}> {{'
    yield f'static constexpr std::string_view toml = R"{CXX_RAW_DELIMITER}('
    yield from record_lines
    yield f'){CXX_RAW_DELIMITER}";'
    # Every runtime-config key the generated constructor consults, for startup
    # validation of --config/--set and for --knobs.
    yield f'static constexpr std::array<std::string_view, {len(runtime_keys)}> runtime_keys{{{{'
    for key in runtime_keys:
        yield f'"{key}",'
    yield '}};'
    yield '};'
