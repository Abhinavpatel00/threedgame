#pragma once
#include "tinytypes.h"
#include <stdint.h>
#include <stdbool.h>

#ifndef GPU_PROF_MAX_SCOPES
#define GPU_PROF_MAX_SCOPES 128
#endif

#ifndef GPU_PROF_NAME_MAX
#define GPU_PROF_NAME_MAX 32
#endif


typedef struct
{
    const char* name;

    uint32_t start_query;
    uint32_t end_query;

    uint32_t stats_query;

    double time_ms;

    uint64_t vs_invocations;
    uint64_t fs_invocations;
    uint64_t primitives;
} GpuPass;


typedef struct
{
    VkQueryPool timestamp_pool;
    VkQueryPool stats_pool;

    uint32_t query_count;
    uint32_t pass_count;

    float timestamp_period;

    bool enable_pipeline_stats;

    GpuPass passes[GPU_PROF_MAX_SCOPES];

} GpuProfiler;

#define MAX_GPU_PASSES GPU_PROF_MAX_SCOPES
FORCE_INLINE void gpu_profiler_init(GpuProfiler* p, VkDevice device, float timestamp_period, bool enable_pipeline_stats)
{
    p->enable_pipeline_stats = enable_pipeline_stats;
    p->timestamp_pool        = VK_NULL_HANDLE;
    p->stats_pool            = VK_NULL_HANDLE;
    p->query_count           = 0;
    p->pass_count            = 0;

    VkQueryPoolCreateInfo time_info = {.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType  = VK_QUERY_TYPE_TIMESTAMP,
                                       .queryCount = MAX_GPU_PASSES * 2};

    vkCreateQueryPool(device, &time_info, NULL, &p->timestamp_pool);

    if(enable_pipeline_stats)
    {
        VkQueryPoolCreateInfo stats_info = {.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                            .queryType  = VK_QUERY_TYPE_PIPELINE_STATISTICS,
                                            .queryCount = MAX_GPU_PASSES,
                                            .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT
                                                                  | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT
                                                                  | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT};

        VkResult stats_create_res = vkCreateQueryPool(device, &stats_info, NULL, &p->stats_pool);
        if(stats_create_res != VK_SUCCESS)
        {
            p->enable_pipeline_stats = false;
            p->stats_pool            = VK_NULL_HANDLE;
        }
    }

    p->timestamp_period = timestamp_period;
}

FORCE_INLINE void gpu_profiler_destroy(GpuProfiler* p, VkDevice device)
{
    if(p->timestamp_pool)
    {
        vkDestroyQueryPool(device, p->timestamp_pool, NULL);
        p->timestamp_pool = VK_NULL_HANDLE;
    }

    if(p->stats_pool)
    {
        vkDestroyQueryPool(device, p->stats_pool, NULL);
        p->stats_pool = VK_NULL_HANDLE;
    }

    p->query_count = 0;
    p->pass_count  = 0;
}

FORCE_INLINE void gpu_profiler_begin_frame(GpuProfiler* p, VkCommandBuffer cmd)
{
    if(!p || cmd == VK_NULL_HANDLE)
        return;

    p->query_count = 0;
    p->pass_count  = 0;

    vkCmdResetQueryPool(cmd, p->timestamp_pool, 0, MAX_GPU_PASSES * 2);

    if(p->enable_pipeline_stats && p->stats_pool != VK_NULL_HANDLE)
        vkCmdResetQueryPool(cmd, p->stats_pool, 0, MAX_GPU_PASSES);
}

FORCE_INLINE void gpu_profiler_begin_pass(GpuProfiler* p, VkCommandBuffer cmd, const char* name, VkPipelineStageFlagBits2 stage)
{


    uint32_t q = p->query_count++;

    GpuPass* pass = &p->passes[p->pass_count];

    pass->name        = name;
    pass->start_query = q;
    pass->stats_query = p->pass_count;

    vkCmdWriteTimestamp2(cmd, stage, p->timestamp_pool, q);

    if(p->enable_pipeline_stats && p->stats_pool != VK_NULL_HANDLE)
    {
        vkCmdBeginQuery(cmd, p->stats_pool, pass->stats_query, 0);
    }
}

FORCE_INLINE void gpu_profiler_end_pass(GpuProfiler* p, VkCommandBuffer cmd, VkPipelineStageFlagBits2 stage)
{


    uint32_t q = p->query_count++;

    GpuPass* pass = &p->passes[p->pass_count];

    pass->end_query = q;

    vkCmdWriteTimestamp2(cmd, stage, p->timestamp_pool, q);

    if(p->enable_pipeline_stats && p->stats_pool != VK_NULL_HANDLE)
    {
        vkCmdEndQuery(cmd, p->stats_pool, pass->stats_query);
    }

    p->pass_count++;
}

FORCE_INLINE void gpu_profiler_collect(GpuProfiler* p, VkDevice device)
{
    if (p->query_count == 0) return;

    uint64_t timestamps[MAX_GPU_PASSES * 2];

    VkResult time_res = vkGetQueryPoolResults(device, p->timestamp_pool, 0, p->query_count, p->query_count * sizeof(uint64_t), timestamps,
                                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    if(time_res != VK_SUCCESS)
        return;

    for(uint32_t i = 0; i < p->pass_count; i++)
    {
        GpuPass* pass = &p->passes[i];

        uint64_t t0 = timestamps[pass->start_query];
        uint64_t t1 = timestamps[pass->end_query];

        double ns = (t1 - t0) * p->timestamp_period;

        pass->time_ms = ns / 1000000.0;
    }

    if(!p->enable_pipeline_stats || p->stats_pool == VK_NULL_HANDLE || p->pass_count == 0)
        return;

    struct
    {
        uint64_t vs;
        uint64_t fs;
        uint64_t prim;
    } stats[MAX_GPU_PASSES];

    VkResult stats_res = vkGetQueryPoolResults(device, p->stats_pool, 0, p->pass_count, p->pass_count * sizeof(stats[0]), stats,
                                               sizeof(stats[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    if(stats_res != VK_SUCCESS)
        return;

    for(uint32_t i = 0; i < p->pass_count; i++)
    {
        p->passes[i].vs_invocations = stats[i].vs;
        p->passes[i].fs_invocations = stats[i].fs;
        p->passes[i].primitives     = stats[i].prim;
    }
}
#define GPU_SCOPE(prof,cmd,name,stage)                                       \
    for(int _gpu_scope_once =                                                \
            (gpu_profiler_begin_pass((prof),cmd,name,stage),0);              \
        _gpu_scope_once == 0;                                                \
        gpu_profiler_end_pass((prof),cmd,stage), _gpu_scope_once++)
// #define GPU_SCOPE(cmd,name,stage)                                             \
//     for(int _gpu_scope_once =                                                 \
//             (gpu_profiler_begin_pass(&renderer.profiler,cmd,name,stage),0);  \
//         _gpu_scope_once == 0;                                                 \
//         gpu_profiler_end_pass(&renderer.profiler,cmd,stage), _gpu_scope_once++)
