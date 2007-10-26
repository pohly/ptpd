#include "../ptpd.h"
#include <stdarg.h>


Boolean initTime(RunTimeOpts *rtOpts, PtpClock *ptpClock)
{
  return TRUE;
}

void getTime(TimeInternal *time, PtpClock *ptpClock)
{
  struct timeval tv;

  gettimeofday(&tv, 0);
  time->seconds = tv.tv_sec;
  time->nanoseconds = tv.tv_usec*1000;
}

void setTime(TimeInternal *time, PtpClock *ptpClock)
{
  struct timeval tv;

  tv.tv_sec = time->seconds;
  tv.tv_usec = time->nanoseconds/1000;
  settimeofday(&tv, 0);

  NOTIFY("resetting system clock to %ds %dns\n", time->seconds, time->nanoseconds);
}

Boolean adjFreq(Integer32 adj, PtpClock *ptpClock)
{
  struct timex t;

  if(adj > ADJ_FREQ_MAX)
    adj = ADJ_FREQ_MAX;
  else if(adj < -ADJ_FREQ_MAX)
    adj = -ADJ_FREQ_MAX;

  t.modes = MOD_FREQUENCY;
  t.freq = adj*((1<<16)/1000);

  return !adjtimex(&t);
}
