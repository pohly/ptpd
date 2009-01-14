/*******************************************************************************
 *
 * Intel(R) Gigabit Ethernet Linux driver
 *
 * Copyright (c) 2006-2008, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Intel Corporation nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Intel Corporation ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Intel Corporation BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *******************************************************************************/

/** @todo add size field to structs to allow for extensions */

#ifndef __E1000_IOCTL_H__
#define __E1000_IOCTL_H__

/**
 * a time stamp
 *
 * The value is always positive, negative time stamps must be
 * represented with an additional +1/-1 sign factor.
 *
 * @todo use a better definition for 64 bit unsigned which works
 * in the driver and user space
 */
struct E1000_TS {
    unsigned long long seconds;
    unsigned int nanoseconds;
};

/**
 * Initialize NIC for PTP time stamping.
 * After this call E1000_TSYNC_SYSTIME_IOCTL will return
 * time stamps which are somewhat related to the current system time,
 * but will drift apart again.
 *
 * @return 0 for success
 */
#define E1000_TSYNC_INIT_IOCTL 0x89f0

/**
 * Optionally (if ARGU::negative_offset != 0) update NIC time by given
 * offset and return current time. Current time is inaccurate because
 * accessing the NIC incurrs a certain non-deterministic delay.
 *
 * @todo determine how large the delay is in reality
 */
#define E1000_TSYNC_SYSTIME_IOCTL 0x89f1

/** parameters and results of E1000_TSYNC_SYSTIME_IOCTL */
struct E1000_TSYNC_SYSTIME_ARGU {
    /** input: offset to be applied to time; output: current time */
    struct E1000_TS time;

    /**
     * <0: substract input offset
     * >0: add input offset
     * =0: only read current time
     */
    int negative_offset;
};

/**
 * Speed up (positive value) or slow down the clock by a
 * certain amount specified as parts per billion (1e-9).
 * For example, a parameter of 1 means "add 1 microsecond
 * every second".
 */
#define E1000_TSYNC_ADJTIME_IOCTL 0x89f2

struct E1000_TSYNC_ADJTIME_ARGU {
    /**
     * input: adjustment to be applied to time in ppb (1e-9);
     * output: current adjustment
     */
    long long adj;

    /**
     * only set adjustment if != 0
     */
    int set_adj;
};

/** @todo: consolidate enable/disable ioctl() calls into one? */

/** enable time stamping of outgoing PTP packets, returns 0 if successful */
#define E1000_TSYNC_ENABLETX_IOCTL 0x89f4
/** disable time stamping of outgoing PTP packets, returns 0 if successful */
#define E1000_TSYNC_DISABLETX_IOCTL 0x89f5

/**
 * enable time stamping of incoming PTP packets, returns 0 if successful
 *
 * *(int *)&ifr_data determines mode
 */
#define E1000_TSYNC_ENABLERX_IOCTL 0x89f8

/** @todo: add RX timestamp mode = 5 */
enum {
  E1000_L2_V2_SYNC,       /**< time stamp incoming layer 2 PTP V2 Sync packets */
  E1000_L2_V2_DELAY,      /**< time stamp incoming layer 2 PTP V2 Delay_Req packets */
  E1000_UDP_V1_SYNC,      /**< time stamp incoming UDP PTP V1 Sync packets */
  E1000_UDP_V1_DELAY,     /**< time stamp incoming UDP PTP V1 Delay_Req packets */
  E1000_TSYNC_MAX
};
  

/** disable time stamping of incoming PTP packets, returns 0 if successful */
#define E1000_TSYNC_DISABLERX_IOCTL 0x89f9

/** get information about send/receive time stamps */
#define E1000_TSYNC_READTS_IOCTL 0x89fc

struct E1000_TSYNC_READTS_ARGU {
    /**
     * in: not only return NIC time stamps, but also the
     * corresponding system time (may cause additional overhead)
     */
    int withSystemTime;

    /** out: receive information is only valid if rx_valid != 0 */
    int rx_valid;
    /** out: receive NIC time stamp */
    struct E1000_TS rx;
    /** out (if withSystemTime was true): the corresponding receive system time */
    struct E1000_TS rx_sys;
    /** out: the PTP sequence ID of the time stamped packet */
    uint16_t sourceSequenceId;
    /** out: the PTP source ID of the time stamped packet */
    unsigned char sourceIdentity[6];

    /** out: send information is only valid if tx_valid != 0 */
    int tx_valid;

    /** out: send NIC time stamp */
    struct E1000_TS tx;
    /** out (if withSystemTime was true): the corresponding send system time */
    struct E1000_TS tx_sys;
};

/**
 * Correlates system time and NIC time each time it is called. The
 * - offsetFromSystem = NIC time - system time
 * is calculated as in PTP/IEEE1555:
 * - oneWayDelay = (NICToSystem + systemToNIC)/2
 * - offsetFromSystem = systemToNIC - oneWayDelay
 *                    = (systemToNIC - NICToSystem)/2
 *
 * A driver which does not measure both delays can simply set one
 * delay to zero and return twice the offset in the other field.
 *
 * A positive offset means that the NIC time is higher than the system
 * time, i.e. either the system clock must speed up to catch up with
 * the NIC or the NIC must slow down.
 */
#define E1000_TSYNC_COMPARETS_IOCTL 0x89fd

struct E1000_TSYNC_COMPARETS_ARGU {
    /** out: one-way delay for sending from NIC to system */
    struct E1000_TS NICToSystem;
    /** +1 for positiv delay or -1 for negative one */
    int NICToSystemSign;

    /** one-way delay for sending from system to NIC */
    struct E1000_TS systemToNIC;
    /** +1 for positiv delay or -1 for negative one */
    int systemToNICSign;
};

#endif /* __E1000_IOCTL_H__ */
