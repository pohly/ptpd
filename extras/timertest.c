/*******************************************************************
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
 *******************************************************************/

/**
 * This program tests the quality of system time (resolution and
 * distribution of increments) and of synchronization between different
 * nodes in a cluster. Synchronization is tested by having all processes
 * sleep for a while, then let each pair of processes exchange multiple
 * messages. Only two processes are active during each message exchange.
 *
 * Clock offset calculation: For each message exchange three time stamps
 * are taken: t_send -> t_middle -> t_recv. Under the assumption that the
 * message transmission in both direction is equally fast, it follows that
 * (t_send + t_recv) / 2 = t_middle + offset. To remove noise the exchanges
 * with the highest (t_recv - t_send) delta are excluded before averaging the
 * remaining samples.
 *
 * Usage: Compile and start as an MPI application with one process per
 * node. It runs until killed. For unmonitored operation of a specific
 * duration use Intel MPI's "MPD_TIMEOUT" option.
 *
 * Output: Is written to the syslog and stderr. Output starts with
 * some information about the resolution of the system time call. Then
 * for each message exchange the process with the smaller rank logs
 * the clock offset with its peer.
 *
 * Analysis: perfbase experiment and input descriptions are provided to
 * import the output of timertest and PTPd from a syslog file.
 *
 * Author: Patrick Ohly
 */

#include <mpi.h>

#ifndef _WIN32
# include <unistd.h>
# include <sys/time.h>
# include <syslog.h>
#else
# include <windows.h>
# include <sys/timeb.h>
# define sleep( sec ) Sleep( 1000 * (sec) )
# define popen  _popen
# define pclose _pclose
# define pclose _pclose
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

#define MSG_CNT         1000      /**< number of message to be sent back and forth */
#define NUM_BINS          11      /**< number of bins for offset histogram  */
#define LATENCY_TEST      11      /**< number of seconds that each message latency measurement is supposed to run */
#define MAX_SAMPLES    10000      /**< maximum number of samples to collect for median/average clock increment */
#define CLOCK_DURATION     5      /**< duration of clock increment test in seconds */

#define HAVE_LIBELF_H      1      /**< compile in support for finding functions in virtual dynamic shared object (VDSO);
                                     this is necessary because on Intel 64 that VDSO was added recently in 2.6.23d-rc1
                                     and glibc will not yet use it by itself */

#ifndef _WIN32
typedef int (*clock_gettime_t)(clockid_t clock_id, struct timespec *tp);
static clock_gettime_t my_clock_gettime = clock_gettime;

typedef int (*gettimeofday_t)(struct timeval *tp, void *tzp);
static gettimeofday_t my_gettimeofday = (void *)gettimeofday;
#endif

#ifdef HAVE_LIBELF_H
#include <libelf.h>
/**
 * return absolute address of dynamic symbol in Linux kernel vdso
 */
static void *findVDSOSym(const char *symname);
#endif

/** type used to count seconds */
typedef double seconds_t;

/** type used to count clock ticks */
typedef long long ticks_t;

/**
 * format number of seconds with ns/us/ms/s suffix (depending on magnitude)
 * and configurable number of digits before the decimal point (width) and after
 * it (precision)
 */
static const char *prettyprintseconds( seconds_t seconds, int width, int precision, char *buffer )
{
    static char localbuffer[80];
    seconds_t absseconds = fabs( seconds );

    if( !buffer ) {
        buffer = localbuffer;
    }

    if( absseconds < 1e-6 ) {
        sprintf( buffer, "%*.*fns", width, precision, seconds * 1e9 );
    } else if( absseconds < 1e-3 ) {
        sprintf( buffer, "%*.*fus", width, precision, seconds * 1e6 );
    } else if( absseconds < 1 ) {
        sprintf( buffer, "%*.*fms", width, precision, seconds * 1e3 );
    } else {
        sprintf( buffer, "%*.*fs", width, precision, seconds );
    }

    return buffer;
}

/** generates a string of 'width' many hash signs */
static const char *printbar( int width )
{
    static char buffer[80];

    if( width > sizeof(buffer) - 1 ) {
        width = sizeof(buffer) - 1;
    }
    memset( buffer, '#', width );
    buffer[width] = 0;
    return buffer;
}

/** only on Linux: switch between gettimeofday() and clock_gettime() calls */
static int usetod;

/** returns system time as number of ticks since common epoch */
static ticks_t systicks(void)
{
#ifdef _WIN32
    struct _timeb timebuffer;
    _ftime( &timebuffer );
    return (ticks_t)timebuffer.time * 1000 + timebuffer.millitm;
#else
    if (usetod) {
        struct timeval cur_time;
        my_gettimeofday( &cur_time, NULL );
        return (ticks_t)cur_time.tv_sec * 1000000 + cur_time.tv_usec;
    } else {
        struct timespec cur_time;
        my_clock_gettime(CLOCK_REALTIME, &cur_time);
        return (ticks_t)cur_time.tv_sec * 1000000000ul + cur_time.tv_nsec;
    }
#endif
}

/** duration of one clock tick in seconds */
static seconds_t clockperiod;

/** returns system time as number of seconds since common epoch */
static seconds_t systime(void)
{
    ticks_t ticks = systicks();
    return ticks * clockperiod;
}

/** the result of one ping/pong message exchange */
struct sample {
    seconds_t t_send, t_middle, t_recv;
};

/** offset between nodes given one sample */
static seconds_t offset( struct sample *sample )
{
    return (sample->t_recv + sample->t_send) / 2 - sample->t_middle;
}

/** qsort comparison function for sorting by increasing duration of samples */
static int compare_duration( const void *a, const void *b )
{
    const struct sample *sa = a, *sb = b;
    seconds_t tmp = (sb->t_recv - sb->t_send) - (sa->t_recv - sa->t_send);

    if( tmp < 0 ) {
        return -1;
    } else if( tmp > 0 ) {
        return 1;
    } else {
        return 0;
    }
}

/**
 * send messages back and forth between two process in MPI_COMM_WORLD
 * without time stamping
 */
static void simplepingpong( int source, int target, int tag, int num )
{
    MPI_Status status;
    int i, rank;
    char buffer[1];
 
    MPI_Comm_rank( MPI_COMM_WORLD, &rank );
 
    /* message exchange */
    for( i = 0; i < num; i++ ) {
        if( rank == target ) {
            MPI_Recv( buffer, 0, MPI_BYTE, source, tag, MPI_COMM_WORLD, &status );
            MPI_Send( buffer, 0, MPI_BYTE, source, tag, MPI_COMM_WORLD );
        } else if( rank == source ) {
            MPI_Send( buffer, 0, MPI_BYTE, target, tag, MPI_COMM_WORLD );
            MPI_Recv( buffer, 0, MPI_BYTE, target, tag, MPI_COMM_WORLD, &status );
        }
    }
}

/**
 * send messages back and forth between two process in MPI_COMM_WORLD,
 * then calculate the clock offset and log it
 */
static void pingpong( int source, int target, int tag, int num, int dryrun )
{
    MPI_Status status;
    int i, rank;
    char buffer[1];
    seconds_t *t_middle;
    struct sample *samples;
    char host[MPI_MAX_PROCESSOR_NAME + 1], peer[MPI_MAX_PROCESSOR_NAME + 1];
    int len;

    t_middle = malloc(sizeof(*t_middle) * num);
    samples = malloc(sizeof(*samples) * num);

    MPI_Comm_rank( MPI_COMM_WORLD, &rank );
    MPI_Get_processor_name(host, &len);
    host[len] = 0;
    memset( buffer, 0, sizeof(buffer) );

    /* message exchange */
    for( i = 0; i < num; i++ ) {
        if( rank == target ) {
            MPI_Recv( buffer, 0, MPI_BYTE, source, tag, MPI_COMM_WORLD, &status );
            t_middle[i] = systime();
            MPI_Send( buffer, 0, MPI_BYTE, source, tag, MPI_COMM_WORLD );
        } else if( rank == source ) {
            samples[i].t_send = systime();
            MPI_Send( buffer, 0, MPI_BYTE, target, tag, MPI_COMM_WORLD );
            MPI_Recv( buffer, 0, MPI_BYTE, target, tag, MPI_COMM_WORLD, &status );
            samples[i].t_recv = systime();
        }
    }

    /* calculation of offset */
    if( dryrun ) {
        /* don't print */
    } else if( rank == source ) {
        MPI_Datatype type;
        MPI_Status status;
        int start, end;
        seconds_t average_off = 0, deviation_off = 0, min_off = 1e9, max_off = -1e9;
        seconds_t average_round = 0, deviation_round = 0, min_round = 1e9, max_round = -1e9;
        unsigned int maxcount = 0, histogram[NUM_BINS], bin;
        char buffers[10][80];

        /* sort incoming time stamps into the right place in each sample */
        MPI_Type_vector( num, 1, 3, MPI_DOUBLE, &type );
        MPI_Type_commit( &type );
        MPI_Recv( &samples[0].t_middle, 1, type, target, 0, MPI_COMM_WORLD, &status );

        /* get peer name */
        MPI_Recv(peer, sizeof(peer), MPI_CHAR, target, 0, MPI_COMM_WORLD, &status);

        /* sort by increasing duration */
        qsort( samples, num, sizeof(*samples), compare_duration );

        /*
         * calculate min, max, average and empirical (n-1) standard deviation
         * of offset, ignoring the 10% of the samples at borders
         */
        memset( histogram, 0, sizeof(histogram) );
        start = num * 5 / 100;
        end = num * 95 / 100;
        for( i = start;
             i < end;
             i++ ) {
            seconds_t tmp;

            tmp = offset(samples + i);
            if( tmp < min_off ) {
                min_off = tmp;
            }
            if( tmp > max_off ) {
                max_off = tmp;
            }

            tmp = samples[i].t_recv - samples[i].t_send;
            if( tmp < min_round ) {
                min_round = tmp;
            }
            if (tmp > max_round) {
                max_round = tmp;
            }
        }

        for( i = start;
             i < end;
             i++ ) {
            seconds_t off = offset(samples + i);
            seconds_t round = samples[i].t_recv - samples[i].t_send;

            bin = (off - min_off) * NUM_BINS / (max_off - min_off);
            /* sanity check */
            if( bin >= NUM_BINS ) {
                bin = NUM_BINS - 1;
            }
            histogram[bin]++;
            average_off += off;
            deviation_off += off * off;

            average_round += round;
            deviation_round += round * round;
        }
        for( bin = 0; bin < NUM_BINS; bin++ ) {
            if( histogram[bin] > maxcount ) {
                maxcount = histogram[bin];
            }
        }
        average_off /= end - start;
        deviation_off = sqrt( deviation_off / ( end - start ) - average_off * average_off );
        average_round /= end - start;
        deviation_round = sqrt( deviation_round / ( end - start ) - average_round * average_round );

        syslog(LOG_INFO, "offset %s - %s", host, peer);
        syslog(LOG_INFO,
               "min/average/max/deviation of offset and round-trip time: "
               "%s %s %s %s     %s %s %s %s",
               prettyprintseconds( min_off, 0, 3, buffers[0] ),
               prettyprintseconds( average_off, 0, 3, buffers[1] ),
               prettyprintseconds( max_off, 0, 3, buffers[2] ),
               prettyprintseconds( deviation_off, 0, 3, buffers[3] ),
               prettyprintseconds( min_round, 0, 3, buffers[4] ),
               prettyprintseconds( average_round, 0, 3, buffers[5] ),
               prettyprintseconds( max_round, 0, 3, buffers[6] ),
               prettyprintseconds( deviation_round, 0, 3, buffers[7] ));
        for( bin = 0; bin < NUM_BINS; bin++ ) {
            syslog(LOG_INFO, " >= %s: %s %u\n",
                   prettyprintseconds( min_off + bin * ( max_off - min_off ) / NUM_BINS, 8, 3, buffers[0] ),
                   printbar( 40 * histogram[bin] / maxcount ),
                   histogram[bin] );
        }
        syslog(LOG_INFO, " >= %s:  0\n",
               prettyprintseconds( max_off, 8, 3, buffers[0] ));
    } else if( rank == target ) {
        MPI_Send(t_middle, num, MPI_DOUBLE, source, 0, MPI_COMM_WORLD);
        MPI_Send(host, strlen(host)+1, MPI_CHAR, source, 0, MPI_COMM_WORLD);
    }

    free(samples);
    free(t_middle);
}

/** qsort routine for increasing sort of doubles */
static int compare_ticks( const void *a, const void *b )
{
    ticks_t delta = *(const ticks_t *)a - *(const ticks_t *)b;

    return delta < 0 ? -1 :
        delta > 0 ? 1 :
        0;
}

/** the initial clock samples encounted by genhistogram() */
static ticks_t samples[MAX_SAMPLES];

/** number of entries in samples array */
static unsigned int count;

/**
 * call timer source repeatedly and record delta between samples
 * in histogram and samples array
 *
 * @param duration          maximum number of seconds for whole run
 * @param min_increase      first slot in histogram is for values < 0,
 *                          second for >=0 and < min_increase
 * @param bin_size          width of all following bins
 * @param histogram_size    number of slots, including special ones
 * @param histogram         buffer for histogram, filled by this function
 * @return number of calls to systicks()
 */
static unsigned int genhistogram(seconds_t duration,
                                 ticks_t min_increase,
                                 ticks_t bin_size,
                                 unsigned int histogram_size,
                                 unsigned int *histogram)
{
    ticks_t increase;
    ticks_t startticks, lastticks, nextticks;
    ticks_t endticks = duration / clockperiod;
    unsigned int calls = 0;

    startticks = systicks();
    lastticks = 0;
    count = 0;
    memset(histogram, 0, sizeof(*histogram) * histogram_size);
    do {
        calls++;
        nextticks = systicks() - startticks;
        increase = nextticks - lastticks;
        if( increase < 0 ) {
            histogram[0]++;
        } else if( increase > 0 ) {
            unsigned int index;

            if( count < MAX_SAMPLES ) {
                samples[count] = increase;
                count++;
            }

            if( increase < min_increase ) {
                index = 1;
            } else {
                index = (unsigned int)( ( increase - min_increase ) / bin_size ) + 2;
                if( index >= histogram_size ) {
                    index = histogram_size - 1;
                }
            }
            histogram[index]++;
        }
        lastticks = nextticks;
    } while( lastticks < endticks );

    return calls;
}

/**
 * runs a timer performance test for the given duration
 *
 * @param duration    duration of test in seconds
 */
void timerperformance(seconds_t duration)
{
    unsigned int i;
    unsigned int max = 0;
    char buffer[3][256];
    double average = 0, median = 0;
    unsigned int calls;
    unsigned int simple_histogram[3];
    ticks_t min_increase, bin_size, max_increase;
    unsigned int histogram_size;
    unsigned int *clockhistogram;

    /* determine range of clock increases for real run */
    calls = genhistogram(2.0, 1, 1, 3, simple_histogram);
    qsort(samples, count, sizeof(samples[0]), compare_ticks);

    /* shoot for 10 slots, but allow for some extra slots at both ends as needed */
    min_increase = samples[0] == 1 ? 1 : samples[0] * 9 / 10;
    max_increase = samples[count - 1];
    bin_size = (max_increase - min_increase) / 10;
    if (bin_size * clockperiod <= 1e-9) {
        bin_size = 1e-9 / clockperiod;
    }
    histogram_size = (max_increase - min_increase) / bin_size + 3 + 5;
    clockhistogram = malloc(histogram_size * sizeof(*clockhistogram));
    calls = genhistogram(duration, min_increase, bin_size, histogram_size, clockhistogram);
    qsort(samples, count, sizeof(samples[0]), compare_ticks);

    /* print average and medium increase */
    for( i = 0; i < count; i++ ) {
        average += samples[i];
    }
    average /= count;
    qsort(samples, count, sizeof(samples[0]), compare_ticks);
    median = samples[count/2];
    syslog(LOG_INFO, "average clock increase %s -> %.3fHz, median clock increase %s -> %3.fHz, %s/call",
           prettyprintseconds(average * clockperiod, 0, 3, buffer[0]), 1/average/clockperiod,
           prettyprintseconds(median * clockperiod, 0, 3, buffer[1]), 1/median/clockperiod,
           prettyprintseconds(duration / calls, 0, 3, buffer[2]));

    for( i = 0; i < histogram_size; i++ ) {
        if( clockhistogram[i] > max ) {
            max = clockhistogram[i];
        }
    }
    syslog(LOG_INFO, " < %11.3fus: %s %u",
           0.0,
           printbar(clockhistogram[0] * 20 / max),
           clockhistogram[0]);
    syslog(LOG_INFO, " < %11.3fus: %s %u",
           min_increase * clockperiod * 1e6,
           printbar( clockhistogram[1] * 20 / max ),
           clockhistogram[1]);
    for( i = 2; i < histogram_size; i++ ) {
        syslog(LOG_INFO, ">= %11.3fus: %s %u",
               ( ( i - 2 ) * bin_size + min_increase ) * clockperiod * 1e6,
               printbar( clockhistogram[i] * 20 / max ),
               clockhistogram[i]);
    }
    printf( "\n" );

    free(clockhistogram);
}

/*
 * command line parameter handling
 */
static const char usage[] = "timertest <options>\n"
#ifndef _WIN32
    "   -g use gettimeofday() instead of clock_gettime() [default: clock_gettime()\n"
#ifdef HAVE_LIBELF_H
    "   -d do not extract pointer to system functions from virtual dynamic shared\n"
    "      instead of relying on glibc to do that (current glibc does not\n"
    "      yet do that for the new 2.6.23-rc1 VDSO) [default: on]\n"
#endif
#endif
    "\n"
    "First determines the resolution of the local clocks in each process.\n"
    "Then it does ping-pong tests between each pair of processes to measure\n"
    "the clock offset at each exchange. Runs until killed.\n"
    "Run with one process to just test clock resolution.\n"
    ;

int main( int argc, char **argv )
{
    int rank, size;
    int option;
    int vdso = 1;
    int c;
    int source, target;

    MPI_Init( &argc, &argv );
    MPI_Comm_rank( MPI_COMM_WORLD, &rank );
    MPI_Comm_size( MPI_COMM_WORLD, &size );

    while ((c = getopt(argc, argv,
                       ""
#ifndef _WIN32
                       "g"
#ifdef HAVE_LIBELF_H
                       "d"
#endif
#endif
                       )) != -1) {
        switch (c) {
#ifndef _WIN32
        case 'g':
            usetod = 1;
            break;
#ifdef HAVE_LIBELF_H
        case 'd':
            vdso = 0;
            break;
#endif
#endif
        default:
            fputs(usage, stderr);
            exit(1);
        }
    }

    option = 0;
#ifdef LOG_PERROR
    option |= LOG_PERROR;
#endif

    clockperiod =
#ifdef _WIN32
        1e-3
#else
        usetod ?
        1e-6 :
        1e-9
#endif
        ;
        
    openlog("timertest", option, LOG_USER);

#ifdef HAVE_LIBELF_H
    if (vdso) {
        if (usetod) {
            my_gettimeofday = findVDSOSym("gettimeofday");
            if (!my_gettimeofday) {
                my_gettimeofday = (void *)gettimeofday;
            }
        } else {
            my_clock_gettime = findVDSOSym("clock_gettime");
            if (!my_clock_gettime) {
                my_clock_gettime = clock_gettime;
            }
        }
    }
#endif

#ifndef _WIN32
    syslog(LOG_NOTICE, "using %s from %s",
           usetod ? "gettimeofday()" : "clock_gettime()",
           (usetod ? (my_gettimeofday == (void *)gettimeofday) : (my_clock_gettime == clock_gettime)) ? "glibc" : "VDSO");
#endif

    timerperformance(CLOCK_DURATION);

    if (size > 1) {
        for( source = 0; source < size - 1; source++ ) {
            for( target = source + 1; target < size; target++ ) {
                ticks_t start, middle, end;
                MPI_Barrier( MPI_COMM_WORLD );
                start = systicks();
                simplepingpong( source, target, 123, MSG_CNT );
                middle = systicks();
                pingpong( source, target, 123, MSG_CNT, 1 );
                end = systicks();

                if (rank == source) {
                    syslog(LOG_NOTICE, "overhead for %d<->%d ping-pong time stamping: %f%%",
                           source, target,
                           100 * (double)(end - middle) / (double)(middle - start) - 100);
                }

                MPI_Barrier( MPI_COMM_WORLD );
            }
        }
    }

    while (size > 1) {
        if(!rank) {
            syslog(LOG_NOTICE, "%s", printbar(75));
        }
        for( source = 0; source < size - 1; source++ ) {
            for( target = source + 1; target < size; target++ ) {
                MPI_Barrier( MPI_COMM_WORLD );
                pingpong( source, target, 123, MSG_CNT, 0 );
                MPI_Barrier( MPI_COMM_WORLD );
            }
        }

        sleep(LATENCY_TEST);
    }

    MPI_Finalize();

    return 0;
}

#ifdef HAVE_LIBELF_H

#if __WORDSIZE == 32
# define ElfNative_Ehdr Elf32_Ehdr
# define elfnative_getehdr elf32_getehdr
# define ElfNative_Shdr Elf32_Shdr
# define elfnative_getshdr elf32_getshdr
# define ElfNative_Sym Elf32_Sym
# define ELFNATIVE_ST_BIND ELF32_ST_BIND
# define ELFNATIVE_ST_TYPE ELF32_ST_TYPE
# define ElfNative_Phdr Elf32_Phdr
# define elfnative_getphdr elf32_getphdr
#else
# define ElfNative_Ehdr Elf64_Ehdr
# define elfnative_getehdr elf64_getehdr
# define ElfNative_Shdr Elf64_Shdr
# define elfnative_getshdr elf64_getshdr
# define ElfNative_Sym Elf64_Sym
# define ELFNATIVE_ST_BIND ELF64_ST_BIND
# define ELFNATIVE_ST_TYPE ELF64_ST_TYPE
# define ElfNative_Phdr Elf64_Phdr
# define elfnative_getphdr elf64_getphdr
#endif

static void *findVDSOSym(const char *symname)
{
    Elf *elf;
    void *res = NULL;
    char *start = NULL, *end = NULL;
    FILE *map;

    /**
     * Normally a program gets a pointer to the vdso via the ELF aux
     * vector entry AT_SYSINFO_EHDR (see
     * http://manugarg.googlepages.com/aboutelfauxiliaryvectors) at
     * startup. At runtime for a library, reading the memory map is
     * simpler.
     */
    map = fopen("/proc/self/maps", "r");
    if (map) {
        char line[320];

        while (fgets(line, sizeof(line), map) != NULL) {
            /* fputs(line, stdout); */
            if (strstr(line, "[vdso]")) {
                sscanf(line, "%p-%p", &start, &end);
                break;
            }
        }
        fclose(map);
    }

    /**
     * we know where the vdso is and that it contains an ELF object
     * => search the symbol via libelf
     */
    if (start) {
        elf = elf_memory(start, end-start);
        if (elf) {
            Elf_Scn *scn;
            size_t loadaddr = 0;

            for (scn = elf_nextscn(elf, NULL);
                 scn && !res;
                 scn = elf_nextscn(elf, scn)) {
                ElfNative_Shdr *shdr = elfnative_getshdr(scn);
                Elf_Data *data;

                /*
                 * All addresses are absolute, but the Linux kernel
                 * maps it at a different one. The load address can be
                 * determined by looking at any absolute address and
                 * substracting its offset relative to the file
                 * beginning because the whole file will be mapped
                 * into memory. We pick the first section for that.
                 */
                if (!loadaddr) {
                    loadaddr = shdr->sh_addr - shdr->sh_offset;
                }

                if( !shdr ||
                    shdr->sh_type != SHT_DYNSYM ) {
                    continue;
                }

                data = elf_getdata(scn, 0);
                if (!data || !data->d_size) {
                    continue;
                }

                ElfNative_Sym *sym = (ElfNative_Sym *)data->d_buf;
                ElfNative_Sym *lastsym = (ElfNative_Sym *)((char *)data->d_buf + data->d_size);

                for( ; !res && sym < lastsym; sym++ ) {
                    const char *name;
                    
                    if( sym->st_value == 0 || /* need valid address and size */
                        sym->st_size == 0 ||
                        ELFNATIVE_ST_TYPE( sym->st_info ) != STT_FUNC || /* only functions */
                        sym->st_shndx == SHN_UNDEF ) { /* ignore dynamic linker stubs */ 
                        continue;
                    }

                    name = elf_strptr( elf, shdr->sh_link, (size_t)sym->st_name );
                    if( name && !strcmp(symname, name) ) {
                        res = (void *)(sym->st_value - loadaddr + start);
                    }
                }
            }
            elf_end(elf);
        }
    }

    return res;
}
#endif /* HAVE_LIBELF_H */
