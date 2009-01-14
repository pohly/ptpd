/* sys.c */

#include "../ptpd.h"
#include <stdarg.h>

Boolean useSyslog;

void message(int priority, const char *format, ...)
{
  va_list ap;

  va_start(ap, format);
  if (useSyslog)
  {
    static Boolean logOpened;
    if (!logOpened)
    {
      openlog("ptpd", 0, LOG_USER);
      logOpened = TRUE;
    }
    vsyslog(priority, format, ap);
  }
  else
  {
    fprintf(stderr, "(ptpd %s) ",
            priority == LOG_EMERG ? "emergency" :
            priority == LOG_ALERT ? "alert" :
            priority == LOG_CRIT ? "critical" :
            priority == LOG_ERR ? "error" :
            priority == LOG_WARNING ? "warning" :
            priority == LOG_NOTICE ? "notice" :
            priority == LOG_INFO ? "info" :
            priority == LOG_DEBUG ? "debug" :
            "???");
    vfprintf(stderr, format, ap);
  }
  va_end(ap);
}

static size_t sprintfTime(PtpClock *ptpClock, char *buffer, TimeInternal *t, const char *prefix)
{
  return sprintf(buffer,
                 ", %s%s%d.%09d",
                 ptpClock->runTimeOpts.csvStats ? "" : prefix,
                 (t->seconds < 0 || t->nanoseconds < 0) ? "-" : "",
                 abs(t->seconds),
                 abs(t->nanoseconds));
}

void displayStats(PtpClock *ptpClock)
{
  static int start = 1;
  static char sbuf[2 * SCREEN_BUFSZ];
  char *s;
  int len = 0;
  
  if(start && ptpClock->runTimeOpts.csvStats)
  {
    start = 0;
    INFO("state, one way delay, offset from master, drift, variance, clock adjustment (ppb), slave to master delay, master to slave delay\n");
    fflush(stdout);
  }
  
  memset(sbuf, ' ', SCREEN_BUFSZ);
  
  switch(ptpClock->port_state)
  {
  case PTP_INITIALIZING:  s = "init";  break;
  case PTP_FAULTY:        s = "flt";   break;
  case PTP_LISTENING:     s = "lstn";  break;
  case PTP_PASSIVE:       s = "pass";  break;
  case PTP_UNCALIBRATED:  s = "uncl";  break;
  case PTP_SLAVE:         s = "slv";   break;
  case PTP_PRE_MASTER:    s = "pmst";  break;
  case PTP_MASTER:        s = "mst";   break;
  case PTP_DISABLED:      s = "dsbl";  break;
  default:                s = "?";     break;
  }
  
  len += sprintf(sbuf + len, "%s%s%s", ptpClock->runTimeOpts.csvStats ? "": "state: ", ptpClock->name, s);
  
  if(ptpClock->port_state == PTP_SLAVE ||
     (ptpClock->port_state == PTP_MASTER && ptpClock->nic_instead_of_system))
  {
    len += sprintfTime(ptpClock, sbuf + len, &ptpClock->one_way_delay, "owd: ");
    len += sprintfTime(ptpClock, sbuf + len, &ptpClock->offset_from_master, "ofm: ");

    len += sprintf(sbuf + len, 
      ", %s%d" ", %s%d",
      ptpClock->runTimeOpts.csvStats ? "" : "drift: ", ptpClock->observed_drift,
      ptpClock->runTimeOpts.csvStats ? "" : "var: ", ptpClock->observed_variance);

    len += sprintf(sbuf + len,
      ", %s%ld",
      ptpClock->runTimeOpts.csvStats ? "" : "adj: ", ptpClock->adj);

    len += sprintfTime(ptpClock, sbuf + len, &ptpClock->slave_to_master_delay, "stm: ");
    len += sprintfTime(ptpClock, sbuf + len, &ptpClock->master_to_slave_delay, "mts: ");
  }

  if (ptpClock->runTimeOpts.csvStats)
  {
    INFO("%s\n", sbuf);
  }
  else
  {
    /* overwrite the same line over and over again... */
    INFO("%.*s\r", SCREEN_MAXSZ + 1, sbuf);
  }
}

UInteger16 getRand(UInteger32 *seed)
{
  return rand_r((unsigned int*)seed);
}

