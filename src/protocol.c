/* protocol.c */

#include "ptpd.h"

Boolean doInit(PtpClock*);
void doState(PtpClock*);
void toState(UInteger8,PtpClock*);

void handle(PtpClock*);
void handleSync(MsgHeader*,Octet*,ssize_t,TimeInternal*,Boolean,Boolean,PtpClock*);
void handleFollowUp(MsgHeader*,Octet*,ssize_t,Boolean,PtpClock*);
void handleDelayReq(MsgHeader*,Octet*,ssize_t,TimeInternal*,Boolean,Boolean,PtpClock*);
void handleDelayResp(MsgHeader*,Octet*,ssize_t,Boolean,PtpClock*);
void handleManagement(MsgHeader*,Octet*,ssize_t,Boolean,PtpClock*);

void issueSync(PtpClock*);
void issueFollowup(TimeInternal*,PtpClock*);
void issueDelayReq(PtpClock*);
void issueDelayResp(TimeInternal*,MsgHeader*,PtpClock*);
void issueManagement(MsgHeader*,MsgManagement*,PtpClock*);

MsgSync * addForeign(Octet*,MsgHeader*,PtpClock*);


/* loop forever. doState() has a switch for the actions and events to be
   checked for 'port_state'. the actions and events may or may not change
   'port_state' by calling toState(), but once they are done we loop around
   again and perform the actions required for the new 'port_state'. */
void protocol(PtpClock *ptpClock)
{
  DBG("event POWERUP\n");
  
  toState(PTP_INITIALIZING, ptpClock);
  
  for(;;)
  {
    if(ptpClock->port_state != PTP_INITIALIZING)
      doState(ptpClock);
    else if(!doInit(ptpClock))
      return;
    
    if(ptpClock->message_activity)
      DBGV("activity\n");
    else
    {
      DBGV("no activity\n");
      timeNoActivity(ptpClock);
    }
  }
}

Boolean doInit(PtpClock *ptpClock)
{
  DBG("manufacturerIdentity: %s\n", MANUFACTURER_ID);
  
  /* initialize networking */
  netShutdown(ptpClock);
  if(!netInit(ptpClock))
  {
    ERROR("failed to initialize network\n");
    toState(PTP_FAULTY, ptpClock);
    return FALSE;
  }

  /* initialize timing, may fail e.g. if timer depends on hardware */
  if(!initTime(ptpClock))
  {
    ERROR("failed to initialize timing\n");
    toState(PTP_FAULTY, ptpClock);
    return FALSE;
  }

  switch (ptpClock->runTimeOpts.time) {
  case TIME_SYSTEM:
  case TIME_SYSTEM_LINUX_HW:
  case TIME_SYSTEM_LINUX_SW:
      /*
       * send time stamp will be returned to socket when available,
       * either via IP_MULTICAST_LOOP or SIOCSHWTSTAMP + error queue
       */
      ptpClock->delayedTiming = FALSE;
      break;
  default:
      /* ask for time stamp shortly after sending */
      ptpClock->delayedTiming = TRUE;
      break;
  }

  /* initialize other stuff */
  initData(ptpClock);
  initTimer();
  initClock(ptpClock);
  m1(ptpClock);
  msgPackHeader(ptpClock->msgObuf, ptpClock);
  
  DBG("sync message interval: %d\n", PTP_SYNC_INTERVAL_TIMEOUT(ptpClock->sync_interval));
  DBG("clock identifier: %s\n", ptpClock->clock_identifier);
  DBG("256*log2(clock variance): %d\n", ptpClock->clock_variance);
  DBG("clock stratum: %d\n", ptpClock->clock_stratum);
  DBG("clock preferred?: %s\n", ptpClock->preferred?"yes":"no");
  DBG("bound interface name: %s\n", ptpClock->runTimeOpts.ifaceName);
  DBG("communication technology: %d\n", ptpClock->port_communication_technology);
  DBG("uuid: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
    ptpClock->port_uuid_field[0], ptpClock->port_uuid_field[1], ptpClock->port_uuid_field[2],
    ptpClock->port_uuid_field[3], ptpClock->port_uuid_field[4], ptpClock->port_uuid_field[5]);
  DBG("PTP subdomain name: %s\n", ptpClock->subdomain_name);
  DBG("subdomain address: %hhx.%hhx.%hhx.%hhx\n",
    ptpClock->subdomain_address[0], ptpClock->subdomain_address[1],
    ptpClock->subdomain_address[2], ptpClock->subdomain_address[3]);
  DBG("event port address: %hhx %hhx\n",
    ptpClock->event_port_address[0], ptpClock->event_port_address[1]);
  DBG("general port address: %hhx %hhx\n",
    ptpClock->general_port_address[0], ptpClock->general_port_address[1]);
  
  toState(PTP_LISTENING, ptpClock);
  return TRUE;
}

/* handle actions and events for 'port_state' */
void doState(PtpClock *ptpClock)
{
  UInteger8 state;
  
  ptpClock->message_activity = FALSE;
  
  switch(ptpClock->port_state)
  {
  case PTP_LISTENING:
  case PTP_PASSIVE:
  case PTP_SLAVE:
  case PTP_MASTER:
    if(ptpClock->record_update)
    {
      ptpClock->record_update = FALSE;
      state = bmc(ptpClock->foreign, ptpClock);
      if(state != ptpClock->port_state)
        toState(state, ptpClock);
    }
    break;
    
  default:
    break;
  }
  
  switch(ptpClock->port_state)
  {
  case PTP_FAULTY:
    /* imaginary troubleshooting */
    
    DBG("event FAULT_CLEARED\n");
    toState(PTP_INITIALIZING, ptpClock);
    return;
    
  case PTP_LISTENING:
  case PTP_PASSIVE:
  case PTP_UNCALIBRATED:
  case PTP_SLAVE:
    handle(ptpClock);
    
    if(timerExpired(SYNC_RECEIPT_TIMER, ptpClock->itimer))
    {
      DBG("event SYNC_RECEIPT_TIMEOUT_EXPIRES\n");
      ptpClock->number_foreign_records = 0;
      ptpClock->foreign_record_i = 0;
      if(!ptpClock->runTimeOpts.slaveOnly && ptpClock->clock_stratum != 255)
      {
        m1(ptpClock);
        toState(PTP_MASTER, ptpClock);
      }
      else if(ptpClock->port_state != PTP_LISTENING)
        toState(PTP_LISTENING, ptpClock);
    }
    
    break;
    
  case PTP_MASTER:
    if(timerExpired(SYNC_INTERVAL_TIMER, ptpClock->itimer))
    {
      DBGV("event SYNC_INTERVAL_TIMEOUT_EXPIRES\n");
      issueSync(ptpClock);
    }
    
    handle(ptpClock);
    
    if(ptpClock->runTimeOpts.slaveOnly || ptpClock->clock_stratum == 255)
      toState(PTP_LISTENING, ptpClock);
    
    break;
    
  case PTP_DISABLED:
    handle(ptpClock);
    break;
    
  default:
    DBG("do unrecognized state\n");
    break;
  }
}

/* perform actions required when leaving 'port_state' and entering 'state' */
void toState(UInteger8 state, PtpClock *ptpClock)
{
  ptpClock->message_activity = TRUE;

  /* leaving state tasks */
  switch(ptpClock->port_state)
  {
  case PTP_MASTER:
    timerStop(SYNC_INTERVAL_TIMER, ptpClock->itimer);
    timerStart(SYNC_RECEIPT_TIMER, PTP_SYNC_RECEIPT_TIMEOUT(ptpClock->sync_interval), ptpClock->itimer);
    break;
    
  case PTP_SLAVE:
    initClock(ptpClock);
    break;
    
  default:
    break;
  }

  timeToState(state, ptpClock);
  
  /* entering state tasks */
  switch(state)
  {
  case PTP_INITIALIZING:
    DBG("state PTP_INITIALIZING\n");
    timerStop(SYNC_RECEIPT_TIMER, ptpClock->itimer);
    
    ptpClock->port_state = PTP_INITIALIZING;
    break;
    
  case PTP_FAULTY:
    DBG("state PTP_FAULTY\n");
    timerStop(SYNC_RECEIPT_TIMER, ptpClock->itimer);
    
    ptpClock->port_state = PTP_FAULTY;
    break;
    
  case PTP_DISABLED:
    DBG("state change to PTP_DISABLED\n");
    timerStop(SYNC_RECEIPT_TIMER, ptpClock->itimer);
    
    ptpClock->port_state = PTP_DISABLED;
    break;
    
  case PTP_LISTENING:
    DBG("state PTP_LISTENING\n");
    
    timerStart(SYNC_RECEIPT_TIMER, PTP_SYNC_RECEIPT_TIMEOUT(ptpClock->sync_interval), ptpClock->itimer);
    
    ptpClock->port_state = PTP_LISTENING;
    break;
    
  case PTP_MASTER:
    DBG("state PTP_MASTER\n");
    
    if(ptpClock->port_state != PTP_PRE_MASTER)
      timerStart(SYNC_INTERVAL_TIMER, PTP_SYNC_INTERVAL_TIMEOUT(ptpClock->sync_interval), ptpClock->itimer);
    
    timerStop(SYNC_RECEIPT_TIMER, ptpClock->itimer);
    
    ptpClock->port_state = PTP_MASTER;
    break;
    
  case PTP_PASSIVE:
    DBG("state PTP_PASSIVE\n");
    ptpClock->port_state = PTP_PASSIVE;
    break;
    
  case PTP_UNCALIBRATED:
    DBG("state PTP_UNCALIBRATED\n");
    ptpClock->port_state = PTP_UNCALIBRATED;
    break;
    
  case PTP_SLAVE:
    DBG("state PTP_PTP_SLAVE\n");
    
    initClock(ptpClock);
    
    /* R is chosen to allow a few syncs before we first get a one-way delay estimate */
    /* this is to allow the offset filter to fill for an accurate initial clock reset */
    ptpClock->Q = 0;
    ptpClock->R = getRand(&ptpClock->random_seed)%4 + 4;
    DBG("Q = %d, R = %d\n", ptpClock->Q, ptpClock->R);
    
    ptpClock->waitingForFollow = FALSE;
    ptpClock->delay_req_send_time.seconds = 0;
    ptpClock->delay_req_send_time.nanoseconds = 0;
    ptpClock->delay_req_receive_time.seconds = 0;
    ptpClock->delay_req_receive_time.nanoseconds = 0;
    
    timerStart(SYNC_RECEIPT_TIMER, PTP_SYNC_RECEIPT_TIMEOUT(ptpClock->sync_interval), ptpClock->itimer);
    
    ptpClock->port_state = PTP_SLAVE;
    break;
    
  default:
    DBG("to unrecognized state\n");
    break;
  }
  
  if(ptpClock->runTimeOpts.displayStats)
    displayStats(ptpClock);
}

/* check and handle received messages */
void handle(PtpClock *ptpClock)
{
  int ret;
  ssize_t length;
  Boolean isFromSelf;
  Boolean isEvent;
  Boolean badTime = FALSE;
  TimeInternal time = { 0, 0 };
  
  if(!ptpClock->message_activity)
  {
    ret = netSelect(0, ptpClock);
    if(ret < 0)
    {
      PERROR("failed to poll sockets");
      toState(PTP_FAULTY, ptpClock);
      return;
    }
    else if(!ret)
    {
      DBGV("handle: nothing\n");
      return;
    }
    /* else length > 0 */
  }
  
  DBGV("handle: something\n");

  isEvent = TRUE;
  length = netRecvEvent(ptpClock->msgIbuf,
                        ptpClock->delayedTiming ? NULL : &time,
                        ptpClock);
  if(length < 0)
  {
    PERROR("failed to receive on the event socket");
    toState(PTP_FAULTY, ptpClock);
    return;
  }
  else if(!length)
  {
    isEvent = FALSE;
    length = netRecvGeneral(ptpClock->msgIbuf, ptpClock);
    if(length < 0)
    {
      PERROR("failed to receive on the general socket");
      toState(PTP_FAULTY, ptpClock);
      return;
    }
    else if(!length)
      return;
  }
  
  ptpClock->message_activity = TRUE;
  
  if(!msgPeek(ptpClock->msgIbuf, length))
    return;
  
  if(length < HEADER_LENGTH)
  {
    ERROR("message shorter than header length\n");
    toState(PTP_FAULTY, ptpClock);
    return;
  }
  
  msgUnpackHeader(ptpClock->msgIbuf, &ptpClock->msgTmpHeader);

  if(isEvent && ptpClock->delayedTiming)
  {
    /* query hardware for matching receive time stamp */
    if(!getReceiveTime(&time, ptpClock->msgTmpHeader.sourceUuid, ptpClock->msgTmpHeader.sequenceId, ptpClock))
    {
      /*
       * Incoming packets without hardware time stamp cannot be ignored outright because
       * a master might only be able to time stamp DelayReq packets; ignoring the Sync
       * packets from another, better clock would break the clock selection protocol.
       * Therefore set system time as fallback and decide below what to do.
       */
      DBGV("*** message with no time stamp ***\n");
      getTime(&time, ptpClock);
      badTime = TRUE;
    }
  }
  
  DBGV("%s Receipt of Message\n"
    "   version %d\n"
    "   type %d\n"
    "   uuid %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n"
    "   sequence %d\n"
    "   time %us %dns\n",
    isEvent ? "event" : "control",
    ptpClock->msgTmpHeader.versionPTP,
    ptpClock->msgTmpHeader.control,
    ptpClock->msgTmpHeader.sourceUuid[0], ptpClock->msgTmpHeader.sourceUuid[1],
    ptpClock->msgTmpHeader.sourceUuid[2], ptpClock->msgTmpHeader.sourceUuid[3],
    ptpClock->msgTmpHeader.sourceUuid[4], ptpClock->msgTmpHeader.sourceUuid[5],
    ptpClock->msgTmpHeader.sequenceId,
    time.seconds, time.nanoseconds);
  
  if(ptpClock->msgTmpHeader.versionPTP != VERSION_PTP)
  {
    DBGV("ignore version %d message\n", ptpClock->msgTmpHeader.versionPTP);
    return;
  }
  
  if( memcmp(ptpClock->msgTmpHeader.subdomain, ptpClock->subdomain_name,
    PTP_SUBDOMAIN_NAME_LENGTH) )
  {
    DBGV("ignore message from subdomain %s\n", ptpClock->msgTmpHeader.subdomain);
    return;
  }
  
  isFromSelf = ptpClock->msgTmpHeader.sourceCommunicationTechnology == ptpClock->port_communication_technology
    && ptpClock->msgTmpHeader.sourcePortId == ptpClock->port_id_field
    && !memcmp(ptpClock->msgTmpHeader.sourceUuid, ptpClock->port_uuid_field, PTP_UUID_LENGTH);
  
  /* subtract the inbound latency adjustment if it is not a loop back and the
     time stamp seems reasonable */
  if(!isFromSelf && time.seconds > 0)
    subTime(&time, &time, &ptpClock->runTimeOpts.inboundLatency);
  
  switch(ptpClock->msgTmpHeader.control)
  {
  case PTP_SYNC_MESSAGE:
    handleSync(&ptpClock->msgTmpHeader, ptpClock->msgIbuf, length, &time, badTime, isFromSelf, ptpClock);
    break;
    
  case PTP_FOLLOWUP_MESSAGE:
    handleFollowUp(&ptpClock->msgTmpHeader, ptpClock->msgIbuf, length, isFromSelf, ptpClock);
    break;
    
  case PTP_DELAY_REQ_MESSAGE:
    handleDelayReq(&ptpClock->msgTmpHeader, ptpClock->msgIbuf, length, &time, badTime, isFromSelf, ptpClock);
    break;
    
  case PTP_DELAY_RESP_MESSAGE:
    handleDelayResp(&ptpClock->msgTmpHeader, ptpClock->msgIbuf, length, isFromSelf, ptpClock);
    break;
    
  case PTP_MANAGEMENT_MESSAGE:
    handleManagement(&ptpClock->msgTmpHeader, ptpClock->msgIbuf, length, isFromSelf, ptpClock);
    break;
    
   default:
    DBG("handle: unrecognized message\n");
    break;
  }
}

void handleSync(MsgHeader *header, Octet *msgIbuf, ssize_t length, TimeInternal *time, Boolean badTime, Boolean isFromSelf, PtpClock *ptpClock)
{
  MsgSync *sync;
  TimeInternal originTimestamp;
  
  if(length < SYNC_PACKET_LENGTH)
  {
    ERROR("short sync message\n");
    toState(PTP_FAULTY, ptpClock);
    return;
  }
  
  switch(ptpClock->port_state)
  {
  case PTP_FAULTY:
  case PTP_INITIALIZING:
  case PTP_DISABLED:
    DBGV("handleSync: disreguard\n");
    return;
    
  case PTP_UNCALIBRATED:
  case PTP_SLAVE:
    if(isFromSelf)
    {
      DBG("handleSync: ignore from self\n");
      return;
    }
    
    if(getFlag(header->flags, PTP_SYNC_BURST) && !ptpClock->burst_enabled)
      return;
    
    DBGV("handleSync: looking for uuid %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
      ptpClock->parent_uuid[0], ptpClock->parent_uuid[1], ptpClock->parent_uuid[2],
      ptpClock->parent_uuid[3], ptpClock->parent_uuid[4], ptpClock->parent_uuid[5]);
    
    if( header->sequenceId > ptpClock->parent_last_sync_sequence_number
      && header->sourceCommunicationTechnology == ptpClock->parent_communication_technology
      && header->sourcePortId == ptpClock->parent_port_id
      && !memcmp(header->sourceUuid, ptpClock->parent_uuid, PTP_UUID_LENGTH) )
    {
      /* addForeign() takes care of msgUnpackSync() */
      ptpClock->record_update = TRUE;
      sync = addForeign(ptpClock->msgIbuf, &ptpClock->msgTmpHeader, ptpClock);
      
      if(sync->syncInterval != ptpClock->sync_interval)
      {
        DBGV("message's sync interval is %d, but clock's is %d\n", sync->syncInterval, ptpClock->sync_interval);
        /* spec recommends handling a sync interval discrepancy as a fault */
      }
      
      /*
       * TODO: Sync packets without hardware time stamp are rare, but might happen.
       * Need to decide what to do with the bad default time stamp, similar to handleDelayReq().
       */

      ptpClock->sync_receive_time.seconds = time->seconds;
      ptpClock->sync_receive_time.nanoseconds = time->nanoseconds;
      
      if(!getFlag(header->flags, PTP_ASSIST))
      {
        ptpClock->waitingForFollow = FALSE;
        
        toInternalTime(&originTimestamp, &sync->originTimestamp, &ptpClock->halfEpoch);
        updateOffset(&originTimestamp, &ptpClock->sync_receive_time,
          &ptpClock->ofm_filt, ptpClock);
        updateClock(ptpClock);
      }
      else
      {
        ptpClock->waitingForFollow = TRUE;
      }
      
      s1(header, sync, ptpClock);
      
      if(!(--ptpClock->R))
      {
        issueDelayReq(ptpClock);
        
        ptpClock->Q = 0;
        ptpClock->R = getRand(&ptpClock->random_seed)%(PTP_DELAY_REQ_INTERVAL - 2) + 2;
        DBG("Q = %d, R = %d\n", ptpClock->Q, ptpClock->R);
      }
      
      DBGV("SYNC_RECEIPT_TIMER reset\n");
      timerStart(SYNC_RECEIPT_TIMER, PTP_SYNC_RECEIPT_TIMEOUT(ptpClock->sync_interval), ptpClock->itimer);
    }
    else
    {
      DBGV("handleSync: unwanted\n");
    }
    
  case PTP_MASTER:
  default:
    if( header->sourceCommunicationTechnology == ptpClock->clock_communication_technology
      || header->sourceCommunicationTechnology == PTP_DEFAULT
      || ptpClock->clock_communication_technology == PTP_DEFAULT )
    {
      if(!isFromSelf)
      {
        ptpClock->record_update = TRUE;
        addForeign(ptpClock->msgIbuf, &ptpClock->msgTmpHeader, ptpClock);
      }
      else if(ptpClock->port_state == PTP_MASTER && ptpClock->clock_followup_capable)
      {
        addTime(time, time, &ptpClock->runTimeOpts.outboundLatency);
        issueFollowup(time, ptpClock);
      }
    }
    break;
  }
}

void handleFollowUp(MsgHeader *header, Octet *msgIbuf, ssize_t length, Boolean isFromSelf, PtpClock *ptpClock)
{
  MsgFollowUp *follow;
  TimeInternal preciseOriginTimestamp;
  
  if(length < FOLLOW_UP_PACKET_LENGTH)
  {
    ERROR("short folow up message\n");
    toState(PTP_FAULTY, ptpClock);
    return;
  }
  
  switch(ptpClock->port_state)
  {
  case PTP_SLAVE:
    if(isFromSelf)
    {
      DBG("handleFollowUp: ignore from self\n");
      return;
    }
    
    if(getFlag(header->flags, PTP_SYNC_BURST) && !ptpClock->burst_enabled)
      return;
    
    DBGV("handleFollowUp: looking for uuid %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
      ptpClock->parent_uuid[0], ptpClock->parent_uuid[1], ptpClock->parent_uuid[2],
      ptpClock->parent_uuid[3], ptpClock->parent_uuid[4], ptpClock->parent_uuid[5]);
    
    follow = &ptpClock->msgTmp.follow;
    msgUnpackFollowUp(ptpClock->msgIbuf, follow);
    
    if( ptpClock->waitingForFollow
      && follow->associatedSequenceId == ptpClock->parent_last_sync_sequence_number
      && header->sourceCommunicationTechnology == ptpClock->parent_communication_technology
      && header->sourcePortId == ptpClock->parent_port_id
      && !memcmp(header->sourceUuid, ptpClock->parent_uuid, PTP_UUID_LENGTH) )
    {
      ptpClock->waitingForFollow = FALSE;
      
      toInternalTime(&preciseOriginTimestamp, &follow->preciseOriginTimestamp, &ptpClock->halfEpoch);
      updateOffset(&preciseOriginTimestamp, &ptpClock->sync_receive_time,
        &ptpClock->ofm_filt, ptpClock);
      updateClock(ptpClock);
    }
    else
    {
      DBGV("handleFollowUp: unwanted\n");
    }
    break;
    
  default:
    DBGV("handleFollowUp: disreguard\n");
    return;
  }
}

void handleDelayReq(MsgHeader *header, Octet *msgIbuf, ssize_t length, TimeInternal *time, Boolean badTime, Boolean isFromSelf, PtpClock *ptpClock)
{
  if(length < DELAY_REQ_PACKET_LENGTH)
  {
    ERROR("short delay request message\n");
    toState(PTP_FAULTY, ptpClock);
    return;
  }
  
  switch(ptpClock->port_state)
  {
  case PTP_MASTER:
    if(isFromSelf)
    {
      DBG("handleDelayReq: ignore from self\n");
      return;
    }
    
    if( header->sourceCommunicationTechnology == ptpClock->clock_communication_technology
      || header->sourceCommunicationTechnology == PTP_DEFAULT
      || ptpClock->clock_communication_technology == PTP_DEFAULT )
    {
      if( badTime )
        NOTIFY("avoid inaccurate DelayResp because of bad time stamp\n");
      else
        issueDelayResp(time, &ptpClock->msgTmpHeader, ptpClock);
    }
    
    break;
    
  case PTP_SLAVE:
    if(isFromSelf)
    {
      DBG("handleDelayReq: self\n");
      
      ptpClock->delay_req_send_time.seconds = time->seconds;
      ptpClock->delay_req_send_time.nanoseconds = time->nanoseconds;
      
      addTime(&ptpClock->delay_req_send_time, &ptpClock->delay_req_send_time, &ptpClock->runTimeOpts.outboundLatency);
      
      if(ptpClock->delay_req_receive_time.seconds)
      {
        updateDelay(&ptpClock->delay_req_send_time, &ptpClock->delay_req_receive_time,
          &ptpClock->owd_filt, ptpClock);
        
        ptpClock->delay_req_send_time.seconds = 0;
        ptpClock->delay_req_send_time.nanoseconds = 0;
        ptpClock->delay_req_receive_time.seconds = 0;
        ptpClock->delay_req_receive_time.nanoseconds = 0;
      }
    }
    break;
    
  default:
    DBGV("handleDelayReq: disreguard\n");
    return;
  }
}

void handleDelayResp(MsgHeader *header, Octet *msgIbuf, ssize_t length, Boolean isFromSelf, PtpClock *ptpClock)
{
  MsgDelayResp *resp;
  
  if(length < DELAY_RESP_PACKET_LENGTH)
  {
    ERROR("short delay request message\n");
    toState(PTP_FAULTY, ptpClock);
    return;
  }
  
  switch(ptpClock->port_state)
  {
  case PTP_SLAVE:
    if(isFromSelf)
    {
      DBG("handleDelayResp: ignore from self\n");
      return;
    }
    
    resp = &ptpClock->msgTmp.resp;
    msgUnpackDelayResp(ptpClock->msgIbuf, resp);
    
    if( ptpClock->sentDelayReq
      && resp->requestingSourceSequenceId == ptpClock->sentDelayReqSequenceId
      && resp->requestingSourceCommunicationTechnology == ptpClock->port_communication_technology
      && resp->requestingSourcePortId == ptpClock->port_id_field
      && !memcmp(resp->requestingSourceUuid, ptpClock->port_uuid_field, PTP_UUID_LENGTH)
      && header->sourceCommunicationTechnology == ptpClock->parent_communication_technology
      && header->sourcePortId == ptpClock->parent_port_id
      && !memcmp(header->sourceUuid, ptpClock->parent_uuid, PTP_UUID_LENGTH) )
    {
      ptpClock->sentDelayReq = FALSE;
      
      toInternalTime(&ptpClock->delay_req_receive_time, &resp->delayReceiptTimestamp, &ptpClock->halfEpoch);
      
      if(ptpClock->delay_req_send_time.seconds)
      {
        updateDelay(&ptpClock->delay_req_send_time, &ptpClock->delay_req_receive_time,
          &ptpClock->owd_filt, ptpClock);
        
        ptpClock->delay_req_send_time.seconds = 0;
        ptpClock->delay_req_send_time.nanoseconds = 0;
        ptpClock->delay_req_receive_time.seconds = 0;
        ptpClock->delay_req_receive_time.nanoseconds = 0;
      }
    }
    else
    {
      DBGV("handleDelayResp: unwanted\n");
    }
    break;
    
  default:
    DBGV("handleDelayResp: disreguard\n");
    return;
  }
}

void handleManagement(MsgHeader *header, Octet *msgIbuf, ssize_t length, Boolean isFromSelf, PtpClock *ptpClock)
{
  MsgManagement *manage;
  
  UInteger8 state;
  
  if(ptpClock->port_state == PTP_INITIALIZING)
    return;
  
  manage = &ptpClock->msgTmp.manage;
  msgUnpackManagement(ptpClock->msgIbuf, manage);
  
  if( (manage->targetCommunicationTechnology == ptpClock->clock_communication_technology
    && !memcmp(manage->targetUuid, ptpClock->clock_uuid_field, PTP_UUID_LENGTH))
    || ((manage->targetCommunicationTechnology == PTP_DEFAULT
    || manage->targetCommunicationTechnology == ptpClock->clock_communication_technology)
    && !sum(manage->targetUuid, PTP_UUID_LENGTH)) )
  {
    switch(manage->managementMessageKey)
    {
    case PTP_MM_OBTAIN_IDENTITY:
    case PTP_MM_GET_DEFAULT_DATA_SET:
    case PTP_MM_GET_CURRENT_DATA_SET:
    case PTP_MM_GET_PARENT_DATA_SET:
    case PTP_MM_GET_PORT_DATA_SET:
    case PTP_MM_GET_GLOBAL_TIME_DATA_SET:
    case PTP_MM_GET_FOREIGN_DATA_SET:
      issueManagement(header, manage, ptpClock);
      break;
      
    default:
      ptpClock->record_update = TRUE;
      state = msgUnloadManagement(ptpClock->msgIbuf, manage, ptpClock);
      if(state != ptpClock->port_state)
        toState(state, ptpClock);
      break;
    }
  }
  else
  {
    DBG("handleManagement: unwanted\n");
  }
}

/* pack and send various messages */
void issueSync(PtpClock *ptpClock)
{
  TimeInternal internalTime;
  TimeRepresentation originTimestamp;
  
  ++ptpClock->last_sync_event_sequence_number;
  ptpClock->grandmaster_sequence_number = ptpClock->last_sync_event_sequence_number;

  /* try to predict outgoing time stamp */
  getTime(&internalTime, ptpClock);
  fromInternalTime(&internalTime, &originTimestamp, ptpClock->halfEpoch);
  msgPackSync(ptpClock->msgObuf, FALSE, TRUE, &originTimestamp, ptpClock);
  
  if(!netSendEvent(ptpClock->msgObuf, SYNC_PACKET_LENGTH,
                   ptpClock->delayedTiming ? &internalTime : NULL,
                   ptpClock))
    toState(PTP_FAULTY, ptpClock);
  else
  {
    DBGV("sent sync message\n");
    if(ptpClock->delayedTiming)
    {
      if (internalTime.seconds || internalTime.nanoseconds) {
        /* compensate with configurable latency, then tell client real time stamp */
        addTime(&internalTime, &internalTime, &ptpClock->runTimeOpts.outboundLatency);
        issueFollowup(&internalTime, ptpClock);
      } else {
        NOTIFY("WARNING: sync message without hardware time stamp, skipped followup\n");
      }
    }
  }
}

void issueFollowup(TimeInternal *time, PtpClock *ptpClock)
{
  TimeRepresentation preciseOriginTimestamp;
  
  ++ptpClock->last_general_event_sequence_number;
  
  fromInternalTime(time, &preciseOriginTimestamp, ptpClock->halfEpoch);
  msgPackFollowUp(ptpClock->msgObuf, ptpClock->last_sync_event_sequence_number, &preciseOriginTimestamp, ptpClock);
  
  if(!netSendGeneral(ptpClock->msgObuf, FOLLOW_UP_PACKET_LENGTH, ptpClock))
    toState(PTP_FAULTY, ptpClock);
  else
    DBGV("sent followup message\n");
}

void issueDelayReq(PtpClock *ptpClock)
{
  TimeInternal internalTime;
  TimeRepresentation originTimestamp;
  
  ptpClock->sentDelayReq = TRUE;
  ptpClock->sentDelayReqSequenceId = ++ptpClock->last_sync_event_sequence_number;

  /* try to predict outgoing time stamp */
  getTime(&internalTime, ptpClock);
  fromInternalTime(&internalTime, &originTimestamp, ptpClock->halfEpoch);
  msgPackDelayReq(ptpClock->msgObuf, FALSE, FALSE, &originTimestamp, ptpClock);
  
  if(!netSendEvent(ptpClock->msgObuf, DELAY_REQ_PACKET_LENGTH,
                   ptpClock->delayedTiming ? &internalTime : NULL,
                   ptpClock))
    toState(PTP_FAULTY, ptpClock);
  else
  {
    DBGV("sent delay request message\n");
    if(ptpClock->delayedTiming)
    {
      if (internalTime.seconds || internalTime.nanoseconds) {
        /* compensate with configurable latency, then store for later use */
        addTime(&internalTime, &internalTime, &ptpClock->runTimeOpts.outboundLatency);
        ptpClock->delay_req_send_time = internalTime;
      } else {
        NOTIFY("WARNING: delay request message without hardware time stamp, will skip response\n");
        ptpClock->sentDelayReq = FALSE;
      }
    }
  }
}

void issueDelayResp(TimeInternal *time, MsgHeader *header, PtpClock *ptpClock)
{
  TimeRepresentation delayReceiptTimestamp;
  
  ++ptpClock->last_general_event_sequence_number;

  fromInternalTime(time, &delayReceiptTimestamp, ptpClock->halfEpoch);
  msgPackDelayResp(ptpClock->msgObuf, header, &delayReceiptTimestamp, ptpClock);
  
  if(!netSendGeneral(ptpClock->msgObuf, DELAY_RESP_PACKET_LENGTH, ptpClock))
    toState(PTP_FAULTY, ptpClock);
  else
    DBGV("sent delay response message\n");
}

void issueManagement(MsgHeader *header, MsgManagement *manage, PtpClock *ptpClock)
{
  UInteger16 length;
  
  ++ptpClock->last_general_event_sequence_number;
  
  if(!(length = msgPackManagementResponse(ptpClock->msgObuf, header, manage, ptpClock)))
    return;
  
  if(!netSendGeneral(ptpClock->msgObuf, length, ptpClock))
    toState(PTP_FAULTY, ptpClock);
  else
    DBGV("sent management message\n");
}

/* add or update an entry in the foreign master data set */
MsgSync * addForeign(Octet *buf, MsgHeader *header, PtpClock *ptpClock)
{
  int i, j;
  Boolean found = FALSE;
  
  DBGV("updateForeign\n");
  
  j = ptpClock->foreign_record_best;
  for(i = 0; i < ptpClock->number_foreign_records; ++i)
  {
    if(header->sourceCommunicationTechnology == ptpClock->foreign[j].foreign_master_communication_technology
      && header->sourcePortId == ptpClock->foreign[j].foreign_master_port_id
      && !memcmp(header->sourceUuid, ptpClock->foreign[j].foreign_master_uuid, PTP_UUID_LENGTH))
    {
      ++ptpClock->foreign[j].foreign_master_syncs;
      found = TRUE;
      DBGV("updateForeign: update record %d\n", j);
      break;
    }
    
    j = (j + 1)%ptpClock->number_foreign_records;
  }
  
  if(!found)
  {
    if(ptpClock->number_foreign_records < ptpClock->max_foreign_records)
      ++ptpClock->number_foreign_records;
    
    j = ptpClock->foreign_record_i;
    
    ptpClock->foreign[j].foreign_master_communication_technology =
      header->sourceCommunicationTechnology;
    ptpClock->foreign[j].foreign_master_port_id =
      header->sourcePortId;
    memcpy(ptpClock->foreign[j].foreign_master_uuid,
      header->sourceUuid, PTP_UUID_LENGTH);
    
    DBG("updateForeign: new record (%d,%d) %d %d %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n",
      ptpClock->foreign_record_i, ptpClock->number_foreign_records,
      ptpClock->foreign[j].foreign_master_communication_technology,
      ptpClock->foreign[j].foreign_master_port_id,
      ptpClock->foreign[j].foreign_master_uuid[0], ptpClock->foreign[j].foreign_master_uuid[1],
      ptpClock->foreign[j].foreign_master_uuid[2], ptpClock->foreign[j].foreign_master_uuid[3],
      ptpClock->foreign[j].foreign_master_uuid[4], ptpClock->foreign[j].foreign_master_uuid[5]);
    
    ptpClock->foreign_record_i = (ptpClock->foreign_record_i + 1)%ptpClock->max_foreign_records;
  }
  
  msgUnpackHeader(buf, &ptpClock->foreign[j].header);
  msgUnpackSync(buf, &ptpClock->foreign[j].sync);
  
  return &ptpClock->foreign[j].sync;
}
