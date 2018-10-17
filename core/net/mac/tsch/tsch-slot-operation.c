/*
 * Copyright (c) 2015, SICS Swedish ICT.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

/**
 * \file
 *         TSCH slot operation implementation, running from interrupt.
 * \author
 *         Simon Duquennoy <simonduq@sics.se>
 *         Beshr Al Nahas <beshr@sics.se>
 *         Atis Elsts <atis.elsts@bristol.ac.uk>
 *
 */

#include "contiki.h"
#include "dev/radio.h"
#include "net/netstack.h"
#include "net/packetbuf.h"
#include "net/queuebuf.h"
#include "net/mac/framer-802154.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-slot-operation.h"
#include "net/mac/tsch/tsch-queue.h"
#include "net/mac/tsch/tsch-private.h"
#include "net/mac/tsch/tsch-log.h"
#include "net/mac/tsch/tsch-packet.h"
#include "net/mac/tsch/tsch-security.h"
#include "net/mac/tsch/tsch-adaptive-timesync.h"
#if CONTIKI_TARGET_COOJA || CONTIKI_TARGET_COOJA_IP64
#include "lib/simEnvChange.h"
#include "sys/cooja_mt.h"
#endif /* CONTIKI_TARGET_COOJA || CONTIKI_TARGET_COOJA_IP64 */

#if TSCH_LOG_LEVEL >= 1
#define DEBUG DEBUG_PRINT
#else /* TSCH_LOG_LEVEL */
#define DEBUG DEBUG_NONE
#endif /* TSCH_LOG_LEVEL */
#include "net/net-debug.h"

#include "ti-lib.h"
/* TSCH debug macros, i.e. to set LEDs or GPIOs on various TSCH
 * timeslot events */
#ifndef TSCH_DEBUG_INIT
#define TSCH_DEBUG_INIT()
#endif
#ifndef TSCH_DEBUG_RX_EVENT
#define TSCH_DEBUG_RX_EVENT()
#endif
#ifndef TSCH_DEBUG_TX_EVENT
#define TSCH_DEBUG_TX_EVENT()
#endif
#ifndef TSCH_DEBUG_SLOT_START
#define TSCH_DEBUG_SLOT_START()
#endif
#ifndef TSCH_DEBUG_SLOT_END
#define TSCH_DEBUG_SLOT_END()
#endif

/* Check if TSCH_MAX_INCOMING_PACKETS is power of two */
#if (TSCH_MAX_INCOMING_PACKETS & (TSCH_MAX_INCOMING_PACKETS - 1)) != 0
#error TSCH_MAX_INCOMING_PACKETS must be power of two
#endif

/* Check if TSCH_DEQUEUED_ARRAY_SIZE is power of two and greater or equal to QUEUEBUF_NUM */
#if TSCH_DEQUEUED_ARRAY_SIZE < QUEUEBUF_NUM
#error TSCH_DEQUEUED_ARRAY_SIZE must be greater or equal to QUEUEBUF_NUM
#endif
#if (TSCH_DEQUEUED_ARRAY_SIZE & (TSCH_DEQUEUED_ARRAY_SIZE - 1)) != 0
#error TSCH_DEQUEUED_ARRAY_SIZE must be power of two
#endif

/* Truncate received drift correction information to maximum half
 * of the guard time (one fourth of TSCH_DEFAULT_TS_RX_WAIT) */
#define SYNC_IE_BOUND ((int32_t)US_TO_RTIMERTICKS(TSCH_DEFAULT_TS_RX_WAIT / 4))

/* By default: check that rtimer runs at >=32kHz and use a guard time of 10us */
#if RTIMER_SECOND < (32 * 1024)
#error "TSCH: RTIMER_SECOND < (32 * 1024)"
#endif
#if CONTIKI_TARGET_COOJA || CONTIKI_TARGET_COOJA_IP64
/* Use 0 usec guard time for Cooja Mote with a 1 MHz Rtimer*/
#define RTIMER_GUARD 0u
#elif RTIMER_SECOND >= 200000
#define RTIMER_GUARD (RTIMER_SECOND / 100000)
#else
#define RTIMER_GUARD 2u
#endif

enum tsch_radio_state_on_cmd {
    TSCH_RADIO_CMD_ON_START_OF_TIMESLOT,
    TSCH_RADIO_CMD_ON_WITHIN_TIMESLOT,
    TSCH_RADIO_CMD_ON_FORCE,
};

enum tsch_radio_state_off_cmd {
    TSCH_RADIO_CMD_OFF_END_OF_TIMESLOT,
    TSCH_RADIO_CMD_OFF_WITHIN_TIMESLOT,
    TSCH_RADIO_CMD_OFF_FORCE,
};

/* A ringbuf storing outgoing packets after they were dequeued.
 * Will be processed layer by tsch_tx_process_pending */
struct ringbufindex dequeued_ringbuf;
struct tsch_packet *dequeued_array[TSCH_DEQUEUED_ARRAY_SIZE];
/* A ringbuf storing incoming packets.
 * Will be processed layer by tsch_rx_process_pending */
struct ringbufindex input_ringbuf;
struct input_packet input_array[TSCH_MAX_INCOMING_PACKETS];

/* Last time we received Sync-IE (ACK or data packet from a time source) */
static struct tsch_asn_t last_sync_asn;

/* A global lock for manipulating data structures safely from outside of interrupt */
static volatile int tsch_locked = 0;
/* As long as this is set, skip all slot operation */
static volatile int tsch_lock_requested = 0;

/* Last estimated drift in RTIMER ticks
 * (Sky: 1 tick = 30.517578125 usec exactly) */
static int32_t drift_correction = 0;
/* Is drift correction used? (Can be true even if drift_correction == 0) */
static uint8_t is_drift_correction_used;

/* The neighbor last used as our time source */
struct tsch_neighbor *last_timesource_neighbor = NULL;

/* Used from tsch_slot_operation and sub-protothreads */
static rtimer_clock_t volatile current_slot_start;

/* Are we currently inside a slot? */
static volatile int tsch_in_slot_operation = 0;

/* If we are inside a slot, this tells the current channel */
static uint8_t current_channel;

/* Info about the link, packet and neighbor of
 * the current (or next) slot */
struct tsch_link *current_link = NULL;
/* A backup link with Rx flag, overlapping with current_link.
 * If the current link is Tx-only and the Tx queue
 * is empty while executing the link, fallback to the backup link. */
static struct tsch_link *backup_link = NULL;
static struct tsch_packet *current_packet = NULL;
static struct tsch_neighbor *current_neighbor = NULL;

/* Protothread for association */
PT_THREAD(tsch_scan(struct pt *pt));
/* Protothread for slot operation, called from rtimer interrupt
 * and scheduled from tsch_schedule_slot_operation */
static PT_THREAD(tsch_slot_operation(struct rtimer *t, void *ptr));
static struct pt slot_operation_pt;
/* Sub-protothreads of tsch_slot_operation */
static PT_THREAD(tsch_tx_slot(struct pt *pt, struct rtimer *t));
static PT_THREAD(tsch_rx_slot(struct pt *pt, struct rtimer *t));

/*---------------------------------------------------------------------------*/
/* TSCH locking system. TSCH is locked during slot operations */

/* Is TSCH locked? */
int
tsch_is_locked(void)
{
    return tsch_locked;
}

/* Lock TSCH (no slot operation) */
int
tsch_get_lock(void)
{
    if(!tsch_locked) {
        rtimer_clock_t busy_wait_time;
        int busy_wait = 0; /* Flag used for logging purposes */
        /* Make sure no new slot operation will start */
        tsch_lock_requested = 1;
        /* Wait for the end of current slot operation. */
        if(tsch_in_slot_operation) {
            busy_wait = 1;
            busy_wait_time = RTIMER_NOW();
            while(tsch_in_slot_operation) {
#if CONTIKI_TARGET_COOJA || CONTIKI_TARGET_COOJA_IP64
                simProcessRunValue = 1;
                cooja_mt_yield();
#endif /* CONTIKI_TARGET_COOJA || CONTIKI_TARGET_COOJA_IP64 */
            }
            busy_wait_time = RTIMER_NOW() - busy_wait_time;
        }
        if(!tsch_locked) {
            /* Take the lock if it is free */
            tsch_locked = 1;
            tsch_lock_requested = 0;
            if(busy_wait) {
                /* Issue a log whenever we had to busy wait until getting the lock */
                TSCH_LOG_ADD(tsch_log_message,
                             snprintf(log->message, sizeof(log->message),
                                      "!get lock delay %u", (unsigned)busy_wait_time);
                );
            }
            return 1;
        }
    }
    TSCH_LOG_ADD(tsch_log_message,
                 snprintf(log->message, sizeof(log->message),
                          "!failed to lock");
    );
    return 0;
}

/* Release TSCH lock */
void
tsch_release_lock(void)
{
    tsch_locked = 0;
}

/*---------------------------------------------------------------------------*/
/* Channel hopping utility functions */

/* Return channel from ASN and channel offset */
uint8_t
tsch_calculate_channel(struct tsch_asn_t *asn, uint8_t channel_offset)
{
    uint16_t index_of_0 = TSCH_ASN_MOD(*asn, tsch_hopping_sequence_length);
    uint16_t index_of_offset = (index_of_0 + channel_offset) % tsch_hopping_sequence_length.val;
    return tsch_hopping_sequence[index_of_offset];
}

/*---------------------------------------------------------------------------*/
/* Timing utility functions */

/* Checks if the current time has passed a ref time + offset. Assumes
 * a single overflow and ref time prior to now. */
static uint8_t
check_timer_miss(rtimer_clock_t ref_time, rtimer_clock_t offset, rtimer_clock_t now)
{
    rtimer_clock_t target = ref_time + offset;
    int now_has_overflowed = now < ref_time;
    int target_has_overflowed = target < ref_time;

    if(now_has_overflowed == target_has_overflowed) {
        /* Both or none have overflowed, just compare now to the target */
        return target <= now;
    } else {
        /* Either now or target of overflowed.
         * If it is now, then it has passed the target.
         * If it is target, then we haven't reached it yet.
         *  */
        return now_has_overflowed;
    }
}
/*---------------------------------------------------------------------------*/
/* Schedule a wakeup at a specified offset from a reference time.
 * Provides basic protection against missed deadlines and timer overflows
 * A return value of zero signals a missed deadline: no rtimer was scheduled. */
static uint8_t
tsch_schedule_slot_operation(struct rtimer *tm, rtimer_clock_t ref_time, rtimer_clock_t offset, const char *str)
{
    rtimer_clock_t now = RTIMER_NOW();
    int r;
    /* Subtract RTIMER_GUARD before checking for deadline miss
     * because we can not schedule rtimer less than RTIMER_GUARD in the future */
    int missed = check_timer_miss(ref_time, offset - RTIMER_GUARD, now);

    if(missed) {
        /*TSCH_LOG_ADD(tsch_log_message,
                 snprintf(log->message, sizeof(log->message),
                          "!dl-miss %s %d %d",
                          str, (int)(now-ref_time), (int)offset);*/
        /*TSCH_LOG_ADD(tsch_log_message,
                     snprintf(log->message, sizeof(log->message),
                              "now %d ref %d off %d",
                              (int)now, (int)ref_time, (int)offset);
        );*/

        return 0;
    }
    ref_time += offset;
    r = rtimer_set(tm, ref_time, 1, (void (*)(struct rtimer *, void *))tsch_slot_operation, NULL);
    if(r != RTIMER_OK) {
        return 0;
    }
    return 1;
}
/*---------------------------------------------------------------------------*/
/* Schedule slot operation conditionally, and YIELD if success only.
 * Always attempt to schedule RTIMER_GUARD before the target to make sure to wake up
 * ahead of time and then busy wait to exactly hit the target. */
#define TSCH_SCHEDULE_AND_YIELD(pt, tm, ref_time, offset, str) \
        do { \
            if(tsch_schedule_slot_operation(tm, ref_time, offset - RTIMER_GUARD, str)) { \
                PT_YIELD(pt); \
            } \
            BUSYWAIT_UNTIL_ABS(0, ref_time, offset); \
        } while(0);
/*---------------------------------------------------------------------------*/
/* Get EB, broadcast or unicast packet to be sent, and target neighbor. */
static struct tsch_packet *
get_packet_and_neighbor_for_link(struct tsch_link *link, struct tsch_neighbor **target_neighbor)
{
    struct tsch_packet *p = NULL;
    struct tsch_neighbor *n = NULL;

    /* Is this a Tx link? */
    if(link->link_options & LINK_OPTION_TX) {
        /* is it for advertisement of EB? */
        if(link->link_type == LINK_TYPE_ADVERTISING || link->link_type == LINK_TYPE_ADVERTISING_ONLY) {
            /* fetch EB packets */
            n = n_eb;
            p = tsch_queue_get_packet_for_nbr(n, link);
        }
        if(link->link_type != LINK_TYPE_ADVERTISING_ONLY) {
            /* NORMAL link or no EB to send, pick a data packet */
            if(p == NULL) {
                /* Get neighbor queue associated to the link and get packet from it */
                n = tsch_queue_get_nbr(&link->addr);
                p = tsch_queue_get_packet_for_nbr(n, link);
                /* if it is a broadcast slot and there were no broadcast packets, pick any unicast packet */
                if(p == NULL && n == n_broadcast) {
                    p = tsch_queue_get_unicast_packet_for_any(&n, link);
                }
            }
        }
    }
    /* return nbr (by reference) */
    if(target_neighbor != NULL) {
        *target_neighbor = n;
    }

    return p;
}
/*---------------------------------------------------------------------------*/
/* Post TX: Update neighbor state after a transmission */
static int
update_neighbor_state(struct tsch_neighbor *n, struct tsch_packet *p,
                      struct tsch_link *link, uint8_t mac_tx_status)
{
    int in_queue = 1;
    int is_shared_link = link->link_options & LINK_OPTION_SHARED;
    int is_unicast = !n->is_broadcast;

    if(mac_tx_status == MAC_TX_OK) {
        /* Successful transmission */
        tsch_queue_remove_packet_from_queue(n);
        in_queue = 0;

        /* Update CSMA state in the unicast case */
        if(is_unicast) {
            if(is_shared_link || tsch_queue_is_empty(n)) {
                /* If this is a shared link, reset backoff on success.
                 * Otherwise, do so only is the queue is empty */
                tsch_queue_backoff_reset(n);
            }
        }
    } else {
        /* Failed transmission */
        if(p->transmissions >= TSCH_MAC_MAX_FRAME_RETRIES + 1) {
            /* Drop packet */
            tsch_queue_remove_packet_from_queue(n);
            in_queue = 0;
        }
        /* Update CSMA state in the unicast case */
        if(is_unicast) {
            /* Failures on dedicated (== non-shared) leave the backoff
             * window nor exponent unchanged */
            if(is_shared_link) {
                /* Shared link: increment backoff exponent, pick a new window */
                tsch_queue_backoff_inc(n);
            }
        }
    }

    return in_queue;
}
/*---------------------------------------------------------------------------*/
/**
 * This function turns on the radio. Its semantics is dependent on
 * the value of TSCH_RADIO_ON_DURING_TIMESLOT constant:
 * - if enabled, the radio is turned on at the start of the slot
 * - if disabled, the radio is turned on within the slot,
 *   directly before the packet Rx guard time and ACK Rx guard time.
 */
static void
tsch_radio_on(enum tsch_radio_state_on_cmd command)
{
    int do_it = 0;
    switch(command) {
    case TSCH_RADIO_CMD_ON_START_OF_TIMESLOT:
        if(TSCH_RADIO_ON_DURING_TIMESLOT) {
            do_it = 1;
        }
        break;
    case TSCH_RADIO_CMD_ON_WITHIN_TIMESLOT:
        if(!TSCH_RADIO_ON_DURING_TIMESLOT) {
            do_it = 1;
        }
        break;
    case TSCH_RADIO_CMD_ON_FORCE:
        do_it = 1;
        break;
    }
    if(do_it) {
        NETSTACK_RADIO.on();
    }
}
/*---------------------------------------------------------------------------*/
/**
 * This function turns off the radio. In the same way as for tsch_radio_on(),
 * it depends on the value of TSCH_RADIO_ON_DURING_TIMESLOT constant:
 * - if enabled, the radio is turned off at the end of the slot
 * - if disabled, the radio is turned off within the slot,
 *   directly after Tx'ing or Rx'ing a packet or Tx'ing an ACK.
 */
static void
tsch_radio_off(enum tsch_radio_state_off_cmd command)
{
    int do_it = 0;
    switch(command) {
    case TSCH_RADIO_CMD_OFF_END_OF_TIMESLOT:
        if(TSCH_RADIO_ON_DURING_TIMESLOT) {
            do_it = 1;
        }
        break;
    case TSCH_RADIO_CMD_OFF_WITHIN_TIMESLOT:
        if(!TSCH_RADIO_ON_DURING_TIMESLOT) {
            do_it = 1;
        }
        break;
    case TSCH_RADIO_CMD_OFF_FORCE:
        do_it = 1;
        break;
    }
    if(do_it) {
        NETSTACK_RADIO.off();
    }
}
/*---------------------------------------------------------------------------*/
static
PT_THREAD(tsch_tx_slot(struct pt *pt, struct rtimer *t))
{
    /**
     * TX slot:
     * 1. Copy packet to radio buffer
     * 2. Perform CCA if enabled
     * 3. Sleep until it is time to transmit
     * 4. Wait for ACK if it is a unicast packet
     * 5. Extract drift if we received an E-ACK from a time source neighbor
     * 6. Update CSMA parameters according to TX status
     * 7. Schedule mac_call_sent_callback
     **/

    /* tx status */
    static uint8_t mac_tx_status;
    static uint8_t beacon_id;
    /* is the packet in its neighbor's queue? */
    uint8_t in_queue;
    static int dequeued_index;
    static int packet_ready = 1;

    PT_BEGIN(pt);

    TSCH_DEBUG(TX_INIT);

    /* First check if we have space to store a newly dequeued packet (in case of
     * successful Tx or Drop) */
    dequeued_index = ringbufindex_peek_put(&dequeued_ringbuf);
    if(dequeued_index != -1) {
        if(current_packet == NULL || current_packet->qb == NULL) {
            mac_tx_status = MAC_TX_ERR_FATAL;
        } else {
            /* packet payload */
            static void *packet;
#if LLSEC802154_ENABLED
            /* encrypted payload */
            static uint8_t encrypted_packet[TSCH_PACKET_MAX_LEN];
#endif /* LLSEC802154_ENABLED */
            /* packet payload length */
            static uint8_t packet_len;
            /* packet seqno */
            static uint8_t seqno;
            /* is this a broadcast packet? (wait for ack?) */
            static uint8_t is_broadcast;
            static rtimer_clock_t tx_start_time;

#if CCA_ENABLED
            static uint8_t cca_status;
#endif

            /* get payload */
            packet = queuebuf_dataptr(current_packet->qb);
            packet_len = queuebuf_datalen(current_packet->qb);
            /* is this a broadcast packet? (wait for ack?) */
            is_broadcast = current_neighbor->is_broadcast;
            /* read seqno from payload */
            seqno = ((uint8_t *)(packet))[2];
            /* if this is an EB, then update its Sync-IE */
            if(current_neighbor == n_eb) {
                packet_ready = tsch_packet_update_eb(packet, packet_len, current_packet->tsch_sync_ie_offset);
            } else {
                packet_ready = 1;
            }

#if LLSEC802154_ENABLED
            if(tsch_is_pan_secured) {
                /* If we are going to encrypt, we need to generate the output in a separate buffer and keep
                 * the original untouched. This is to allow for future retransmissions. */
                int with_encryption = queuebuf_attr(current_packet->qb, PACKETBUF_ATTR_SECURITY_LEVEL) & 0x4;
                packet_len += tsch_security_secure_frame(packet, with_encryption ? encrypted_packet : packet, current_packet->header_len,
                        packet_len - current_packet->header_len, &tsch_current_asn);
                if(with_encryption) {
                    packet = encrypted_packet;
                }
            }
#endif /* LLSEC802154_ENABLED */

            /* prepare packet to send: copy to radio buffer */
            if(packet_ready && NETSTACK_RADIO.prepare(packet, packet_len) == 0) { /* 0 means success */
                static rtimer_clock_t tx_duration;

#if CCA_ENABLED
                cca_status = 1;
                /* delay before CCA */
                TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start, TS_CCA_OFFSET, "cca");
                TSCH_DEBUG_TX_EVENT();
                tsch_radio_on(TSCH_RADIO_CMD_ON_WITHIN_TIMESLOT);
                /* CCA */
                BUSYWAIT_UNTIL_ABS(!(cca_status |= NETSTACK_RADIO.channel_clear()),
                                   current_slot_start, TS_CCA_OFFSET + TS_CCA);
                TSCH_DEBUG_TX_EVENT();
                /* there is not enough time to turn radio off */
                /*  NETSTACK_RADIO.off(); */
                if(cca_status == 0) {
                    mac_tx_status = MAC_TX_COLLISION;
                } else
#endif /* CCA_ENABLED */
                {
#if !GUARD_BEACON
                    /* delay before TX */
                    TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start, tsch_timing[tsch_ts_tx_offset] - RADIO_DELAY_BEFORE_TX, "TxBeforeTx");/*Linha Original*/
                    TSCH_DEBUG_TX_EVENT();
                    /* send packet already in radio tx buffer */
                    mac_tx_status = NETSTACK_RADIO.transmit(packet_len);

#else /* !GUARD_BEACON */
                    /* send packet already in radio tx buffer */
                    if(!is_broadcast) //se nao for broadcast, envia apenas 1 vez
                    {
                        /* delay before TX */
                        TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start, tsch_timing[tsch_ts_tx_offset] - RADIO_DELAY_BEFORE_TX, "TxBeforeTx");
                        TSCH_DEBUG_TX_EVENT();
                        mac_tx_status = NETSTACK_RADIO.transmit(packet_len);
                    }
                    else
                    {
                        if(packet_len <= 1 && tsch_is_coordinator) //se for um GB envia 3x
                        {
                            static rtimer_clock_t now,before;
                            /* delay before TX */
                            before = RTIMER_NOW();
                            TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start, tsch_timing[tsch_ts_tx_offset] - RADIO_DELAY_BEFORE_TX - GUARD_BEACON_TIME, "TxBeforeTx");
                            now = RTIMER_NOW();
                            //PRINTF("Tempo dormindo: now %d bef %d\n",(int)(now), (int)(before));
                            TSCH_DEBUG_TX_EVENT();
                            packet_len++;
                            beacon_id = 0x11;
                            uint8_t *pointer = (uint8_t *)packet;
                            pointer[packet_len-1] = beacon_id;


                            if(NETSTACK_RADIO.prepare(packet, packet_len)==0)
                            {
                                before = RTIMER_NOW();
                                mac_tx_status = NETSTACK_RADIO.transmit(packet_len);
                                now = RTIMER_NOW();
                                //PRINTF("Tempo dormindo 1: now %d bef %d\n",(int)(now), (int)(before));
                                /*TSCH_LOG_ADD(tsch_log_message,
                                             snprintf(log->message, sizeof(log->message),
                                                      "TB 1 now %d bef %d", (int)now, (int)before));*/
                            }
                            packet = queuebuf_dataptr(current_packet->qb);
                            packet_len = queuebuf_datalen(current_packet->qb);
                            //atualiza o relógio enviado no 2º beacon
                            //packet_ready = tsch_packet_update_eb(packet, packet_len, current_packet->tsch_sync_ie_offset);
                            packet_len++;
                            //PRINTF("Posicao identificador beacon: %d\n",packet_len);
                            beacon_id = 0x22;
                            pointer = (uint8_t *)packet;
                            pointer[packet_len-1] = beacon_id;
                            //TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start, tsch_timing[tsch_ts_tx_offset] - RADIO_DELAY_BEFORE_TX+(now-before), "TxBeforeTx");
                            TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start, tsch_timing[tsch_ts_tx_offset] - RADIO_DELAY_BEFORE_TX, "TxBeforeTx");
                            if(NETSTACK_RADIO.prepare(packet, packet_len) == 0)
                            {
                                before = RTIMER_NOW();
                                mac_tx_status = NETSTACK_RADIO.transmit(packet_len);
                                now = RTIMER_NOW();
                                //PRINTF("Tempo dormindo 2: now %d bef %d\n",(int)(now), (int)(before));
                                //TSCH_LOG_ADD(tsch_log_message,
                                //             snprintf(log->message, sizeof(log->message),
                                //                      "TB 2 now %d bef %d", (int)now, (int)before));
                            }
                            packet = queuebuf_dataptr(current_packet->qb);
                            packet_len = queuebuf_datalen(current_packet->qb);
                            //atualiza o relógio enviado no 3º beacon
                            //packet_ready = tsch_packet_update_eb(packet, packet_len, current_packet->tsch_sync_ie_offset);
                            packet_len++;
                            beacon_id = 0x33;
                            pointer = (uint8_t *)packet;
                            pointer[packet_len-1] = beacon_id;
                            //TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start, tsch_timing[tsch_ts_tx_offset] - RADIO_DELAY_BEFORE_TX + GUARD_BEACON_TIME + (now-before), "TxBeforeTx");
                            TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start, tsch_timing[tsch_ts_tx_offset] - RADIO_DELAY_BEFORE_TX + GUARD_BEACON_TIME, "TxBeforeTx");
                            if(NETSTACK_RADIO.prepare(packet, packet_len) == 0)
                            {
                                before = RTIMER_NOW();
                                mac_tx_status = NETSTACK_RADIO.transmit(packet_len);
                                now = RTIMER_NOW();
                                //PRINTF("Tempo dormindo 3: now %d bef %d\n",(int)(now), (int)(before));
                                //TSCH_LOG_ADD(tsch_log_message,
                                //             snprintf(log->message, sizeof(log->message),
                                //                      "TB 3 now %d bef %d", (int)now, (int)before));
                            }

                        }
                        else //se for um broadcast qqer envia 1x
                        {
                            /* delay before TX */
                            TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start, tsch_timing[tsch_ts_tx_offset] - RADIO_DELAY_BEFORE_TX, "TxBeforeTx");
                            TSCH_DEBUG_TX_EVENT();
                            if(packet_len<=1)
                            {
                                packet = queuebuf_dataptr(current_packet->qb);
                                packet_len = queuebuf_datalen(current_packet->qb);
                                packet_len++;
                                beacon_id = 0x11;
                                uint8_t *pointer = (uint8_t *)packet;
                                pointer[packet_len-1] = beacon_id;
                            }
                            if(NETSTACK_RADIO.prepare(packet, packet_len) == 0)
                            {
                                mac_tx_status = NETSTACK_RADIO.transmit(packet_len);
                            }
                        }
                    }

#endif
                    /* Save tx timestamp */
                    tx_start_time = current_slot_start + tsch_timing[tsch_ts_tx_offset];
                    /* calculate TX duration based on sent packet len */
                    tx_duration = TSCH_PACKET_DURATION(packet_len);
                    /* limit tx_time to its max value */
                    tx_duration = MIN(tx_duration, tsch_timing[tsch_ts_max_tx]);
                    /* turn tadio off -- will turn on again to wait for ACK if needed */
                    tsch_radio_off(TSCH_RADIO_CMD_OFF_WITHIN_TIMESLOT);
                    TSCH_DEBUG(TS_TX_OFFSET_AFTER_TRANSMIT);
                    if(mac_tx_status == RADIO_TX_OK) {
                        if(!is_broadcast) {
                            uint8_t ackbuf[TSCH_PACKET_MAX_LEN];
                            int ack_len;
                            rtimer_clock_t ack_start_time;
                            int is_time_source;
                            struct ieee802154_ies ack_ies;
                            uint8_t ack_hdrlen;
                            frame802154_t frame;

#if TSCH_HW_FRAME_FILTERING
                            radio_value_t radio_rx_mode;
                            /* Entering promiscuous mode so that the radio accepts the enhanced ACK */
                            NETSTACK_RADIO.get_value(RADIO_PARAM_RX_MODE, &radio_rx_mode);
                            NETSTACK_RADIO.set_value(RADIO_PARAM_RX_MODE, radio_rx_mode & (~RADIO_RX_MODE_ADDRESS_FILTER));
#endif /* TSCH_HW_FRAME_FILTERING */
                            /* Unicast: wait for ack after tx: sleep until ack time */
                            TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start,
                                                    tsch_timing[tsch_ts_tx_offset] + tx_duration + tsch_timing[tsch_ts_rx_ack_delay] - RADIO_DELAY_BEFORE_RX, "TxBeforeAck");
                            TSCH_DEBUG(TS_RX_ACK_DELAY);
                            tsch_radio_on(TSCH_RADIO_CMD_ON_WITHIN_TIMESLOT);
                            /* Wait for ACK to come */
                            BUSYWAIT_UNTIL_ABS(NETSTACK_RADIO.receiving_packet(),
                                               tx_start_time, tx_duration + tsch_timing[tsch_ts_rx_ack_delay] + tsch_timing[tsch_ts_ack_wait] + RADIO_DELAY_BEFORE_DETECT);
                            TSCH_DEBUG(TS_ACK_WAIT);

                            ack_start_time = RTIMER_NOW() - RADIO_DELAY_BEFORE_DETECT;

                            /* Wait for ACK to finish */
                            BUSYWAIT_UNTIL_ABS(!NETSTACK_RADIO.receiving_packet(),
                                               ack_start_time, tsch_timing[tsch_ts_max_ack]);
                            TSCH_DEBUG(ACK_RECEIVED);
                            tsch_radio_off(TSCH_RADIO_CMD_OFF_WITHIN_TIMESLOT);
                            TSCH_DEBUG(RADIO_OFF_AFTER_ACK_RECEIVED);
#if TSCH_HW_FRAME_FILTERING
                            /* Leaving promiscuous mode */
                            NETSTACK_RADIO.get_value(RADIO_PARAM_RX_MODE, &radio_rx_mode);
                            NETSTACK_RADIO.set_value(RADIO_PARAM_RX_MODE, radio_rx_mode | RADIO_RX_MODE_ADDRESS_FILTER);
#endif /* TSCH_HW_FRAME_FILTERING */

                            /* Read ack frame */
                            ack_len = NETSTACK_RADIO.read((void *)ackbuf, sizeof(ackbuf));

                            is_time_source = 0;
                            /* The radio driver should return 0 if no valid packets are in the rx buffer */
                            if(ack_len > 0) {
                                is_time_source = current_neighbor != NULL && current_neighbor->is_time_source;
                                if(tsch_packet_parse_eack(ackbuf, ack_len, seqno,
                                                          &frame, &ack_ies, &ack_hdrlen) == 0) {
                                    ack_len = 0;
                                }

#if LLSEC802154_ENABLED
                                if(ack_len != 0) {
                                    if(!tsch_security_parse_frame(ackbuf, ack_hdrlen, ack_len - ack_hdrlen - tsch_security_mic_len(&frame),
                                                                  &frame, &current_neighbor->addr, &tsch_current_asn)) {
                                        TSCH_LOG_ADD(tsch_log_message,
                                                     snprintf(log->message, sizeof(log->message),
                                                              "!failed to authenticate ACK"));
                                        ack_len = 0;
                                    }
                                } else {
                                    TSCH_LOG_ADD(tsch_log_message,
                                                 snprintf(log->message, sizeof(log->message),
                                                          "!failed to parse ACK"));
                                }
#endif /* LLSEC802154_ENABLED */
                            }

                            if(ack_len != 0) {
                                if(is_time_source) {
                                    int32_t eack_time_correction = US_TO_RTIMERTICKS(ack_ies.ie_time_correction);
                                    int32_t since_last_timesync = TSCH_ASN_DIFF(tsch_current_asn, last_sync_asn);
                                    if(eack_time_correction > SYNC_IE_BOUND) {
                                        drift_correction = SYNC_IE_BOUND;
                                    } else if(eack_time_correction < -SYNC_IE_BOUND) {
                                        drift_correction = -SYNC_IE_BOUND;
                                    } else {
                                        drift_correction = eack_time_correction;
                                    }
                                    //não corrige drift com ack, somente com EB, para possibilitar
                                    //avaliação do Guard Beacon
                                    drift_correction = 0;
                                    if(drift_correction != eack_time_correction) {
                                        TSCH_LOG_ADD(tsch_log_message,
                                                     snprintf(log->message, sizeof(log->message),
                                                              "!truncated dr %d %d", (int)eack_time_correction, (int)drift_correction);
                                        );
                                    }
                                    is_drift_correction_used = 1;
                                    tsch_timesync_update(current_neighbor, since_last_timesync, drift_correction);
                                    /* Keep track of sync time */
                                    last_sync_asn = tsch_current_asn;
                                    tsch_schedule_keepalive();
                                }
                                mac_tx_status = MAC_TX_OK;
                            } else {
                                mac_tx_status = MAC_TX_NOACK;
                            }
                        } else {
                            mac_tx_status = MAC_TX_OK;
                        }
                    } else {
                        mac_tx_status = MAC_TX_ERR;
                    }
                }
            }
        }

        tsch_radio_off(TSCH_RADIO_CMD_OFF_END_OF_TIMESLOT);
        TSCH_DEBUG(RADIO_OFF_END_TX_SLOT);
        current_packet->transmissions++;
        current_packet->ret = mac_tx_status;

        /* Post TX: Update neighbor state */
        in_queue = update_neighbor_state(current_neighbor, current_packet, current_link, mac_tx_status);

        /* The packet was dequeued, add it to dequeued_ringbuf for later processing */
        if(in_queue == 0) {
            dequeued_array[dequeued_index] = current_packet;
            ringbufindex_put(&dequeued_ringbuf);
        }

        /* Log every tx attempt */
        TSCH_LOG_ADD(tsch_log_tx,
                     log->tx.mac_tx_status = mac_tx_status;
        log->tx.num_tx = current_packet->transmissions;
        log->tx.datalen = queuebuf_datalen(current_packet->qb);
        log->tx.drift = RTIMERTICKS_TO_US(drift_correction);
        log->tx.drift_used = is_drift_correction_used;
        log->tx.is_data = ((((uint8_t *)(queuebuf_dataptr(current_packet->qb)))[0]) & 7) == FRAME802154_DATAFRAME;
#if LLSEC802154_ENABLED
        log->tx.sec_level = queuebuf_attr(current_packet->qb, PACKETBUF_ATTR_SECURITY_LEVEL);
#else /* LLSEC802154_ENABLED */
        log->tx.sec_level = 0;
#endif /* LLSEC802154_ENABLED */
        log->tx.dest = TSCH_LOG_ID_FROM_LINKADDR(queuebuf_addr(current_packet->qb, PACKETBUF_ADDR_RECEIVER));
        );

        /* Poll process for later processing of packet sent events and logs */
        process_poll(&tsch_pending_events_process);
    }

    TSCH_DEBUG(TX_END);

    PT_END(pt);
}
/*---------------------------------------------------------------------------*/
static
PT_THREAD(tsch_rx_slot(struct pt *pt, struct rtimer *t))
{
    /**
     * RX slot:
     * 1. Check if it is used for TIME_KEEPING
     * 2. Sleep and wake up just before expected RX time (with a guard time: TS_LONG_GT)
     * 3. Check for radio activity for the guard time: TS_LONG_GT
     * 4. Prepare and send ACK if needed
     * 5. Drift calculated in the ACK callback registered with the radio driver. Use it if receiving from a time source neighbor.
     **/

    struct tsch_neighbor *n;
    static linkaddr_t source_address;
    static linkaddr_t destination_address;
    static int16_t input_index;
    static int input_queue_drop = 0;
    static uint8_t is_gb;
    static uint8_t is_eb;

    PT_BEGIN(pt);

    TSCH_DEBUG(RX_INIT);

    input_index = ringbufindex_peek_put(&input_ringbuf);
    if(input_index == -1) {
        input_queue_drop++;
    } else {
        static struct input_packet *current_input;
        /* Estimated drift based on RX time */
        static int32_t estimated_drift;
        /* Rx timestamps */
        static rtimer_clock_t rx_start_time;
        static rtimer_clock_t expected_rx_time;
        static rtimer_clock_t packet_duration;
        uint8_t packet_seen;

        expected_rx_time = current_slot_start + tsch_timing[tsch_ts_tx_offset];
        /* Default start time: expected Rx time */
        rx_start_time = expected_rx_time;

        current_input = &input_array[input_index];

        while(NETSTACK_RADIO.pending_packet())
        {
            NETSTACK_RADIO.read((void *)current_input->payload, TSCH_PACKET_MAX_LEN);
        }
        /* Wait before starting to listen */
        TSCH_SCHEDULE_AND_YIELD(pt, t, current_slot_start, tsch_timing[tsch_ts_rx_offset] - RADIO_DELAY_BEFORE_RX, "RxBeforeListen");
        TSCH_DEBUG(TS_RX_OFFSET);

        /* Start radio for at least guard time */
        tsch_radio_on(TSCH_RADIO_CMD_ON_WITHIN_TIMESLOT);
        packet_seen = NETSTACK_RADIO.receiving_packet() || NETSTACK_RADIO.pending_packet();
        if(!packet_seen) {
            /* Check if receiving within guard time */
            BUSYWAIT_UNTIL_ABS((packet_seen = NETSTACK_RADIO.receiving_packet()),
                               current_slot_start, tsch_timing[tsch_ts_rx_offset] + tsch_timing[tsch_ts_rx_wait] + RADIO_DELAY_BEFORE_DETECT);
            packet_seen = NETSTACK_RADIO.receiving_packet() || NETSTACK_RADIO.pending_packet();
        }
        if(!packet_seen) {
            /* no packets on air */
            TSCH_DEBUG(RX_IDLE);
            tsch_radio_off(TSCH_RADIO_CMD_OFF_FORCE);
            TSCH_DEBUG(RX_IDLE_RX_OFF);
        } else {
            TSCH_DEBUG(PACKET_DETECTED);
            /* Save packet timestamp */
            rx_start_time = RTIMER_NOW() - RADIO_DELAY_BEFORE_DETECT;

            /* Wait until packet is received, turn radio off */
            BUSYWAIT_UNTIL_ABS(!NETSTACK_RADIO.receiving_packet(),
                               current_slot_start, tsch_timing[tsch_ts_rx_offset] + tsch_timing[tsch_ts_rx_wait] + tsch_timing[tsch_ts_max_tx]);
            TSCH_DEBUG(PACKET_RECEIVED);
            tsch_radio_off(TSCH_RADIO_CMD_OFF_WITHIN_TIMESLOT);
            TSCH_DEBUG(RX_OFF_AFTER_PACKET_RECEIVED);
            if(NETSTACK_RADIO.pending_packet()) {
                static int frame_valid;
                static int header_len;
                static frame802154_t frame;
                radio_value_t radio_last_rssi;

                /* Read packet */

#if !GUARD_BEACON /*Inserido Marcos 03/09*/

                current_input->len = NETSTACK_RADIO.read((void *)current_input->payload, TSCH_PACKET_MAX_LEN);

#else /* !GUARD_BEACON */    /*Inserido Marcos 03/09*/

                current_input->len = NETSTACK_RADIO.read((void *)current_input->payload, TSCH_PACKET_MAX_LEN);

                is_gb = *(current_input->payload+current_input->len-2) == GUARD_BEACON_FRAME;

                if(is_gb)
                {
                    TSCH_LOG_ADD(tsch_log_message,
                                 snprintf(log->message, sizeof(log->message),
                                          "Eh guard beacon: %d", is_gb));

                    TSCH_LOG_ADD(tsch_log_message,
                                 snprintf(log->message, sizeof(log->message),
                                          "Numero do beacon: %.2X", *(current_input->payload+current_input->len-1)));
                    //printf("Eh guard beacon: %d\n", is_gb);
                    //printf("Numero do beacon: %.2X\n", *(current_input->payload+current_input->len-1));
                    current_input->len -= 1;
                    frame.fcf.ack_required = 0;
                }

#endif  /*Inserido Marcos 03/09*/
                if(!is_gb)
                {
                    uint8_t ret = frame802154_parse(current_input->payload, current_input->len, &frame);
                    is_eb = ret
                            && frame.fcf.frame_version == FRAME802154_IEEE802154E_2012
                            && frame.fcf.frame_type == FRAME802154_BEACONFRAME;
                }

                NETSTACK_RADIO.get_value(RADIO_PARAM_LAST_RSSI, &radio_last_rssi);
                current_input->rx_asn = tsch_current_asn;
                current_input->rssi = (signed)radio_last_rssi;
                current_input->channel = current_channel;
                if(!is_gb)
                {
                    header_len = frame802154_parse((uint8_t *)current_input->payload, current_input->len, &frame);
                    frame_valid = header_len > 0 &&
                            frame802154_check_dest_panid(&frame) &&
                            frame802154_extract_linkaddr(&frame, &source_address, &destination_address);
                }

                if(is_eb)
                {
                    TSCH_LOG_ADD(tsch_log_message,
                                 snprintf(log->message, sizeof(log->message),
                                          "Eh beacon: %d", is_eb));
                    //printf("Eh beacon: %d\n", is_eb);
                }

#if TSCH_RESYNC_WITH_SFD_TIMESTAMPS
                /* At the end of the reception, get an more accurate estimate of SFD arrival time */
                NETSTACK_RADIO.get_object(RADIO_PARAM_LAST_PACKET_TIMESTAMP, &rx_start_time, sizeof(rtimer_clock_t));
#endif

                packet_duration = TSCH_PACKET_DURATION(current_input->len);

#if LLSEC802154_ENABLED
                /* Decrypt and verify incoming frame */
                if(frame_valid) {
                    if(tsch_security_parse_frame(
                            current_input->payload, header_len, current_input->len - header_len - tsch_security_mic_len(&frame),
                            &frame, &source_address, &tsch_current_asn)) {
                        current_input->len -= tsch_security_mic_len(&frame);
                    } else {
                        TSCH_LOG_ADD(tsch_log_message,
                                     snprintf(log->message, sizeof(log->message),
                                              "!failed to authenticate frame %u", current_input->len));
                        frame_valid = 0;
                    }
                } else {
                    TSCH_LOG_ADD(tsch_log_message,
                                 snprintf(log->message, sizeof(log->message),
                                          "!failed to parse frame %u %u", header_len, current_input->len));
                    frame_valid = 0;
                }
#endif /* LLSEC802154_ENABLED */

                if(frame_valid || is_gb) {
                    if(linkaddr_cmp(&destination_address, &linkaddr_node_addr)
                            || linkaddr_cmp(&destination_address, &linkaddr_null)) {
                        int do_nack = 0;
                        estimated_drift = RTIMER_CLOCK_DIFF(expected_rx_time, rx_start_time);

#if TSCH_TIMESYNC_REMOVE_JITTER
                        /* remove jitter due to measurement errors */
                        if(ABS(estimated_drift) <= TSCH_TIMESYNC_MEASUREMENT_ERROR) {
                            estimated_drift = 0;
                        } else if(estimated_drift > 0) {
                            estimated_drift -= TSCH_TIMESYNC_MEASUREMENT_ERROR;
                        } else {
                            estimated_drift += TSCH_TIMESYNC_MEASUREMENT_ERROR;
                        }
#endif

#ifdef TSCH_CALLBACK_DO_NACK
                        if(frame.fcf.ack_required) {
                            do_nack = TSCH_CALLBACK_DO_NACK(current_link,
                                                            &source_address, &destination_address);
                        }
#endif

                        if(frame.fcf.ack_required) {
                            static uint8_t ack_buf[TSCH_PACKET_MAX_LEN];
                            static int ack_len;

                            /* Build ACK frame */
                            ack_len = tsch_packet_create_eack(ack_buf, sizeof(ack_buf),
                                                              &source_address, frame.seq, (int16_t)RTIMERTICKS_TO_US(estimated_drift), do_nack);

                            if(ack_len > 0) {
#if LLSEC802154_ENABLED
                                if(tsch_is_pan_secured) {
                                    /* Secure ACK frame. There is only header and header IEs, therefore data len == 0. */
                                    ack_len += tsch_security_secure_frame(ack_buf, ack_buf, ack_len, 0, &tsch_current_asn);
                                }
#endif /* LLSEC802154_ENABLED */

                                /* Copy to radio buffer */
                                NETSTACK_RADIO.prepare((const void *)ack_buf, ack_len);

                                /* Wait for time to ACK and transmit ACK */
                                TSCH_SCHEDULE_AND_YIELD(pt, t, rx_start_time,
                                                        packet_duration + tsch_timing[tsch_ts_tx_ack_delay] - RADIO_DELAY_BEFORE_TX, "RxBeforeAck");
                                TSCH_DEBUG(RX_ACK_SEND);
                                NETSTACK_RADIO.transmit(ack_len);
                                tsch_radio_off(TSCH_RADIO_CMD_OFF_WITHIN_TIMESLOT);
                            }
                        }

                        /*uint8_t ret = frame802154_parse(current_input->payload, current_input->len, &frame);
                        is_eb = ret
                                && frame.fcf.frame_version == FRAME802154_IEEE802154E_2012
                                && frame.fcf.frame_type == FRAME802154_BEACONFRAME;*/

#if !GUARD_BEACON /*Inserido Marcos 03/09*/
                        n = tsch_queue_get_nbr(&source_address);

                        if(n != NULL && n->is_time_source && is_eb) {
                            int32_t since_last_timesync = TSCH_ASN_DIFF(tsch_current_asn, last_sync_asn);

                            /* Keep track of last sync time */
                            last_sync_asn = tsch_current_asn;
                            /* Save estimated drift */

                            drift_correction = -estimated_drift;

                            is_drift_correction_used = 1;
                            tsch_timesync_update(n, since_last_timesync, -estimated_drift);
                            tsch_schedule_keepalive();

                        }

#else
                        if(is_gb && !tsch_is_coordinator)
                        {
                            uint8_t beaconOrder = *(current_input->payload+current_input->len);
                            if(beaconOrder==0x11)
                            {
                                estimated_drift -= GUARD_BEACON_TIME;
                            }
                            else if(beaconOrder==0x22)
                            {
                                estimated_drift = estimated_drift;
                            }
                            else if(beaconOrder==0x33)
                            {
                                estimated_drift += GUARD_BEACON_TIME;
                            }
                        }


                        n = tsch_queue_get_nbr(&source_address);
                        if(n != NULL && n->is_time_source && is_gb)
                        {
                            int32_t since_last_timesync = TSCH_ASN_DIFF(tsch_current_asn, last_sync_asn);

                            if(since_last_timesync>21)
                            {
                                /* Keep track of last sync time */
                                last_sync_asn = tsch_current_asn;
                                /* Save estimated drift */

                                drift_correction = -estimated_drift;

                                is_drift_correction_used = 1;
                                tsch_timesync_update(n, since_last_timesync, -estimated_drift);
                                tsch_schedule_keepalive();
                            }
                        }
#endif


                        /* Add current input to ringbuf */
                        ringbufindex_put(&input_ringbuf);

                        /* Log every reception */
                        TSCH_LOG_ADD(tsch_log_rx,
                                     log->rx.src = TSCH_LOG_ID_FROM_LINKADDR((linkaddr_t*)&frame.src_addr);
                        log->rx.is_unicast = frame.fcf.ack_required;
                        log->rx.datalen = current_input->len;
                        log->rx.drift = RTIMERTICKS_TO_US(drift_correction);
                        log->rx.drift_used = is_drift_correction_used;
                        log->rx.is_data = frame.fcf.frame_type == FRAME802154_DATAFRAME;
                        log->rx.sec_level = frame.aux_hdr.security_control.security_level;
                        log->rx.estimated_drift = RTIMERTICKS_TO_US(estimated_drift);
                        );
                    }

                    /* Poll process for processing of pending input and logs */
                    process_poll(&tsch_pending_events_process);
                }
            }

            tsch_radio_off(TSCH_RADIO_CMD_OFF_END_OF_TIMESLOT);
        }

        if(input_queue_drop != 0) {
            TSCH_LOG_ADD(tsch_log_message,
                         snprintf(log->message, sizeof(log->message),
                                  "!queue full skipped %u", input_queue_drop);
            );
            input_queue_drop = 0;
        }
    }

    TSCH_DEBUG(RX_END);

    PT_END(pt);
}
/*---------------------------------------------------------------------------*/
/* Protothread for slot operation, called from rtimer interrupt
 * and scheduled from tsch_schedule_slot_operation */
static
PT_THREAD(tsch_slot_operation(struct rtimer *t, void *ptr))
{
    PT_BEGIN(&slot_operation_pt);
    /* Loop over all active slots */
    while(tsch_is_associated) {

        if(current_link == NULL || tsch_lock_requested) { /* Skip slot operation if there is no link
                                                          or if there is a pending request for getting the lock */
            /* Issue a log whenever skipping a slot */
            TSCH_LOG_ADD(tsch_log_message,
                         snprintf(log->message, sizeof(log->message),
                                  "!skipped slot %u %u %u",
                                  tsch_locked,
                                  tsch_lock_requested,
                                  current_link == NULL);
            );

        } else {
            int is_active_slot;
            TSCH_DEBUG(SLOT_START);
            tsch_in_slot_operation = 1;
            /* Reset drift correction */
            drift_correction = 0;
            is_drift_correction_used = 0;
            /* Get a packet ready to be sent */
            current_packet = get_packet_and_neighbor_for_link(current_link, &current_neighbor);
            /* There is no packet to send, and this link does not have Rx flag. Instead of doing
             * nothing, switch to the backup link (has Rx flag) if any. */
            if(current_packet == NULL && !(current_link->link_options & LINK_OPTION_RX) && backup_link != NULL) {
                current_link = backup_link;
                current_packet = get_packet_and_neighbor_for_link(current_link, &current_neighbor);
            }
            is_active_slot = current_packet != NULL || (current_link->link_options & LINK_OPTION_RX);
            if(is_active_slot) {
                /* Hop channel */
                current_channel = tsch_calculate_channel(&tsch_current_asn, current_link->channel_offset);
                //indice = (indice+1)%tsch_hopping_sequence_length.val;
                //current_channel = tsch_hopping_sequence[indice];
                NETSTACK_RADIO.set_value(RADIO_PARAM_CHANNEL, current_channel);
                /* Turn the radio on already here if configured so; necessary for radios with slow startup */

                TSCH_DEBUG(SLOT_START_TURN_RADIO_ON);
                tsch_radio_on(TSCH_RADIO_CMD_ON_START_OF_TIMESLOT);

                TSCH_DEBUG(SLOT_START_RADIO_IS_ON);
                /* Decide whether it is a TX/RX/IDLE or OFF slot */
                /* Actual slot operation */
                if(current_packet != NULL) {
                    /* We have something to transmit, do the following:
                     * 1. send
                     * 2. update_backoff_state(current_neighbor)
                     * 3. post tx callback
                     **/
                    static struct pt slot_tx_pt;
                    PT_SPAWN(&slot_operation_pt, &slot_tx_pt, tsch_tx_slot(&slot_tx_pt, t));
                } else {
                    /* Listen */
                    static struct pt slot_rx_pt;
                    PT_SPAWN(&slot_operation_pt, &slot_rx_pt, tsch_rx_slot(&slot_rx_pt, t));
                }
                TSCH_DEBUG(SLOT_END);
            }
            //TSCH_DEBUG_SLOT_END();
        }

        /* End of slot operation, schedule next slot or resynchronize */

        /* Do we need to resynchronize? i.e., wait for EB again */
        if(!tsch_is_coordinator && (TSCH_ASN_DIFF(tsch_current_asn, last_sync_asn) >
        (100 * TSCH_CLOCK_TO_SLOTS(TSCH_DESYNC_THRESHOLD / 100, tsch_timing[tsch_ts_timeslot_length])))) {
            TSCH_LOG_ADD(tsch_log_message,
                         snprintf(log->message, sizeof(log->message),
                                  "! leaving the network, last sync %u",
                                  (unsigned)TSCH_ASN_DIFF(tsch_current_asn, last_sync_asn));
            );
            last_timesource_neighbor = NULL;
            tsch_disassociate();
        } else {
            /* backup of drift correction for printing debug messages */
            /* int32_t drift_correction_backup = drift_correction; */
            uint16_t timeslot_diff = 0;
            rtimer_clock_t prev_slot_start;
            /* Time to next wake up */
            rtimer_clock_t time_to_next_active_slot;
            TSCH_DEBUG(SLOT_SCHEDULE);
            /* Schedule next wakeup skipping slots if missed deadline */
            do {
                if(current_link != NULL
                        && current_link->link_options & LINK_OPTION_TX
                        && current_link->link_options & LINK_OPTION_SHARED) {
                    /* Decrement the backoff window for all neighbors able to transmit over
                     * this Tx, Shared link. */
                    tsch_queue_update_all_backoff_windows(&current_link->addr);
                }

                /* Get next active link */
                current_link = tsch_schedule_get_next_active_link(&tsch_current_asn, &timeslot_diff, &backup_link);
                if(current_link == NULL) {
                    /* There is no next link. Fall back to default
                     * behavior: wake up at the next slot. */
                    timeslot_diff = 1;
                }
                /* Update ASN */
                TSCH_ASN_INC(tsch_current_asn, timeslot_diff);
                /* Time to next wake up */
                time_to_next_active_slot = timeslot_diff * tsch_timing[tsch_ts_timeslot_length] + drift_correction;
                drift_correction = 0;
                is_drift_correction_used = 0;
                /* Update current slot start */
                prev_slot_start = current_slot_start;
                current_slot_start += time_to_next_active_slot;
                current_slot_start += tsch_timesync_adaptive_compensate(time_to_next_active_slot);
            } while(!tsch_schedule_slot_operation(t, prev_slot_start, time_to_next_active_slot, "main"));
            TSCH_DEBUG(SLOT_OPERATION_END);
        }

        tsch_in_slot_operation = 0;
        PT_YIELD(&slot_operation_pt);
    }

    PT_END(&slot_operation_pt);
}
/*---------------------------------------------------------------------------*/
/* Set global time before starting slot operation,
 * with a rtimer time and an ASN */
void
tsch_slot_operation_start(void)
{
    static struct rtimer slot_operation_timer;
    rtimer_clock_t time_to_next_active_slot;
    rtimer_clock_t prev_slot_start;
    TSCH_DEBUG_INIT();
    do {
        uint16_t timeslot_diff;
        /* Get next active link */
        current_link = tsch_schedule_get_next_active_link(&tsch_current_asn, &timeslot_diff, &backup_link);
        if(current_link == NULL) {
            /* There is no next link. Fall back to default
             * behavior: wake up at the next slot. */
            timeslot_diff = 1;
        }
        /* Update ASN */
        TSCH_ASN_INC(tsch_current_asn, timeslot_diff);
        /* Time to next wake up */
        time_to_next_active_slot = timeslot_diff * tsch_timing[tsch_ts_timeslot_length];
        /* Update current slot start */
        prev_slot_start = current_slot_start;
        current_slot_start += time_to_next_active_slot;
    } while(!tsch_schedule_slot_operation(&slot_operation_timer, prev_slot_start, time_to_next_active_slot, "association"));
}
/*---------------------------------------------------------------------------*/
/* Start actual slot operation */
void
tsch_slot_operation_sync(rtimer_clock_t next_slot_start,
                         struct tsch_asn_t *next_slot_asn)
{
    current_slot_start = next_slot_start;
    tsch_current_asn = *next_slot_asn;
    last_sync_asn = tsch_current_asn;
    current_link = NULL;
}
/*---------------------------------------------------------------------------*/
