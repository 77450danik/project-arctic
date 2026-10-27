/* The Games panel's settings and the machine's tuning (docs/games.md):
 * undervolt, the processor's power limits and top frequency, the graphics
 * card's power and clocks, whatever this machine allows. */
#ifndef ARCTIC_TUNING_H
#define ARCTIC_TUNING_H

#include <sys/types.h>

/* what C:\ProgramData\Arctic\games.ini says (the Games panel writes it) */
struct games_settings {
    int game_mode;      /* a game that fills a monitor gets the priority; 1 unless turned off */
    int apply_at_boot;  /* the tuning below is applied at every start */
    int cpu_core_mv;    /* voltage offsets, 0 or less */
    int cpu_cache_mv;
    int cpu_pl1_w;      /* 0: the firmware's */
    int cpu_pl2_w;
    int cpu_max_mhz;    /* 0: no limit */
    int cpu_turbo_ratio; /* 0: the firmware's turbo ratios */
    int gpu_power_w;    /* 0: the card's default */
    int gpu_core_mhz;   /* clock offsets */
    int gpu_mem_mhz;
    int gpu_max_mhz;    /* 0: no limit */
};

extern struct games_settings games;
extern void (*tuning_log)(const char *fmt, ...);

void games_load(void);
/* once the hardware is there: what can be tuned, and the tuning when set to come at start */
void tuning_boot(int keeps_state, uid_t owner, gid_t group);
/* the panel asked: the settings again, applied now */
void tuning_apply(void);
/* a game came: a card that slept forgot its settings */
void tuning_game_started(void);
/* the main loop: a start with the tuning that lasted two minutes is a good one */
void tuning_tick(void);

#endif
