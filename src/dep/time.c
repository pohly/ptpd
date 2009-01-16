#include "../ptpd.h"
#include <stdarg.h>

#include "e1000_ioctl.h"

/** global state for controlling system time when TIME_BOTH is selected */
static PtpClock timeBothClock;

/**
 * Most recent send time stamp from NIC, 0/0 if none available right now.
 * Reset by getSendTime().
 */
static TimeInternal lastSendTime;

#ifndef RECV_ARRAY_SIZE
/**
 * Must be large enough to buffer all time stamps received from the NIC
 * but not yet requested by the protocol processor. Because new information
 * can only be added when the protocol asks for old one, this should not
 * get very full.
 */
# define RECV_ARRAY_SIZE 10
#endif

/**
 * An array of the latest RECV_ARRAY_SIZE packet receive information.
 * Once it overflows the oldest ones are overwritten in a round-robin
 * fashion.
 */
static struct {
  TimeInternal recvTimeStamp;
  UInteger16 sequenceId;
  Octet sourceUuid[PTP_UUID_LENGTH];
} lastRecvTimes[RECV_ARRAY_SIZE];

/**
 * Oldest valid and next free entry in lastRecvTimes.
 * Valid ones are [oldest, free[ if oldest <= free,
 * otherwise [oldest, RECV_ARRAY_SIZE[ and [0, free[.
 */
static int oldestRecv, nextFreeRecv;

/**
 * if TIME_BOTH: measure NIC<->system time offsets and adapt system time
 *
 * This function is called whenever init.c gets control; to prevent to
 * frequent changes it ignores invocations less than one second away from
 * the previous one.
 */
static void syncSystemWithNIC(PtpClock *ptpClock)
{
  struct E1000_TSYNC_COMPARETS_ARGU ts;
  TimeInternal delay;
  static TimeInternal zero;

  if(ptpClock->runTimeOpts.time != TIME_BOTH)
    return;
  else
  {
#if 1
    static TimeInternal lastsync;
    TimeInternal now, offset;
    timerNow(&now);
    subTime(&offset, &now, &lastsync);
    if(offset.seconds <= 0)
      return;
    lastsync = now;
#endif
  }

  ptpClock->netPath.eventSockIFR.ifr_data = (void *)&ts;
  memset(&ts, 0, sizeof(ts));
  if (ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_COMPARETS_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
    ERROR("could not correlate E1000 hardware and system time on %s: %s\n",
          ptpClock->netPath.eventSockIFR.ifr_name,
          strerror(errno));
    return;
  }
  delay.seconds = ts.systemToNIC.seconds * ts.systemToNICSign;
  delay.nanoseconds = ts.systemToNIC.nanoseconds * ts.systemToNICSign;
  DBGV("system to NIC delay %ld.%09d\n",
       delay.seconds, delay.nanoseconds);
  updateDelay(&delay, &zero, &timeBothClock.owd_filt, &timeBothClock);

  delay.seconds = ts.NICToSystem.seconds * ts.NICToSystemSign;
  delay.nanoseconds = ts.NICToSystem.nanoseconds * ts.NICToSystemSign;
  DBGV("NIC to system delay %ld.%09d\n",
       delay.seconds, delay.nanoseconds);
  updateOffset(&delay, &zero, &timeBothClock.ofm_filt, &timeBothClock);

  if(ptpClock->port_state == PTP_MASTER)
  {
    timeBothClock.nic_instead_of_system = TRUE;
    timeBothClock.runTimeOpts.time = TIME_NIC;
  }
  else
  {
    timeBothClock.nic_instead_of_system = FALSE;
    timeBothClock.runTimeOpts.time = TIME_SYSTEM;
  }
  updateClock(&timeBothClock);
  DBGV("system time updated\n");
}

static Boolean selectNICTimeMode(Boolean sync, PtpClock *ptpClock)
{
  DBGV("time stamp incoming %s packets\n", sync ? "Sync" : "Delay_Req");

  switch (ptpClock->runTimeOpts.time) {
#ifdef HAVE_LINUX_NET_TSTAMP_H
  case TIME_SYSTEM_LINUX_HW: {
      struct hwtstamp_config hwconfig;
      int so_timestamping_flags;

      ptpClock->netPath.eventSockIFR.ifr_data = (void *)&hwconfig;
      memset(&hwconfig, 0, sizeof(&hwconfig));

      /*
       * Configure for time stamping of incoming Sync or Delay_Req
       * messages and for time stamping of all out-going event
       * messages. Out-going messages will be bounced via the error
       * queue of the event socket.
       */
      hwconfig.tx_type = HWTSTAMP_TX_ON;
      hwconfig.rx_filter = sync ?
          HWTSTAMP_FILTER_PTP_V1_L4_SYNC :
          HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ;
      so_timestamping_flags = SOF_TIMESTAMPING_TX_HARDWARE|SOF_TIMESTAMPING_RX_HARDWARE|SOF_TIMESTAMPING_SYS_HARDWARE;

      if (ioctl(ptpClock->netPath.eventSock, SIOCSHWTSTAMP, &ptpClock->netPath.eventSockIFR) < 0) {
          if (errno == ERANGE) {
              /* hardware time stamping not supported */
              PERROR("net_tstamp SIOCSHWTSTAMP: mode of operation not supported");
              return FALSE;
          } else {
              PERROR("net_tstamp SIOCSHWTSTAMP: %s", strerror(errno));
              return FALSE;
          }
      }

      if (setsockopt(ptpClock->netPath.eventSock, SOL_SOCKET, SO_TIMESTAMPING, &so_timestamping_flags, sizeof(so_timestamping_flags)) < 0) {
          PERROR("net_tstamp SO_TIMESTAMPING: %s", strerror(errno));
          return FALSE;
      }
      break;
  }
  case TIME_SYSTEM_LINUX_SW: {
      /* same as before, but without requiring support by the NIC */
      int so_timestamping_flags =
          SOF_TIMESTAMPING_TX_SOFTWARE|SOF_TIMESTAMPING_RX_SOFTWARE|SOF_TIMESTAMPING_SOFTWARE;
      if (setsockopt(ptpClock->netPath.eventSock, SOL_SOCKET, SO_TIMESTAMPING, &so_timestamping_flags, sizeof(so_timestamping_flags)) < 0) {
          PERROR("net_tstamp SO_TIMESTAMPING: %s", strerror(errno));
          return FALSE;
      }
      break;
  }
#else
  case TIME_SYSTEM_LINUX_SW:
  case TIME_SYSTEM_LINUX_HW:
      PERROR("net_tstamp interface not supported");
      return FALSE;
#endif /* HAVE_LINUX_NET_TSTAMP_H */
  default:
      *(int *)&ptpClock->netPath.eventSockIFR.ifr_data = sync ? E1000_UDP_V1_SYNC : E1000_UDP_V1_DELAY;
      if(ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_ENABLERX_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
          ERROR("could not activate E1000 hardware receive time stamping on %s: %s\n",
                ptpClock->netPath.eventSockIFR.ifr_name,
                strerror(errno));
          return FALSE;
      }
      break;
  }

  return TRUE;
}

static Boolean initNICTime(Boolean sync, PtpClock *ptpClock)
{
  /** @todo also check success indicator in ifr_data */
  if (ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_INIT_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
    ERROR("could not activate E1000 hardware time stamping on %s: %s\n",
          ptpClock->netPath.eventSockIFR.ifr_name,
          strerror(errno));
  }
  else if(ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_ENABLETX_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
    ERROR("could not activate E1000 hardware send time stamping on %s: %s\n",
          ptpClock->netPath.eventSockIFR.ifr_name,
          strerror(errno));
  }
  else if(!selectNICTimeMode(sync, ptpClock)) {
    // error already printed
  }
  else {
#if 0
    // move the NIC time for debugging purposes
    TimeInternal timeTmp;

    DBGV("shift NIC time\n");
    getTime(&timeTmp, ptpClock);
    timeTmp.seconds -= 2;
    setTime(&timeTmp, ptpClock);
    DBGV("shift NIC time done\n");
#endif
    return TRUE;
  }

  return FALSE;
}

Boolean initTime(PtpClock *ptpClock)
{
  switch(ptpClock->runTimeOpts.time) {
  case TIME_SYSTEM:
    return TRUE;
    break;
  case TIME_BOTH:
    /* prepare clock servo for controlling system time */
    timeBothClock = *ptpClock;
    timeBothClock.runTimeOpts.time = TIME_SYSTEM;
    timeBothClock.name = "sys ";
    initClock(&timeBothClock);

    /* default options for NIC synchronization */
    ptpClock->runTimeOpts.noResetClock = DEFAULT_NO_RESET_CLOCK;
    ptpClock->runTimeOpts.noAdjust = DEFAULT_NO_ADJUST_CLOCK;
    ptpClock->runTimeOpts.s = DEFAULT_DELAY_S;
    ptpClock->runTimeOpts.ap = DEFAULT_AP;
    ptpClock->runTimeOpts.ai = DEFAULT_AI;

    return initNICTime(TRUE, ptpClock);
    break;
  case TIME_SYSTEM_LINUX_HW:
  case TIME_SYSTEM_LINUX_SW:
    return selectNICTimeMode(TRUE, ptpClock);
    break;
  case TIME_NIC:
  case TIME_SYSTEM_ASSISTED:
    return initNICTime(TRUE, ptpClock);
    break;
  default:
    ERROR("unsupported selection of time source\n");
    return FALSE;
    break;
  }
}

void getTime(TimeInternal *time, PtpClock *ptpClock)
{
  switch(ptpClock->runTimeOpts.time)
  {
  case TIME_SYSTEM_LINUX_HW:
  case TIME_SYSTEM_LINUX_SW:
  case TIME_SYSTEM_ASSISTED:
  case TIME_SYSTEM: {
    struct timeval tv;

    gettimeofday(&tv, 0);
    time->seconds = tv.tv_sec;
    time->nanoseconds = tv.tv_usec*1000;
    break;
  }
  case TIME_BOTH:
  case TIME_NIC: {
    struct E1000_TSYNC_SYSTIME_ARGU ts;

    ptpClock->netPath.eventSockIFR.ifr_data = (void *)&ts;
    memset(&ts, 0, sizeof(ts));
    if (ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_SYSTIME_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
      ERROR("could not read E1000 hardware time on %s: %s\n",
            ptpClock->netPath.eventSockIFR.ifr_name,
            strerror(errno));
      return;
    }
    time->seconds = ts.time.seconds;
    time->nanoseconds = ts.time.nanoseconds;

    syncSystemWithNIC(ptpClock);
    break;
  }
  default:
    ERROR("unsupported selection of time source\n");
    break;
  }
}

void setTime(TimeInternal *time, PtpClock *ptpClock)
{
  switch(ptpClock->runTimeOpts.time)
  {
  case TIME_SYSTEM_LINUX_HW:
  case TIME_SYSTEM_LINUX_SW:
  case TIME_SYSTEM_ASSISTED:
  case TIME_SYSTEM: {
    NOTIFY("resetting system clock to %ds %dns\n", time->seconds, time->nanoseconds);
    struct timeval tv;
    tv.tv_sec = time->seconds;
    tv.tv_usec = time->nanoseconds/1000;
    settimeofday(&tv, 0);
    break;
  }
  case TIME_BOTH:
  case TIME_NIC: {
    struct E1000_TSYNC_SYSTIME_ARGU ts;
    TimeInternal currentTime, offset;

    NOTIFY("resetting NIC clock to %ds %dns\n", time->seconds, time->nanoseconds);
    memset(&ts, 0, sizeof(ts));
    getTime(&currentTime, ptpClock);
    subTime(&offset, time, &currentTime);
    ts.negative_offset = (offset.seconds < 0 || offset.nanoseconds < 0) ? -1 : 1;
    ts.time.seconds = ts.negative_offset * offset.seconds;
    ts.time.nanoseconds = ts.negative_offset * offset.nanoseconds;
    ptpClock->netPath.eventSockIFR.ifr_data = (void *)&ts;
    NOTIFY("adding NIC offset %s%ld.%09d (%ld/%p)\n",
         ts.negative_offset < 0 ? "-" : "",
         ts.time.seconds,
         ts.time.nanoseconds,
         sizeof(ts), &ts);
    if (ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_SYSTIME_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
      ERROR("could not set E1000 hardware time on %s: %s\n",
            ptpClock->netPath.eventSockIFR.ifr_name,
            strerror(errno));
    }
    else
    {
      DBGV("new NIC time %ld.%09d\n",
           ts.time.seconds,
           ts.time.nanoseconds);
      syncSystemWithNIC(ptpClock);
    }
    break;
  }
  default:
    ERROR("unsupported selection of time source\n");
    break;
  }
}

void adjTime(Integer32 adj, TimeInternal *offset, PtpClock *ptpClock)
{
  switch(ptpClock->runTimeOpts.time)
  {
  case TIME_SYSTEM_LINUX_HW:
  case TIME_SYSTEM_LINUX_SW:
  case TIME_SYSTEM_ASSISTED:
  case TIME_SYSTEM: {
    struct timex t;
    static Boolean maxAdjValid;
    static long maxAdj;
    static long minTick, maxTick;
    static long userHZ;
    static long tickRes; /* USER_HZ * 1000 [ppb] */
    long tickAdj;
    long freqAdj;
    int res;

    if (!maxAdjValid) {
        userHZ = sysconf(_SC_CLK_TCK);
        t.modes = 0;
        adjtimex(&t);
        maxAdj = t.tolerance / ((1<<16)/1000);
        tickRes = userHZ * 1000;
        /* limits from the adjtimex command man page; could be determined via binary search */
        minTick = (900000 - 1000000) / userHZ;
        maxTick = (1100000 - 1000000) / userHZ;
        maxAdjValid = TRUE;
    }

    /*
     * The Linux man page for the adjtimex() system call does not
     * describe limits for frequency. The more recent man page for
     * the adjtimex command on RH5 does and says that
     * -tolerance <= frequency <= tolerance
     * which was confirmed by trying out values just outside that interval.
     *
     * Note that this contradicts the comments for struct timex which say
     * that freq and tolerance have different units (scaled ppm vs ppm).
     *
     * We follow the actual implementation on Linux 2.6.22 and do the
     * range check after scaling.
     */

    t.modes = MOD_FREQUENCY|MOD_CLKB;
    /*
     * @todo
     * Where is the official documentation for "scaled  ppm"?
     * Should this perhaps be adj * (1<<16) / 1000 (more accurate
     * than multiplying by ((1<<16)/1000) == 65)?
     */

    /*
     * 1 t.tick = 1 e-6 s * USER_HZ 1/s = 1 USER_HZ * 1000 ppb
     *
     * Large values of adj can be turned into t.tick adjustments:
     * tickAdj t.tick = adj ppb / ( USER_HZ * 1000 ppb )
     *
     * Round this so that the error is as small is possible,
     * because we need to fit that into t.freq.
     */
    freqAdj = adj;
    tickAdj = 0;
    if(freqAdj > maxAdj)
    {
      tickAdj = (adj - maxAdj + tickRes - 1) / tickRes;
      if(tickAdj > maxTick)
        tickAdj = maxTick;
      freqAdj = adj - tickAdj * tickRes;
    }
    else if(freqAdj < -maxAdj)
    {
      tickAdj = -((-adj - maxAdj + tickRes - 1) / tickRes);
      if(tickAdj < minTick)
        tickAdj = minTick;
      freqAdj = adj - tickAdj * tickRes;
    }
    if(freqAdj > maxAdj)
      freqAdj = maxAdj;
    else if(freqAdj < -maxAdj)
      freqAdj = -maxAdj;

    t.freq = freqAdj * ((1<<16)/1000);
    t.tick = tickAdj + 1000000 / userHZ;
    ptpClock->adj = tickAdj * tickRes + freqAdj;

    INFO("requested adj %d ppb => adjust system frequency by %d scaled ppm (%d ppb) + %ld us/tick (%d ppb) = adj %d ppb (freq limit %ld/%ld ppm, tick limit %ld/%ld us*USER_HZ)\n",
         adj,
         t.freq, freqAdj,
         t.tick - 1000000 / userHZ, tickAdj * tickRes,
         ptpClock->adj,
         -maxAdj, maxAdj,
         minTick, maxTick);

    res = adjtimex(&t);
    switch (res) {
    case -1:
        ERROR("adjtimex(freq = %d) failed: %s\n",
              t.freq, strerror(errno));
        break;
    case TIME_OK:
        INFO("  -> TIME_OK\n");
        break;
    case TIME_INS:
        ERROR("adjtimex -> insert leap second?!\n");
        break;
    case TIME_DEL:
        ERROR("adjtimex -> delete leap second?!\n");
        break;
    case TIME_OOP:
        ERROR("adjtimex -> leap second in progress?!\n");
        break;
    case TIME_WAIT:
        ERROR("adjtimex -> leap second has occurred?!\n");
        break;
    case TIME_BAD:
        ERROR("adjtimex -> time bad\n");
        break;
    default:
        ERROR("adjtimex -> unknown result %d\n", res);
        break;
    }
    break;
  }
  case TIME_BOTH:
  case TIME_NIC: {
    if(offset)
    {
#if 0
      struct E1000_TSYNC_SYSTIME_ARGU ts;
      memset(&ts, 0, sizeof(ts));
      // always store positive seconds/nanoseconds
      ts.negative_offset = (offset->seconds < 0 || offset->nanoseconds < 0) ? -1 : 1;
      ts.time.seconds = ts.negative_offset * offset->seconds;
      ts.time.nanoseconds = ts.negative_offset * offset->nanoseconds;
      // invert the sign: if offset is positive, we need to substract it and vice versa
      ts.negative_offset *= -1;
      DBGV("adjust NIC time by offset %s%lu.%09d (adj %d)\n",
           ts.negative_offset < 0 ? "-" : "",
           ts.time.seconds, ts.time.nanoseconds,
           adj);
      ptpClock->netPath.eventSockIFR.ifr_data = (void *)&ts;
      if (ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_SYSTIME_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
        ERROR("could not modify E1000 hardware time on %s: %s\n",
              ptpClock->netPath.eventSockIFR.ifr_name,
              strerror(errno));
      }
      else
        syncSystemWithNIC(ptpClock);
#else
      // adjust NIC frequency
      struct E1000_TSYNC_ADJTIME_ARGU ts;
      memset(&ts, 0, sizeof(ts));
      ts.adj = (long long)adj;
      if(ptpClock->nic_instead_of_system)
        ts.adj = -ts.adj;
      ts.set_adj = TRUE;
      ptpClock->netPath.eventSockIFR.ifr_data = (void *)&ts;
      DBGV("adjust NIC frequency by %d ppb\n", ts.adj);
      ptpClock->adj = ts.adj;
      if (ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_ADJTIME_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
        ERROR("could not modify E1000 hardware frequency on %s: %s\n",
              ptpClock->netPath.eventSockIFR.ifr_name,
              strerror(errno));
      }
      else
        syncSystemWithNIC(ptpClock);
#endif
    }
    else
      syncSystemWithNIC(ptpClock);
    break;
  }
  default:
    ERROR("unsupported selection of time source\n");
    break;
  }
}

void adjTimeOffset(TimeInternal *offset, PtpClock *ptpClock)
{
  switch(ptpClock->runTimeOpts.time)
  {
  case TIME_BOTH:
  case TIME_NIC: {
    struct E1000_TSYNC_SYSTIME_ARGU ts;
    memset(&ts, 0, sizeof(ts));
    // always store positive seconds/nanoseconds
    ts.negative_offset = (offset->seconds < 0 || offset->nanoseconds < 0) ? -1 : 1;
    ts.time.seconds = ts.negative_offset * offset->seconds;
    ts.time.nanoseconds = ts.negative_offset * offset->nanoseconds;

    // invert the sign: if offset is positive, we need to substract it and vice versa;
    // when in nic_instead_of_system the logic is already inverted
    if (!ptpClock->nic_instead_of_system)
      ts.negative_offset *= -1;

    DBGV("adjust NIC time by offset %s%lu.%09d\n",
         ts.negative_offset < 0 ? "-" : "",
         ts.time.seconds, ts.time.nanoseconds);
    ptpClock->netPath.eventSockIFR.ifr_data = (void *)&ts;
    if (ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_SYSTIME_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
        ERROR("could not modify E1000 hardware time on %s: %s\n",
              ptpClock->netPath.eventSockIFR.ifr_name,
              strerror(errno));
    }
    else
      syncSystemWithNIC(ptpClock);
    break;
  }
  default: {
    TimeInternal timeTmp;

    getTime(&timeTmp, ptpClock);
    subTime(&timeTmp, &timeTmp, offset);
    setTime(&timeTmp, ptpClock);
  }
  }
}

static void getTimeStamps(PtpClock *ptpClock)
{
  struct E1000_TSYNC_READTS_ARGU ts;

  ptpClock->netPath.eventSockIFR.ifr_data = (void *)&ts;
  memset(&ts, 0, sizeof(ts));
  ts.withSystemTime = (ptpClock->runTimeOpts.time == TIME_SYSTEM_ASSISTED);
  if (ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_READTS_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
    ERROR("could not read E1000 hardware time stamps on %s: %s\n",
          ptpClock->netPath.eventSockIFR.ifr_name,
          strerror(errno));
    return;
  }

  DBGV("rx %s, tx %s\n",
       ts.rx_valid ? "valid" : "invalid",
       ts.tx_valid ? "valid" : "invalid");

  if(ts.rx_valid)
  {
    int newIndex;

    if(nextFreeRecv == RECV_ARRAY_SIZE)
    {
      newIndex = 0;
      nextFreeRecv = 1;
      oldestRecv = 2;
    }
    else
    {
      newIndex = nextFreeRecv;
      nextFreeRecv++;
      if(oldestRecv && nextFreeRecv == oldestRecv)
        ++oldestRecv;
    }

    if(oldestRecv >= RECV_ARRAY_SIZE)
      oldestRecv = 0;

    DBGV("new entry %d, oldest %d, next free %d\n", newIndex, oldestRecv, nextFreeRecv);

    lastRecvTimes[newIndex].recvTimeStamp.seconds = ts.withSystemTime ? ts.rx_sys.seconds : ts.rx.seconds;
    lastRecvTimes[newIndex].recvTimeStamp.nanoseconds = ts.withSystemTime ? ts.rx_sys.nanoseconds : ts.rx.nanoseconds;
    lastRecvTimes[newIndex].sequenceId = ts.sourceSequenceId;
    memcpy(lastRecvTimes[newIndex].sourceUuid, ts.sourceIdentity, sizeof(ts.sourceIdentity));

    DBGV("rx %d: time %lu.%09u (%lu.%09u), sequence %u, uuid %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
         newIndex,
         lastRecvTimes[newIndex].recvTimeStamp.seconds,
         lastRecvTimes[newIndex].recvTimeStamp.nanoseconds,
         ts.withSystemTime ? ts.rx.seconds : 0,
         ts.withSystemTime ? ts.rx.nanoseconds : 0,
         lastRecvTimes[newIndex].sequenceId,
         ts.sourceIdentity[0],
         ts.sourceIdentity[1],
         ts.sourceIdentity[2],
         ts.sourceIdentity[3],
         ts.sourceIdentity[4],
         ts.sourceIdentity[5]);
  }

  if(ts.tx_valid)
  {
    lastSendTime.seconds = ts.withSystemTime ? ts.tx_sys.seconds : ts.tx.seconds;
    lastSendTime.nanoseconds = ts.withSystemTime ? ts.tx_sys.nanoseconds : ts.tx.nanoseconds;

    DBGV("tx time %lu.%09u (%lu.%09u)\n",
         lastSendTime.seconds,
         lastSendTime.nanoseconds,
         ts.withSystemTime ? ts.tx.seconds : 0,
         ts.withSystemTime ? ts.tx.nanoseconds : 0);
  }
}

Boolean getSendTime(TimeInternal *sendTimeStamp,
                    PtpClock *ptpClock)
{
  /* check for new time stamps */
  getTimeStamps(ptpClock);

  if(lastSendTime.seconds || lastSendTime.nanoseconds)
  {
    *sendTimeStamp = lastSendTime;
    lastSendTime.seconds = 0;
    lastSendTime.nanoseconds = 0;
    return TRUE;
  }
  else
    return FALSE;
}

/**
 * helper function for getReceiveTime() which searches for time stamp
 * in lastRecvTimes[leftIndex, rightIndex[
 */
static Boolean getReceiveTimeFromArray(TimeInternal *recvTimeStamp,
                                       Octet sourceUuid[PTP_UUID_LENGTH],
                                       UInteger16 sequenceId,
                                       int leftIndex, int rightIndex)
{
  int i;

  for(i = leftIndex; i < rightIndex; i++)
  {
    if(!memcmp(lastRecvTimes[i].sourceUuid, sourceUuid, sizeof(lastRecvTimes[i].sourceUuid)) &&
       lastRecvTimes[i].sequenceId == sequenceId)
    {
      DBGV("found rx index %d: time %lu.%09u, sequence %u, uuid %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
           i,
           lastRecvTimes[i].recvTimeStamp.seconds,
           lastRecvTimes[i].recvTimeStamp.nanoseconds,
           lastRecvTimes[i].sequenceId,
           lastRecvTimes[i].sourceUuid[0],
           lastRecvTimes[i].sourceUuid[1],
           lastRecvTimes[i].sourceUuid[2],
           lastRecvTimes[i].sourceUuid[3],
           lastRecvTimes[i].sourceUuid[4],
           lastRecvTimes[i].sourceUuid[5]);
      *recvTimeStamp = lastRecvTimes[i].recvTimeStamp;
      // invalidate entry to prevent accidental reuse (happened when slaves were
      // restarted quickly while the master still had their old sequence IDs in the array)
      memset(&lastRecvTimes[i], 0, sizeof(lastRecvTimes[i]));
      return TRUE;
    }
  }
  return FALSE;
}

Boolean getReceiveTime(TimeInternal *recvTimeStamp,
                       Octet sourceUuid[PTP_UUID_LENGTH],
                       UInteger16 sequenceId,
                       PtpClock *ptpClock)
{
  /* check for new time stamps */
  getTimeStamps(ptpClock);

  if(oldestRecv <= nextFreeRecv)
    return getReceiveTimeFromArray(recvTimeStamp, sourceUuid, sequenceId, oldestRecv, nextFreeRecv);
  else
  {
    if(getReceiveTimeFromArray(recvTimeStamp, sourceUuid, sequenceId, oldestRecv, RECV_ARRAY_SIZE))
      return TRUE;
    else
      return getReceiveTimeFromArray(recvTimeStamp, sourceUuid, sequenceId, 0, nextFreeRecv);
  }
}

void timeNoActivity(PtpClock *ptpClock)
{
#ifdef PTPD_DBGV
  switch(ptpClock->runTimeOpts.time) {
  case TIME_NIC:
  case TIME_BOTH:
  case TIME_SYSTEM_ASSISTED:
  {
    TimeInternal now, ts, offset;
    struct E1000_TSYNC_COMPARETS_ARGU argu;
    int sign;

    getTime(&ts, ptpClock);
    timerNow(&now);
    subTime(&offset, &now, &ts);
    sign = (offset.seconds < 0 || offset.nanoseconds < 0) ? -1 : 1;
    DBGV("system time %d.%09d, NIC time %d.%09d => system time - NIC time = %s%d.%09d\n",
         now.seconds, now.nanoseconds,
         ts.seconds, ts.nanoseconds,
         sign < 0 ? "-" : "",
         sign * offset.seconds, sign * offset.nanoseconds);

    ptpClock->netPath.eventSockIFR.ifr_data = (void *)&argu;
    memset(&ts, 0, sizeof(ts));
    if (ioctl(ptpClock->netPath.eventSock, E1000_TSYNC_COMPARETS_IOCTL, &ptpClock->netPath.eventSockIFR) < 0) {
      ERROR("could not correlate E1000 hardware and system time on %s: %s\n",
            ptpClock->netPath.eventSockIFR.ifr_name,
            strerror(errno));
      return;
    }

    now.seconds = argu.systemToNICSign * argu.systemToNIC.seconds;
    now.nanoseconds = argu.systemToNICSign * argu.systemToNIC.nanoseconds;
    ts.seconds = argu.NICToSystemSign * argu.NICToSystem.seconds;
    ts.nanoseconds = argu.NICToSystemSign * argu.NICToSystem.nanoseconds;
    subTime(&offset, &now, &ts);
    offset.seconds /= 2;
    offset.nanoseconds /= 2;
    DBGV("delay system to NIC %s%ld.%09d/NIC to system %s%ld.%09d => system - NIC time = %d.%09d\n",
         argu.systemToNICSign > 0 ? "" : argu.systemToNICSign < 0 ? "-" : "?",
         argu.systemToNIC.seconds, argu.systemToNIC.nanoseconds,
         argu.NICToSystemSign > 0 ? "" : argu.NICToSystemSign < 0 ? "-" : "?",
         argu.NICToSystem.seconds, argu.NICToSystem.nanoseconds,
         offset.seconds, offset.nanoseconds);
    break;
  }
  }
#endif
  syncSystemWithNIC(ptpClock);
}

void timeToState(UInteger8 state, PtpClock *ptpClock)
{
  if(ptpClock->runTimeOpts.time > TIME_SYSTEM &&
     state != ptpClock->port_state)
  {
    if(state == PTP_MASTER)
      /* only master listens for Delay_Req... */
      selectNICTimeMode(FALSE, ptpClock);
    else if(ptpClock->port_state == PTP_MASTER)
      /** ... and only while he still is master */
      selectNICTimeMode(TRUE, ptpClock);

    timeBothClock.port_state = state;
  }
}
