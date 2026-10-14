#pragma once
typedef struct {
    long small_bytes, small_blocks, small_peak, large_bytes;
} sim_mem_stats_t;
extern sim_mem_stats_t g_sim_mem;
