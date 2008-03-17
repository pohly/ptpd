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


void displayStats(PtpClock *ptpClock)
{
  static int start = 1;
  static char sbuf[SCREEN_BUFSZ];
  char *s;
  int len = 0;
  
  if(start && ptpClock->runTimeOpts.csvStats)
  {
    start = 0;
    INFO("state, one way delay, offset from master, drift, variance\n");
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
    len += sprintf(sbuf + len,
      ", %s%d.%09d" ", %s%d.%09d",
      ptpClock->runTimeOpts.csvStats ? "" : "owd: ",
      ptpClock->one_way_delay.seconds,
      abs(ptpClock->one_way_delay.nanoseconds),
      ptpClock->runTimeOpts.csvStats ? "" : "ofm: ",
      ptpClock->offset_from_master.seconds,
      abs(ptpClock->offset_from_master.nanoseconds));
    
    len += sprintf(sbuf + len, 
      ", %s%d" ", %s%d",
      ptpClock->runTimeOpts.csvStats ? "" : "drift: ", ptpClock->observed_drift,
      ptpClock->runTimeOpts.csvStats ? "" : "var: ", ptpClock->observed_variance);
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

