"""Pinned native revision: 72427a1bba3771564c4fb0e494ba02242fd1eaa7.

Single-channel LPDDR5 external memory configuration for ChampSim validation.

Export using the pinned Ramulator 2.1 Python source package:
PYTHONPATH=/path/to/ramulator2/python python3 -m ramulator export lpddr5.py -o lpddr5.yaml
"""
import ramulator

frontend = ramulator.frontend.External(clock_ratio=1)
dram = ramulator.dram.LPDDR5(
    org_preset="LPDDR5_8Gb_x16", timing_preset="LPDDR5_5500", rank=1
)
controller = ramulator.controller.LPDDR5(
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
