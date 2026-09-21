#!/usr/bin/env python3
"""Compatibility entry point; export one policy via --policy and --objects.

Use make compile_commands to resolve the selected mode, flavor and source list.
"""
from selected import main

if __name__ == '__main__':
    main()
