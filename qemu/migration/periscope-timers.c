#include "periscope-timers.h"

#ifdef PERISCOPE_TIMERS
static QLIST_HEAD( ,peri_timer) peri_timers =
    QLIST_HEAD_INITIALIZER(peri_timers);

static unsigned long long cntr = 0;

static peri_timer* find_ptimer(const char* name)
{
   peri_timer *pt;
   QLIST_FOREACH(pt, &peri_timers, list) {
      if(strcmp(name, pt->name) == 0) return pt;
   }
   return NULL;

}

int add_timer(const char* name)
{
   peri_timer *pt = find_ptimer(name);
   if(pt != NULL) {
      printf("Warning timer %s already active\n", name);
      return -1;
   }
   pt = g_malloc(sizeof(peri_timer));
   pt->started = 0;
   pt->name = g_malloc(strlen(name));
   strcpy(pt->name, name);
   pt->fp = fopen(name, "w");
   if(!pt->fp) {
      printf("Failed to open %s\n", name);
      g_free(pt->name);
      g_free(pt);
      return -1;
   }
   QLIST_INSERT_HEAD(&peri_timers, pt, list);
   return 0;
}

int remove_timer(const char* name)
{
   peri_timer *pt = find_ptimer(name);
   if(pt == NULL) {
      printf("Warning timer %s not active\n", name);
      return -1;
   }
   QLIST_REMOVE(pt, list);
   fclose(pt->fp);
   g_free(pt->name);
   g_free(pt);
   return 0;
}

peri_timer* start_interval(const char* name)
{
   peri_timer *pt = find_ptimer(name);
   if(pt == NULL) {
      printf("Warning timer %s not active\n", name);
      return NULL;
   }
   if(pt->started) {
      printf("Warning timer %s already started\n", name);
      return NULL;
   }
   pt->started = 1;
   qemu_gettimeofday(&pt->t0);
   return pt;
}

int stop_interval(peri_timer* pt)
{
   qemu_timeval elapsed;
   char buf[256];
   if(!pt->started) {
      printf("Warning timer %s not started\n", pt->name);
      return -1;
   }
   qemu_gettimeofday(&pt->t1);
   timersub(&pt->t1, &pt->t0, &elapsed);
   snprintf(buf, 256, "%llu %llu\n", cntr++, elapsed.tv_sec * 1000000ULL + elapsed.tv_usec);
   fwrite(buf, strlen(buf), 1, pt->fp);
   fflush(pt->fp);
   pt->started = 0;
   return 0;
}

void pt_mark_all(const char *mark)
{
   peri_timer *pt;
   QLIST_FOREACH(pt, &peri_timers, list) {
      fwrite(mark, strlen(mark), 1, pt->fp);
      fflush(pt->fp);
   }
}
#endif
