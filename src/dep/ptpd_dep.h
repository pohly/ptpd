/* ptpd_dep.h */

#ifndef PTPD_DEP_H
#define PTPD_DEP_H

#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<unistd.h>
#include<errno.h>
#include<signal.h>
#include<fcntl.h>
#include<syslog.h>
#include<sys/stat.h>
#include<time.h>
#include<sys/time.h>
#include<sys/timex.h>
#include<sys/socket.h>
#include<sys/select.h>
#include<sys/ioctl.h>
#include<arpa/inet.h>

#ifdef HAVE_LINUX_NET_TSTAMP_H
#include "asm/types.h"
#include "linux/net_tstamp.h"
#include "linux/errqueue.h"

#ifndef SO_TIMESTAMPNS
# define SO_TIMESTAMPNS 35
#endif

#ifndef SO_TIMESTAMPING
# define SO_TIMESTAMPING 37
#endif

#ifndef SIOCGSTAMPNS
# define SIOCGSTAMPNS 0x8907
#endif

#ifndef SIOCSHWTSTAMP
# define SIOCSHWTSTAMP 0x89b0
#endif

#endif /* HAVE_LINUX_NET_TSTAMP_H */

/**
 * route output either into syslog or stderr, depending on global useSyslog settings
 * @param priority       same as for syslog()
 * @param format         printf style format string
 */
void message(int priority, const char *format, ...);

/**
 * if TRUE then message() will print via syslog(); no init required and
 * can be reverted to FALSE at any time
 */
extern Boolean useSyslog;

/* system messages */
#define ERROR(x, ...)  message(LOG_ERR, x, ##__VA_ARGS__)
#define PERROR(x, ...) message(LOG_ERR, x ": %m\n", ##__VA_ARGS__)
#define NOTIFY(x, ...) message(LOG_NOTICE, x, ##__VA_ARGS__)
#define INFO(x, ...)   message(LOG_INFO, x, ##__VA_ARGS__)

/* debug messages */
#ifdef PTPD_DBGV
#define PTPD_DBG
#define DBGV(x, ...) message(LOG_DEBUG, x, ##__VA_ARGS__)
#else
#define DBGV(x, ...)
#endif

#ifdef PTPD_DBG
#define DBG(x, ...)  message(LOG_DEBUG, x, ##__VA_ARGS__)
#else
#define DBG(x, ...)
#endif

/* endian corrections */
#if defined(PTPD_MSBF)
#define shift8(x,y)   ( (x) << ((3-y)<<3) )
#define shift16(x,y)  ( (x) << ((1-y)<<4) )
#elif defined(PTPD_LSBF)
#define shift8(x,y)   ( (x) << ((y)<<3) )
#define shift16(x,y)  ( (x) << ((y)<<4) )
#endif

#define flip16(x) htons(x)
#define flip32(x) htonl(x)

/* i don't know any target platforms that do not have htons and htonl,
   but here are generic funtions just in case */
/*
#if defined(PTPD_MSBF)
#define flip16(x) (x)
#define flip32(x) (x)
#elif defined(PTPD_LSBF)
static inline Integer16 flip16(Integer16 x)
{
   return (((x) >> 8) & 0x00ff) | (((x) << 8) & 0xff00);
}

static inline Integer32 flip32(x)
{
  return (((x) >> 24) & 0x000000ff) | (((x) >> 8 ) & 0x0000ff00) |
         (((x) << 8 ) & 0x00ff0000) | (((x) << 24) & 0xff000000);
}
#endif
*/

/* bit array manipulation */
#define getFlag(x,y)  !!( *(UInteger8*)((x)+((y)<8?1:0)) &   (1<<((y)<8?(y):(y)-8)) )
#define setFlag(x,y)    ( *(UInteger8*)((x)+((y)<8?1:0)) |=   1<<((y)<8?(y):(y)-8)  )
#define clearFlag(x,y)  ( *(UInteger8*)((x)+((y)<8?1:0)) &= ~(1<<((y)<8?(y):(y)-8)) )


/* msg.c */
Boolean msgPeek(void*,ssize_t);
void msgUnpackHeader(void*,MsgHeader*);
void msgUnpackSync(void*,MsgSync*);
void msgUnpackDelayReq(void*,MsgDelayReq*);
void msgUnpackFollowUp(void*,MsgFollowUp*);
void msgUnpackDelayResp(void*,MsgDelayResp*);
void msgUnpackManagement(void*,MsgManagement*);
UInteger8 msgUnloadManagement(void*,MsgManagement*,PtpClock*);
void msgUnpackManagementPayload(void *buf, MsgManagement *manage);
void msgPackHeader(void*,PtpClock*);
void msgPackSync(void*,Boolean,Boolean,TimeRepresentation*,PtpClock*);
void msgPackDelayReq(void*,Boolean,Boolean,TimeRepresentation*,PtpClock*);
void msgPackFollowUp(void*,UInteger16,TimeRepresentation*,PtpClock*);
void msgPackDelayResp(void*,MsgHeader*,TimeRepresentation*,PtpClock*);
UInteger16 msgPackManagement(void*,MsgManagement*,PtpClock*);
UInteger16 msgPackManagementResponse(void*,MsgHeader*,MsgManagement*,PtpClock*);

/* net.c */
/* linux API dependent */
Boolean netInit(PtpClock*);
Boolean netShutdown(PtpClock*);
int netSelect(TimeInternal*,PtpClock*);
ssize_t netRecvEvent(Octet*,TimeInternal*,PtpClock*);
ssize_t netRecvGeneral(Octet*,PtpClock*);
ssize_t netSendEvent(Octet*,UInteger16,TimeInternal*,PtpClock*);
ssize_t netSendGeneral(Octet*,UInteger16,PtpClock*);

/* servo.c */
void initClock(PtpClock*);
void updateDelay(TimeInternal*,TimeInternal*,
  one_way_delay_filter*,PtpClock*);
void updateOffset(TimeInternal*,TimeInternal*,
  offset_from_master_filter*,PtpClock*);
void updateClock(PtpClock*);

/* startup.c */
/* unix API dependent */
PtpClock * ptpdStartup(int,char**,Integer16*,RunTimeOpts*);
void ptpdShutdown(void);

/* sys.c */
/* unix API dependent */
void displayStats(PtpClock*);
UInteger16 getRand(UInteger32*);

/**
 * @defgroup time Time Source
 *
 * Interface to the clock which is used to time stamp
 * packages and which is adjusted by PTPd.
 *
 * The intention is to hide different actual implementations
 * behind one interface:
 * - system time (gettimeofday())
 * - NIC time (timer inside the network hardware)
 * - ...
 */
/*@{*/
/** @file time.c */
Boolean initTime(PtpClock*);
void getTime(TimeInternal*, PtpClock*);
void setTime(TimeInternal*, PtpClock*);

/**
 * Adjusts the time, ideally by varying the clock rate.
 *
 * @param adj      frequency adjustment: a time source which supports that ignores the offset
 * @param offset   offset (reference time - local time) from last measurement: a time source which
 *                 cannot adjust the frequence must fall back to this cruder method (may be NULL)
 */
void adjTime(Integer32 adj, TimeInternal *offset, PtpClock*);

/**
 * Adjusts the time by shifting the clock.
 *
 * @param offset   this value must be substracted from clock (might be negative)
 */
void adjTimeOffset(TimeInternal *offset, PtpClock*);

/**
 * Gets the time when the latest outgoing packet left the host.
 *
 * There is no way to identify the packet the time stamp belongs to,
 * so this must be called after sending each packet until the time
 * stamp for the packet is available. This can be some (hopefully
 * small) time after the packet was passed to the IP stack.
 *
 * There is no mechanism either to determine packet loss and thus a
 * time stamp which never becomes available.
 *
 * @todo Can such packet loss occur?
 *
 * Does not work with TIME_SYSTEM.
 *
 * @retval sendTimeStamp    set to the time when the packet left the host
 * @return TRUE if the time stamp was available
 */
Boolean getSendTime(TimeInternal *sendTimeStamp, PtpClock*);

/**
 * Gets the time when the packet identified by the given attributes
 * was received by the host.
 *
 * Because the arrival of packets is out of the control of PTPd, the
 * time stamping must support unique identification of which time
 * stamp belongs to which packet.
 *
 * Due to packet loss in the receive queue, there can be time stamps
 * without IP packets. getReceiveTime() automatically discards stale
 * time stamps, including the ones that where returned by
 * getReceiveTime(). This implies that there is not guarantee that
 * calling getReceiveTime() more than once for the same packet
 * will always return a result.
 *
 * Due to hardware limitations only one time stamp might be stored
 * until queried by the NIC driver; this can lead to packets without
 * time stamp. This needs to be handled by the caller of
 * getReceiveTime(), for example by ignoring the packet.
 *
 * Does not work with TIME_SYSTEM.
 *
 * @retval recvTimeStamp    set to the time when the packet entered the host, if available
 * @return TRUE if the time stamp was available
 */
Boolean getReceiveTime(TimeInternal *recvTimeStamp,
                       Octet sourceUuid[PTP_UUID_LENGTH],
                       UInteger16 sequenceId, PtpClock*);

/** called regularly every second while process is idle */
void timeNoActivity(PtpClock*);

/**
 * called while still in the old state and before entering a new one:
 * transition is relevant for hardware assisted timing
 */
void timeToState(UInteger8 state, PtpClock *ptpClock);

/*@}*/

/**
 * @defgroup timer regular wakeup at different timer intervals
 *
 * This timing is always done using the system time of the host.
 */
/*@{*/
/** @file timer.c */
void initTimer(void);
void timerUpdate(IntervalTimer*);
void timerStop(UInteger16,IntervalTimer*);
void timerStart(UInteger16,UInteger16,IntervalTimer*);
Boolean timerExpired(UInteger16,IntervalTimer*);
Boolean nanoSleep(TimeInternal*);
/** gets the current system time */
void timerNow(TimeInternal*);
/*@}*/

#endif

