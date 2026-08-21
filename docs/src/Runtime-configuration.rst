======================
Runtime configuration
======================

ChampSim is configured at run time from a TOML file. There is no configuration
script to run and no generated machine: ``config.sh`` discovers the modules
present on disk and nothing else.

.. code-block:: console

    $ ./config.sh
    $ make
    $ bin/champsim --config configs/sample.toml trace.champsimtrace.xz

Sources and precedence
======================

``--config <file>`` and ``--set key=value`` may both be repeated, and are
applied strictly in command-line order. The last definition of a key wins,
whichever source it came from:

.. code-block:: console

    $ bin/champsim --config base.toml --set ooo_cpu.cpu0.rob_size=512 \
                   --config override.toml trace.champsimtrace.xz

Keys
====

Keys name a component and a parameter: ``ooo_cpu.cpu0.rob_size``,
``cache.cpu0_l1d.sets``, ``ptw.cpu0_ptw.mshr_size``, ``pmem.tcas``,
``vmem.num_levels``, ``sim.deadlock_cycle``. Component names are lower-cased.

Four keys select modules -- ``ooo_cpu.<cpu>.branch_predictor``,
``ooo_cpu.<cpu>.btb``, ``cache.<name>.prefetcher``,
``cache.<name>.replacement`` -- naming any module compiled into the binary by
its directory name. One module per key.

A module may accept its own knobs from a table named after it:

.. code-block:: toml

    [ooo_cpu.cpu0]
    btb = "basic_btb"

    [ooo_cpu.cpu0.basic_btb]
    sets = 2048
    ways = 8

``bin/champsim --knobs`` lists every key a binary accepts, with the value the
current invocation would use, and the modules selectable per kind. Its output
is a valid TOML document -- the module list is commented -- so it doubles as a
starting configuration file:

.. code-block:: console

    $ bin/champsim --knobs > my.toml

A key that nothing consumes is a fatal error at startup, not a silent no-op.

What is not runtime-configurable
================================

The machine's shape -- which components exist and how they are wired -- is
C++ in ``src/static_environment.cc``. ``NUM_CPUS``, ``BLOCK_SIZE`` and
``PAGE_SIZE`` are in ``inc/defs.h``. Changing either is a code edit and a
rebuild; multi-core is a separate binary by design.

Provenance
==========

A run's statistics document records what produced it: ``[config]`` is the
effective configuration -- every key consulted, with the value used.
``[config_override]`` records what the configuration sources set,
``[meta].config_files`` the files loaded in order, and ``[meta].build_id`` a
content hash identifying the machine.

That makes a statistics document a configuration source in its own right.
Pointing ``--config`` at a previous run's ``--toml`` output reproduces that
run's machine:

.. code-block:: console

    $ bin/champsim --toml run.toml trace.champsimtrace.xz
    $ bin/champsim --config run.toml --toml repro.toml trace.champsimtrace.xz

The loader recognises the document by ``[meta].schema_version`` and reads its
``[config]`` table, ignoring the results; an ordinary configuration file has no
such key and is read whole. This only reproduces what is *configurable* --
``[meta].build_id`` still has to match, or the two binaries are different
machines.
