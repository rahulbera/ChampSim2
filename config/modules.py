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

import os
import itertools

def get_module_name(path, start=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))):
    ''' Create a mangled module name from the path to its sources '''
    fname_translation_table = str.maketrans('./-','_DH')
    return os.path.relpath(path, start=start).translate(fname_translation_table)

class ModuleSearchContext:
    def __init__(self, paths, verbose=False):
        self.paths = [p for p in paths if os.path.exists(p) and os.path.isdir(p)]
        self.verbose = verbose

    def data_from_path(self, path):
        name = get_module_name(path)
        is_legacy = ('__legacy__' in [*itertools.chain(*(f for _,_,f in os.walk(path)))])
        retval = {
            'name': name,
            'path': path,
            'legacy': is_legacy,
            'class': 'champsim::modules::generated::'+name if is_legacy else os.path.basename(path)
        }

        if self.verbose:
            print('M:', retval)
        return retval

    # Try the context's module directories, then try to interpret as a path
    def find(self, module):
        # Return a normalized directory: variables and user shorthands are expanded
        paths = itertools.chain(
            (os.path.join(dirname, module) for dirname in self.paths), # Prepend search paths
            (module,) # Interpret as file path
        )

        paths = map(os.path.expandvars, paths)
        paths = map(os.path.expanduser, paths)
        paths = filter(os.path.exists, paths)
        path = os.path.relpath(next(paths, None))

        return self.data_from_path(path)

    def find_all(self):
        base_dirs = [next(os.walk(p)) for p in self.paths]
        files = itertools.starmap(os.path.join, itertools.chain(*(zip(itertools.repeat(b), d) for b,d,_ in base_dirs)))
        return [self.data_from_path(f) for f in files]


# The four module kinds and the directory each lives in, relative to the repo.
KIND_DIRS = (('branch', 'branch'), ('btb', 'btb'), ('pref', 'prefetcher'), ('repl', 'replacement'))

def get_module_info(module_dir=None, branch_dir=None, btb_dir=None, pref_dir=None, repl_dir=None, verbose=False):
    """
    Discover every module on disk, by kind.

    This is the one thing a header cannot know and a script must therefore
    generate: modules are added by creating a directory.
    """
    champsim_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    extra = {'branch': branch_dir or [], 'btb': btb_dir or [], 'pref': pref_dir or [], 'repl': repl_dir or []}
    shared = list(module_dir or [])

    info = {}
    for kind, dirname in KIND_DIRS:
        paths = [os.path.join(champsim_root, dirname)]
        paths.extend(os.path.join(d, dirname) for d in shared)
        paths.extend(extra[kind])
        found = ModuleSearchContext(paths, verbose=verbose).find_all()
        info[kind] = {mod['name']: mod for mod in found}
    return info
