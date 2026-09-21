'''
ChampSim's configuration step, written in Python, discovers the modules present
on disk and emits the two things a C++ header cannot know for itself: the
registry mapping a module's name to a factory, and the makefile fragment listing
its objects.

It does not configure the simulated machine. That is hand-written C++
(src/static_environment.cc, with its compile-time constants in inc/defs.h), and
every simulation parameter is set at run time from a TOML file. There is no
JSON configuration file and no code generator for one.
'''
