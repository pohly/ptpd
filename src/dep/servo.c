#include "../ptpd.h"

void initClock(PtpClock *ptpClock)
{
  DBG("%sinitClock\n", ptpClock->name);
  
  /* clear vars */
  ptpClock->master_to_slave_delay.seconds = ptpClock->master_to_slave_delay.nanoseconds = 0;
  ptpClock->slave_to_master_delay.seconds = ptpClock->slave_to_master_delay.nanoseconds = 0;
  ptpClock->observed_variance = 0;
  ptpClock->observed_drift = 0;  /* clears clock servo accumulator (the I term) */
  ptpClock->owd_filt.s_exp = 0;  /* clears one-way delay filter */
  ptpClock->halfEpoch = ptpClock->halfEpoch || ptpClock->runTimeOpts.halfEpoch;
  ptpClock->runTimeOpts.halfEpoch = 0;
  
  /* level clock */
  if(!ptpClock->runTimeOpts.noAdjust)
    adjTime(0, NULL, ptpClock);
}

void updateDelay(TimeInternal *send_time, TimeInternal *recv_time,
  one_way_delay_filter *owd_filt, PtpClock *ptpClock)
{
  Integer16 s;
  
  DBGV("%supdateDelay send %10ds %11dns recv %10ds %11dns\n",
       ptpClock->name,
       send_time->seconds, send_time->nanoseconds,
       recv_time->seconds, recv_time->nanoseconds);
  
  /* calc 'slave_to_master_delay' */
  subTime(&ptpClock->slave_to_master_delay, recv_time, send_time);
  
  /* update 'one_way_delay' */
  addTime(&ptpClock->one_way_delay, &ptpClock->master_to_slave_delay, &ptpClock->slave_to_master_delay);
  ptpClock->one_way_delay.seconds /= 2;
  ptpClock->one_way_delay.nanoseconds /= 2;

  DBGV("%supdateDelay slave_to_master_delay %10ds %11dns one_way_delay %10ds %11dns\n",
       ptpClock->name,
       ptpClock->slave_to_master_delay.seconds, ptpClock->slave_to_master_delay.nanoseconds,
       ptpClock->one_way_delay.seconds, ptpClock->one_way_delay.nanoseconds);
  
  if(ptpClock->one_way_delay.seconds)
  {
    /* cannot filter with secs, clear filter */
    owd_filt->s_exp = owd_filt->nsec_prev = 0;
    return;
  }
  
  /* avoid overflowing filter */
  s =  ptpClock->runTimeOpts.s;
  while(abs(owd_filt->y)>>(31-s))
    --s;
  
  /* crank down filter cutoff by increasing 's_exp' */
  if(owd_filt->s_exp < 1)
    owd_filt->s_exp = 1;
  else if(owd_filt->s_exp < 1<<s)
    ++owd_filt->s_exp;
  else if(owd_filt->s_exp > 1<<s)
    owd_filt->s_exp = 1<<s;
  
  /* filter 'one_way_delay' */
  owd_filt->y = (owd_filt->s_exp-1)*owd_filt->y/owd_filt->s_exp +
    (ptpClock->one_way_delay.nanoseconds/2 + owd_filt->nsec_prev/2)/owd_filt->s_exp;
  
  owd_filt->nsec_prev = ptpClock->one_way_delay.nanoseconds;
  ptpClock->one_way_delay.nanoseconds = owd_filt->y;
  
  DBG("%sdelay filter %d, %d\n", ptpClock->name, owd_filt->y, owd_filt->s_exp);
}

void updateOffset(TimeInternal *send_time, TimeInternal *recv_time,
  offset_from_master_filter *ofm_filt, PtpClock *ptpClock)
{
    DBGV("%supdateOffset send %10ds %11dns recv %10ds %11dns\n",
         ptpClock->name,
         send_time->seconds, send_time->nanoseconds,
         recv_time->seconds, recv_time->nanoseconds);
  
  /* calc 'master_to_slave_delay' */
  subTime(&ptpClock->master_to_slave_delay, recv_time, send_time);
  
  /* update 'offset_from_master' */
  subTime(&ptpClock->offset_from_master, &ptpClock->master_to_slave_delay, &ptpClock->one_way_delay);
  
  DBGV("%supdateOffset master_to_slave_delay %10ds %11dns offset_from_master %10ds %11dns\n",
       ptpClock->name,
       ptpClock->master_to_slave_delay.seconds, ptpClock->master_to_slave_delay.nanoseconds,
       ptpClock->offset_from_master.seconds, ptpClock->offset_from_master.nanoseconds);

  if(ptpClock->offset_from_master.seconds)
  {
    /* cannot filter with secs, clear filter */
    ofm_filt->nsec_prev = 0;
    return;
  }
  
  /* filter 'offset_from_master' */
  ofm_filt->y = ptpClock->offset_from_master.nanoseconds/2 + ofm_filt->nsec_prev/2;
  ofm_filt->nsec_prev = ptpClock->offset_from_master.nanoseconds;
  ptpClock->offset_from_master.nanoseconds = ofm_filt->y;
  
  DBGV("%soffset filter %d\n", ptpClock->name, ofm_filt->y);
}

void updateClock(PtpClock *ptpClock)
{
  Integer32 adj;
  
  DBGV("%supdateClock\n", ptpClock->name);
  
  if(ptpClock->offset_from_master.seconds)
  {
    /* if secs, reset clock or set freq adjustment to max */
    if(!ptpClock->runTimeOpts.noAdjust || ptpClock->nic_instead_of_system)
    {
      if(!ptpClock->runTimeOpts.noResetClock)
      {
        adjTimeOffset(&ptpClock->offset_from_master, ptpClock);
        initClock(ptpClock);
      }
      else
      {
        adj = ptpClock->offset_from_master.nanoseconds > 0 ? ADJ_FREQ_MAX : -ADJ_FREQ_MAX;
        adjTime(-adj, &ptpClock->offset_from_master, ptpClock);
      }
    }
  }
  else
  {
    /* the PI controller */
    
    /* no negative or zero attenuation */
    if(ptpClock->runTimeOpts.ap < 1)
     ptpClock->runTimeOpts.ap = 1;
    if(ptpClock->runTimeOpts.ai < 1)
      ptpClock->runTimeOpts.ai = 1;
    
    /* the accumulator for the I component */
    ptpClock->observed_drift += ptpClock->offset_from_master.nanoseconds/ptpClock->runTimeOpts.ai;
    
    /* clamp the accumulator to ADJ_FREQ_MAX for sanity */
    if(ptpClock->observed_drift > ADJ_FREQ_MAX)
      ptpClock->observed_drift = ADJ_FREQ_MAX;
    else if(ptpClock->observed_drift < -ADJ_FREQ_MAX)
      ptpClock->observed_drift = -ADJ_FREQ_MAX;
    
    adj = ptpClock->offset_from_master.nanoseconds/ptpClock->runTimeOpts.ap + ptpClock->observed_drift;
    
    /* apply controller output as a clock tick rate adjustment */
    if(!ptpClock->runTimeOpts.noAdjust || ptpClock->nic_instead_of_system)
      adjTime(-adj, &ptpClock->offset_from_master, ptpClock);
  }
  
  if(ptpClock->runTimeOpts.displayStats)
    displayStats(ptpClock);
  
  DBGV("%smaster-to-slave delay:   %10ds %11dns\n",
    ptpClock->name,
    ptpClock->master_to_slave_delay.seconds, ptpClock->master_to_slave_delay.nanoseconds);
  DBGV("%sslave-to-master delay:   %10ds %11dns\n",
       ptpClock->name,
    ptpClock->slave_to_master_delay.seconds, ptpClock->slave_to_master_delay.nanoseconds);
  DBGV("%sone-way delay:           %10ds %11dns\n",
    ptpClock->name,
    ptpClock->one_way_delay.seconds, ptpClock->one_way_delay.nanoseconds);
  DBG("%soffset from master:      %10ds %11dns\n",
    ptpClock->name,
    ptpClock->offset_from_master.seconds, ptpClock->offset_from_master.nanoseconds);
  DBG("%sobserved drift: %10d\n", ptpClock->name, ptpClock->observed_drift);
}

