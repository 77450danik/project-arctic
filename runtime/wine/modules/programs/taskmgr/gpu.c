/*
 * Task Manager: the graphics cards
 *
 * What each card is doing comes from powrprof's ArcticGpuList, as the
 * Control Panel's battery page reads it: the load the driver reports
 * (amdgpu's gpu_busy_percent, NVIDIA's NVML, the engine time of every
 * program for Intel's), the temperature and the card's own memory. NVML
 * takes a while to start each time, so the cards are asked on a thread of
 * their own and each sample takes the latest answer.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "taskmgr.h"
#include "wine/arctic_power.h"

static struct arctic_gpu_list asked, latest;
static BOOL have_latest;
static CRITICAL_SECTION section;
static HANDLE wake;

static DWORD WINAPI gpu_thread( void *arg )
{
    for (;;)
    {
        WaitForSingleObject( wake, INFINITE );
        if (!ArcticGpuList( &asked, TRUE )) continue;
        EnterCriticalSection( &section );
        latest = asked;
        have_latest = TRUE;
        LeaveCriticalSection( &section );
    }
    return 0;
}

static double mb( UINT32 value )
{
    return value == ARCTIC_UNKNOWN ? NO_VALUE : (double)value * 1024 * 1024;
}

void gpu_init(void)
{
    HANDLE thread;

    InitializeCriticalSection( &section );
    /* the first look in front, so the tiles are there when the window opens */
    if (ArcticGpuList( &latest, TRUE )) have_latest = TRUE;
    wake = CreateEventW( NULL, FALSE, FALSE, NULL );
    if ((thread = CreateThread( NULL, 0, gpu_thread, NULL, 0, NULL ))) CloseHandle( thread );
}

void gpu_sample( struct sample *s )
{
    UINT count;

    EnterCriticalSection( &section );
    if (have_latest)
    {
        /* the processor's own graphics first, as Windows numbers them */
        count = 0;
        for (int pass = 0; pass < 2; pass++)
        {
            for (UINT i = 0; i < latest.count && count < MAX_GPUS; i++)
            {
                const struct arctic_gpu *g = &latest.gpus[i];
                struct gpu_info *info = &sysinfo.gpus[count];

                if ((pass == 0) != !!g->integrated) continue;
                lstrcpynW( info->name, g->name, ARRAY_SIZE(info->name) );
                lstrcpynW( info->driver, g->driver, ARRAY_SIZE(info->driver) );
                lstrcpynW( info->pci, g->pci, ARRAY_SIZE(info->pci) );
                info->integrated = g->integrated;
                if (g->memory_total_mb != ARCTIC_UNKNOWN) info->mem_total = mb( g->memory_total_mb );
                else if (!info->mem_total) info->mem_total = NO_VALUE;

                /* a card asleep does nothing */
                s->gpus[count].util = g->busy_percent == ARCTIC_UNKNOWN || !g->active ? 0 : min( 100, g->busy_percent );
                s->gpus[count].mem_used = mb( g->memory_used_mb );
                s->gpus[count].temperature = g->temperature_mc == ARCTIC_UNKNOWN || !g->active
                                             ? NO_VALUE : g->temperature_mc / 1000.0;
                count++;
            }
        }
        sysinfo.gpu_count = count;
    }
    else
    {
        for (UINT i = 0; i < MAX_GPUS; i++) s->gpus[i].mem_used = s->gpus[i].temperature = NO_VALUE;
    }
    LeaveCriticalSection( &section );
    SetEvent( wake );
}
