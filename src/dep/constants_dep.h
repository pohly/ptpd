/* constants_dep.h */

#ifndef CONSTANTS_DEP_H
#define CONSTANTS_DEP_H

/* platform dependent */

#include <limits.h>

#if !defined(linux) && !defined(__NetBSD__) && !defined(__FreeBSD__)
#error Not ported to this architecture, please update.
#endif

#ifdef	linux
#include<netinet/in.h>
#include<net/if.h>
#include<net/if_arp.h>
#define IFACE_NAME_LENGTH         IF_NAMESIZE
#define NET_ADDRESS_LENGTH        INET_ADDRSTRLEN

#define IFCONF_LENGTH 10

#include<endian.h>
#if __BYTE_ORDER == __LITTLE_ENDIAN
#define PTPD_LSBF
#elif __BYTE_ORDER == __BIG_ENDIAN
#define PTPD_MSBF
#endif
#endif /* linux */


#if defined(__NetBSD__) || defined(__FreeBSD__)
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <net/if.h>
# include <net/if_dl.h>
# include <net/if_types.h>
# if defined(__FreeBSD__)
#  include <net/ethernet.h>
#  include <sys/uio.h>
# else
#  include <net/if_ether.h>
# endif
# include <ifaddrs.h>
# define IFACE_NAME_LENGTH         IF_NAMESIZE
# define NET_ADDRESS_LENGTH        INET_ADDRSTRLEN

# define IFCONF_LENGTH 10

# define adjtimex ntp_adjtime

# include <machine/endian.h>
# if BYTE_ORDER == LITTLE_ENDIAN
#   define PTPD_LSBF
# elif BYTE_ORDER == BIG_ENDIAN
#   define PTPD_MSBF
# endif
#endif

/**
 * This value used to be used as limit for adjtimex() and the clock
 * servo. The limits of adjtimex() are now determined at runtime in
 * dep/time.c and are larger than before because also the us/tick
 * value is adjusted, but the servo still uses this limit as sanity
 * check. Now that the underlying system is no longer the limiting
 * factor, perhaps the value should be configurable?
 *
 * The value was 5120000 (based on the maximum adjustment possible by
 * adjusting just the frequency in adjtimex()), but that turned out to
 * be too small on a system under load: as soon as the observed drift
 * got larger than 5120000, it was clamped and the frequency
 * adjustment remained too small to drive the offset back to
 * zero. Here's a log of that situation:
 *
00:00:10 knlcst4 ptpd: state, one way delay, offset from master, drift, variance, clock adjustment (ppb)
00:00:10 knlcst4 ptpd: init
00:00:10 knlcst4 ptpd: lstn
00:00:30 knlcst4 ptpd: mst
00:00:31 knlcst4 ptpd: slv, 0.000000000, 0.000000000, 0, 0, 0
00:00:33 knlcst4 ptpd: slv, 0.000000000, 0.000104000, -104, 0, 10504
00:00:35 knlcst4 ptpd: slv, 0.000000000, 0.001509000, -1613, 0, 152513
00:00:37 knlcst4 ptpd: slv, 0.000000000, 0.001842500, 229, 0, -184479
00:00:39 knlcst4 ptpd: slv, 0.000000000, 0.001268000, 1497, 0, -128297
...
00:30:17 knlcst4 ptpd: slv, 0.003492994, 0.000289006, -175627, 0, 146727
00:30:19 knlcst4 ptpd: slv, 0.003492994, 0.004753994, -180380, 0, 655779
00:30:21 knlcst4 ptpd: slv, 0.003487771, 0.000671117, -179709, 0, 112598
00:30:23 knlcst4 ptpd: slv, 0.003487771, 0.002132729, -177577, 0, -35695
00:30:25 knlcst4 ptpd: slv, 0.003487771, 0.004137270, -181714, 0, 595441
00:30:27 knlcst4 ptpd: slv, 0.003487771, 0.006283270, -187997, 0, 816324
00:30:29 knlcst4 ptpd: slv, 0.003487771, 0.004510770, -192507, 0, 643584
[offset continuesly grows from here on]
...
00:35:03 knlcst4 ptpd: slv, 0.003435910, 0.061051910, -4602318, 0, 10707509
00:35:05 knlcst4 ptpd: slv, 0.003435910, 0.061784910, -4664102, 0, 10842593
00:35:07 knlcst4 ptpd: slv, 0.003435910, 0.060685910, -4724787, 0, 10793378
00:35:09 knlcst4 ptpd: slv, 0.003435910, 0.057270910, -4782057, 0, 10509148
00:35:11 knlcst4 ptpd: slv, 0.003435910, 0.055342410, -4837399, 0, 10371640
00:35:13 knlcst4 ptpd: slv, 0.003435910, 0.059554910, -4896953, 0, 10852444
00:35:15 knlcst4 ptpd: slv, 0.003435910, 0.061615910, -4958568, 0, 11120159
00:35:17 knlcst4 ptpd: slv, 0.003435910, 0.062028410, -5020596, 0, 11223437
00:35:19 knlcst4 ptpd: slv, 0.003435910, 0.063420910, -5084016, 0, 11426107
[offset gets clamped at -5120000]
00:35:21 knlcst4 ptpd: slv, 0.003435910, 0.060922910, -5120000, 0, 11212291
00:35:23 knlcst4 ptpd: slv, 0.003435910, 0.061610910, -5120000, 0, 11281091
00:35:25 knlcst4 ptpd: slv, 0.003435910, 0.061527910, -5120000, 0, 11272791
00:35:27 knlcst4 ptpd: slv, 0.003435910, 0.063027910, -5120000, 0, 11422791
...
[maximum value for adjustment reached]
00:53:31 knlcst4 ptpd: slv, 0.003368888, 0.285260388, -5120000, 0, 33554432
00:53:33 knlcst4 ptpd: slv, 0.003368888, 0.284009388, -5120000, 0, 33520938
00:53:35 knlcst4 ptpd: slv, 0.003368888, 0.282019888, -5120000, 0, 33321988
00:53:37 knlcst4 ptpd: slv, 0.003368888, 0.284614888, -5120000, 0, 33554432
00:53:39 knlcst4 ptpd: slv, 0.003368888, 0.287445888, -5120000, 0, 33554432
00:53:41 knlcst4 ptpd: slv, 0.003368888, 0.283629388, -5120000, 0, 33482938
00:53:43 knlcst4 ptpd: slv, 0.003368888, 0.283933388, -5120000, 0, 33513338
00:53:45 knlcst4 ptpd: slv, 0.003368888, 0.289208888, -5120000, 0, 33554432
00:53:47 knlcst4 ptpd: slv, 0.003368888, 0.289672388, -5120000, 0, 33554432
00:53:49 knlcst4 ptpd: slv, 0.003368888, 0.284487388, -5120000, 0, 33554432
00:53:51 knlcst4 ptpd: slv, 0.003368888, 0.277956888, -5120000, 0, 32915688
00:53:53 knlcst4 ptpd: slv, 0.003368888, 0.283933388, -5120000, 0, 33513338
00:53:55 knlcst4 ptpd: slv, 0.003368888, 0.287284388, -5120000, 0, 33554432
00:53:57 knlcst4 ptpd: slv, 0.003368888, 0.285872388, -5120000, 0, 33554432
00:53:59 knlcst4 ptpd: slv, 0.003368888, 0.290243888, -5120000, 0, 33554432
00:54:01 knlcst4 ptpd: slv, 0.003368888, 0.292336388, -5120000, 0, 33554432
00:54:03 knlcst4 ptpd: slv, 0.003368888, 0.292805388, -5120000, 0, 33554432
00:54:05 knlcst4 ptpd: slv, 0.003379526, 0.291303707, -5120000, 0, 33554432
...
01:51:44 knlcst4 ptpd: slv, 0.003304196, 0.995760196, -5120000, 0, 33554432
01:51:46 knlcst4 ptpd: slv, 0.003304196, 0.993363696, -5120000, 0, 33554432
01:51:48 knlcst4 ptpd: resetting system clock to 1200569796s 698871196e-9
01:51:49 knlcst4 ptpd: slv, 0.003304196, -1.000127196, 0, 0, 0
01:51:51 knlcst4 ptpd: slv, 0.003304196, 0.001648402, 1648, 0, -166488
01:51:53 knlcst4 ptpd: slv, 0.003304196, 0.004037804, 5685, 0, -409465
01:51:55 knlcst4 ptpd: slv, 0.003304196, 0.002923304, 8608, 0, -300938
01:51:57 knlcst4 ptpd: slv, 0.003304196, 0.000091696, 8517, 0, 652
[cycle repeats]
 */
#define ADJ_FREQ_MAX  512000000

/* UDP/IPv4 dependent */

#define SUBDOMAIN_ADDRESS_LENGTH  4
#define PORT_ADDRESS_LENGTH       2

#define PACKET_SIZE  300

#define PTP_EVENT_PORT    319
#define PTP_GENERAL_PORT  320

#define DEFAULT_PTP_DOMAIN_ADDRESS     "224.0.1.129"
#define ALTERNATE_PTP_DOMAIN1_ADDRESS  "224.0.1.130"
#define ALTERNATE_PTP_DOMAIN2_ADDRESS  "224.0.1.131"
#define ALTERNATE_PTP_DOMAIN3_ADDRESS  "224.0.1.132"

#define HEADER_LENGTH             40
#define SYNC_PACKET_LENGTH        124
#define DELAY_REQ_PACKET_LENGTH   124
#define FOLLOW_UP_PACKET_LENGTH   52
#define DELAY_RESP_PACKET_LENGTH  60
#define MANAGEMENT_PACKET_LENGTH  136

#define MM_STARTING_BOUNDARY_HOPS  0x7fff

/* others */

#define SCREEN_BUFSZ  128
#define SCREEN_MAXSZ  80

#endif

