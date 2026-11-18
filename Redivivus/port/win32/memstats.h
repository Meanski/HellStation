// Memory report: where the game's RAM goes, for deciding what fits in 32 MB.
// Counters live in win32/system.cpp (VirtualAlloc regions, Win32 heap blocks);
// the C heap's side comes from the backend (hs_mem_query).
#pragma once

// Logs one "mem" line: current and peak figures. gdi.cpp calls it every 250
// presented frames, next to the backend's frame-pacing line.
void hs_mem_report(unsigned frame);

// One line per Fog pool manager: committed vs in use, and its malloc overflow
// (port/fog_pools.cpp). hs_mem_report adds it every 500 frames.
void hs_mem_report_fog(void);
void hs_mem_report_fog_exact(void);  // overrides/fog_exact_alloc.cpp
void hs_mem_report_storm_exact(void);  // overrides/storm_exact_alloc.cpp
