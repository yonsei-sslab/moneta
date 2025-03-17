#ifndef PERISCOPE_TIMERS_H
#define PERISCOPE_TIMERS_H

#include <stdio.h>
#include "qemu/osdep.h"
#include "sysemu/sysemu.h"

//#define PERISCOPE_TIMERS

struct peri_timer;
typedef struct peri_timer {
   char* name;
   qemu_timeval t0;
   qemu_timeval t1;
   char started;
   FILE* fp;
   QLIST_ENTRY(peri_timer) list;
} peri_timer;

#ifdef PERISCOPE_TIMERS
int add_timer(const char* name);
int remove_timer(const char* name);
peri_timer* start_interval(const char* name);
int stop_interval(peri_timer* pt);
void pt_mark_all(const char *mark);
#else
static inline int add_timer(const char* name) { return 0;}
static inline int remove_timer(const char* name) {return 0;}
static inline peri_timer* start_interval(const char* name) {return NULL;}
static inline int stop_interval(peri_timer* pt) {return 0;}
static inline void pt_mark_all(const char *mark) {}
#endif


#endif
