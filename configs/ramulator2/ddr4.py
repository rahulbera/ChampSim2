"""Pinned native revision: 72427a1bba3771564c4fb0e494ba02242fd1eaa7.

Single-channel DDR4 external memory configuration for ChampSim validation.

Export using the pinned Ramulator 2.1 Python source package:
PYTHONPATH=/path/to/ramulator2/python python3 -m ramulator export ddr4.py -o ddr4.yaml
"""
import ramulator

frontend = ramulator.frontend.External(clock_ratio=1)
dram = ramulator.dram.DDR4(
    org_preset="DDR4_8Gb_x8", timing_preset="DDR4_2400R", rank=1
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
