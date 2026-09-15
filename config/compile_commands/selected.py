#!/usr/bin/env python3
"""Export the exact argv used by the selected Make policy and flavor."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build_config import write_changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--policy', type=Path, required=True)
    parser.add_argument('--objects', nargs='+', required=True)
    args = parser.parse_args()
    policy = json.loads(args.policy.read_text())
    root = args.policy.parent
    # The stamp retains literal driver argv (including compiler response-file
    # references); policy.compiler.command records their effective expansion.
    command = json.loads((root / 'compiler.stamp').read_text())['command']
    result = []
    for output in args.objects:
        relative = Path(output).relative_to(root)
        options = [f'@{root}/policy.options', f'@{root}/absolute.options']
        if relative.parts[0] == 'modules':
            source = Path(str(relative)[len('modules/'):].replace('externUPdir', '..', 1).replace('_UPdir', '/..')).with_suffix('.cc')
            options += [f'@{root}/module-policy.options']
        elif relative.parts[0] == 'test':
            source = Path('test/cpp/src') / str(relative)[len('test/'):].replace('TEST_000-test-main.o', '000-test-main.o')
            source = source.with_suffix('.cc')
        else:
            source = Path('src') / relative.with_suffix('.cc')
            if relative.name in ('SIM_main.o', 'TEST_main.o'):
                source = Path('src/main.cc')
        if source == Path('src/ramulator2_driver.cc') and policy['native']['enabled']:
            options += ['-isystem', policy['native']['root'] + '/src', '-std=c++20']
        result.append({'directory': str(Path.cwd()), 'file': str(source), 'output': output,
                       'arguments': command + options + ['-c', '-o', output, str(source)]})
    write_changed(root / 'compile_commands.json', json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
