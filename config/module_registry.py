'''
Generate the runtime module registry: per build id, a name -> factory mapping
over every compiled module, producing the type-erased pimpls that
O3_CPU/CACHE already dispatch through. This is what lets a runtime key
(--set ooo_cpu.cpu0.branch_predictor=...) select a module without rebuilding.

Runtime module names are directory basenames, which equal the class names for
every non-legacy module (config.modules derives the class from the directory).
Legacy modules go through generated free-function shims rather than the
class-based model the registry instantiates, so they are not registered.
'''

import itertools
import os

# (module_info key, runtime key suffix / user-facing kind name, factory name,
#  concept type, model template, owner type)
KINDS = (
    ('branch', 'branch_predictor', 'make_branch', 'O3_CPU::branch_module_concept', 'O3_CPU::branch_module_model', 'O3_CPU'),
    ('btb', 'btb', 'make_btb', 'O3_CPU::btb_module_concept', 'O3_CPU::btb_module_model', 'O3_CPU'),
    ('pref', 'prefetcher', 'make_prefetcher', 'CACHE::prefetcher_module_concept', 'CACHE::prefetcher_module_model', 'CACHE'),
    ('repl', 'replacement', 'make_replacement', 'CACHE::replacement_module_concept', 'CACHE::replacement_module_model', 'CACHE'),
)

# factory name -> the pimpl member its product replaces
PIMPL_MEMBERS = {
    'make_branch': 'branch_module_pimpl',
    'make_btb': 'btb_module_pimpl',
    'make_prefetcher': 'pref_module_pimpl',
    'make_replacement': 'repl_module_pimpl',
}

def module_include_files(datas):
    """
    Every header contributing to the given modules, sorted.

    Sorted deliberately: the original walked a set, so two identical configure
    runs emitted these includes in different orders and forced a needless
    rebuild of the translation unit that includes every module.
    """
    def all_headers_on(path):
        for base, _, files in os.walk(path):
            for file in files:
                if os.path.splitext(file)[1] == '.h':
                    yield os.path.abspath(os.path.join(base, file))

    candidates = {header for data in datas for header in all_headers_on(data['path'])}
    yield from (f'#include "{header}"' for header in sorted(candidates))

def registered(module_info, kind):
    ''' The registerable modules of one kind, sorted by name for stable output. '''
    return sorted((v for v in module_info.get(kind, {}).values() if not v.get('legacy', False)), key=lambda v: v['class'])

def registry_class_lines(module_info):
    '''
    The module_registry definition for the generated header: per-kind
    constexpr name arrays (used by --knobs and by main's validation of
    module-knob table prefixes) and the four factory declarations (defined in
    the generated registry TU).
    '''
    # Standalone header: it declares only the registry, so it carries its own
    # includes, and its own guard so that the definitions can include it
    # without depending on include order.
    yield '#ifndef CHAMPSIM_GENERATED_REGISTRY_INC'
    yield '#define CHAMPSIM_GENERATED_REGISTRY_INC'
    yield '#include <array>'
    yield '#include <memory>'
    yield '#include <string_view>'
    yield '#include "cache.h"'
    yield '#include "environment.h"'
    yield '#include "ooo_cpu.h"'
    yield 'struct champsim::configured::module_registry {'
    for kind, keyname, factory, concept, _, owner in KINDS:
        names = [v['class'] for v in registered(module_info, kind)]
        joined = ', '.join(f'"{n}"' for n in names)
        yield f'static constexpr std::array<std::string_view, {len(names)}> {keyname}{{{{{joined}}}}};'
    for kind, keyname, factory, concept, _, owner in KINDS:
        yield f'static std::unique_ptr<{concept}> {factory}(std::string_view name, {owner}* owner);'
    yield '};'
    yield '#endif'

def registry_impl_lines(module_info):
    '''
    The factory definitions for registry.cc.inc: include every registered
    module's headers, then one name -> make_unique chain per kind. An unknown
    name throws listing the valid names; a comma is rejected as multi-module
    (runtime selection is single-module; prefetcher lists are the planned
    extension).
    '''
    # The declaration, guarded, so include order cannot matter -- clang-format
    # sorts includes within a block, which put these definitions before the
    # declaration once already.
    yield '#include "registry.inc"'
    yield '#include <memory>'
    yield '#include <stdexcept>'
    yield '#include <string>'
    yield ''
    all_datas = list(itertools.chain.from_iterable(registered(module_info, kind) for kind, *_ in KINDS))
    yield from module_include_files(all_datas)
    yield ''

    for kind, keyname, factory, concept, model, owner in KINDS:
        mods = registered(module_info, kind)
        names = ', '.join(v['class'] for v in mods)
        yield f'std::unique_ptr<{concept}> champsim::configured::module_registry::{factory}(std::string_view name, {owner}* owner)'
        yield '{'
        yield "  if (name.find(',') != std::string_view::npos) {"
        yield f'    throw std::runtime_error("runtime module selection takes one module (got \'" + std::string{{name}} + "\'); compose multiple modules at config.sh time");'
        yield '  }'
        for mod in mods:
            yield f'  if (name == "{mod["class"]}") {{'
            yield f'    return std::make_unique<{model}<class {mod["class"]}>>(owner);'
            yield '  }'
        yield f'  throw std::runtime_error("unknown {keyname} module \'" + std::string{{name}} + "\'; valid {keyname} modules: {names}");'
        yield '}'
        yield ''

def module_selection_lines(member, index, cfg_prefix, entries):
    '''
    Constructor-body statements for one component: per module kind, look up the
    selection key (default: the baked pack, comma-joined), install the
    registry's product when it differs, and deliver configure() to the selected
    module with its knob table as the prefix. A composed pack gets no
    configure() call -- a prefix shared across the pack would collide knobs --
    and a legacy pack is left entirely alone.
    '''
    for key_suffix, install, factory, datas in entries:
        if any(d.get('legacy', False) for d in datas):
            continue
        baked = ','.join(d['class'] for d in datas)
        yield '{'
        yield f'const auto sel = cfg.value<std::string>("{cfg_prefix}.{key_suffix}", "{baked}");'
        if len(datas) == 1:
            # Single-module pack: configure the selected module -- baked or
            # swapped -- with its knob table as the prefix.
            yield f'if (sel != "{baked}") {{'
            yield f'  {member}.at({index}).{install}(champsim::configured::module_registry::{factory}(sel, &{member}.at({index})));'
            yield '}'
            yield f'{member}.at({index}).{PIMPL_MEMBERS[factory]}->impl_configure(cfg, std::string{{"{cfg_prefix}."}} + sel);'
        else:
            # Composed baked pack: no knob table when it stands (a shared
            # prefix would collide knobs), but a runtime override installs a
            # SINGLE module, which then gets its table like any other.
            yield f'if (sel != "{baked}") {{'
            yield f'  {member}.at({index}).{install}(champsim::configured::module_registry::{factory}(sel, &{member}.at({index})));'
            yield f'  {member}.at({index}).{PIMPL_MEMBERS[factory]}->impl_configure(cfg, std::string{{"{cfg_prefix}."}} + sel);'
            yield '}'
        yield '}'
