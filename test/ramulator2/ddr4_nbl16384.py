"""Pinned native revision: 72427a1bba3771564c4fb0e494ba02242fd1eaa7.

The DDR4 fixture (configs/ramulator2/ddr4.py) with nBL raised to 16384: a
read of a closed row takes nRCD + nCL + nBL = 16416 native cycles, 13.7 us at
833 ps, longer than the 10 us no-progress allowance ChampSim grants native mode
by default. test_ramulator2_cli.py uses it to show that such a valid pause
needs an explicit sim.deadlock_cycle, and that a sufficient one changes no
result. The burst length is not a JEDEC timing.

Export using the pinned Ramulator 2.1 Python source package:
PYTHONPATH=/path/to/ramulator2/python python3 -m ramulator export ddr4_nbl16384.py -o ddr4_nbl16384.yaml
"""
import ramulator

frontend = ramulator.frontend.External(clock_ratio=1)
dram = ramulator.dram.DDR4(
    org_preset="DDR4_8Gb_x8", timing_preset="DDR4_2400R", rank=1, nBL=16384
)
controller = ramulator.controller.GenericDDR(
    dram=dram,
    scheduler=ramulator.scheduler.FRFCFS(),
    refresh_manager=ramulator.refresh_manager.AllBank(),
    row_policy=ramulator.row_policy.Open(),
    addr_mapper=ramulator.addr_mapper.RoBaRaCoCh(),
)
memory = ramulator.memory_system.GenericDRAM(
    clock_ratio=1,
    controllers=[controller],
    channel_mapper=ramulator.channel_mapper.CacheLineInterleave(),
)
sim = ramulator.Simulation(frontend, memory)
