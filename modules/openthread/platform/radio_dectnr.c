/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 *   This file implements the OpenThread platform abstraction
 *   for radio communication.
 *
 */

#define LOG_MODULE_NAME net_otPlat_radio_dectnr

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME, CONFIG_OPENTHREAD_L2_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/ieee802154_radio.h>
#include <zephyr/net/net_pkt.h>
#include <openthread/ip6.h>
#include <openthread-system.h>
#include <openthread/instance.h>
#include <openthread/platform/radio.h>
#include <openthread/platform/time.h>
#include <openthread/thread.h>

#include "ot_dectnr.h"
#include <nrf_modem_dect_phy.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/drivers/hwinfo.h>

#include <zephyr/random/random.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/openthread.h>

#if defined(CONFIG_OPENTHREAD_NAT64_TRANSLATOR)
#include <openthread/nat64.h>
#endif

#define PKT_IS_IPv6(_p) ((NET_IPV6_HDR(_p)->vtc & 0xf0) == 0x60)

#if defined(CONFIG_NET_TC_THREAD_COOPERATIVE)
#define OT_WORKER_PRIORITY K_PRIO_COOP(CONFIG_OPENTHREAD_THREAD_PRIORITY)
#else
#define OT_WORKER_PRIORITY K_PRIO_PREEMPT(CONFIG_OPENTHREAD_THREAD_PRIORITY)
#endif

enum pending_events {
    PENDING_EVENT_FRAME_TO_SEND,  /* There is a tx frame to send */
    PENDING_EVENT_FRAME_RECEIVED, /* Radio has received new frame */
    PENDING_EVENT_RX_FAILED,      /* The RX failed */
    PENDING_EVENT_DECT_IDLE,      /* DECT Radio is ready for next operation */
    PENDING_EVENT_TX_DONE,        /* Radio transmission finished */
    PENDING_EVENT_COUNT           /* Keep last */
};

ATOMIC_DEFINE(pending_events, PENDING_EVENT_COUNT);

/* Semaphore to synchronize modem calls. */
K_SEM_DEFINE(modem_operation_sem, 0, 1);
K_FIFO_DEFINE(rx_pkt_fifo);
K_FIFO_DEFINE(tx_pkt_fifo);
K_FIFO_DEFINE(dect_tx_fifo);

#define OPENTHREAD_MTU            1280

static struct net_pkt *net_tx_pkt;
static struct net_buf *net_tx_buf;

static struct nrf_modem_dect_phy_tx_params harq_feedback_tx_params;
static struct dect_phy_header_type2_format1_t harq_feedback_header;

/* Radio capabilities and state declarations for OpenThread */
static otRadioCaps  ot_radio_caps = OT_RADIO_CAPS_RX_ON_WHEN_IDLE | OT_RADIO_CAPS_ACK_TIMEOUT | OT_RADIO_CAPS_TRANSMIT_RETRIES | OT_RADIO_CAPS_SLEEP_TO_TX | OT_RADIO_CAPS_ENERGY_SCAN;
static otRadioState ot_state = OT_RADIO_STATE_DISABLED;
static otRadioFrame ot_transmit_frame;
static otPanId      ot_pan_id;
static otError      ot_rx_result;
static uint8_t      ot_channel;
static int8_t       ot_tx_power = CONFIG_OPENTHREAD_DEFAULT_TX_POWER;

/* structure for buffering received frame */
typedef struct ot_dectnr_rx_frame_t {
    void *fifo_reserved; /* 1st word reserved for use by fifo. */
    enum ot_dectnr_rx_frame_status status; /* Frame status */
    struct nrf_modem_dect_phy_pcc_event pcc_info;
    uint8_t length;
    uint8_t data[DECT_DATA_MAX_LEN];
    int16_t rssi_2;
    int16_t snr;
    uint64_t time; /* RX timestamp. */
} ot_dectnr_rx_frame;

struct openthread_over_dectnr_phy_ctx {
    /* OpenThread over DECT NR network interface */
    struct net_if *iface;

    /* DECT NR+ Radio state */
    enum ot_dectnr_radio_state radio_state;

    /* EUI-64 of the radio device */
    uint8_t eui64[8];

    /* Last received PCC event */
    struct nrf_modem_dect_phy_pcc_event last_pcc_event;

    /* Result of last DECT NR operation */
    enum nrf_modem_dect_phy_err last_dect_op_result;

    /* Modem time of last received DECT NR event, in modem time units. */
    uint64_t last_modem_event_time;

    /* Last RSSI of received DECT NR PHY data */
    int8_t last_rssi;

    /* Address mapping from OT to DECT NR+ for broadcasting */
    ot_dectnr_address_mapping_t ot_addr_map;

    /* OT DECT PEER DEVICE TABLE */
    struct ot_dectnr_peer_device peer_devices[CONFIG_OPENTHREAD_MAX_CHILDREN];

    /* Work to send address mapping beacon */
    struct k_work_delayable address_mapping_beacon_work;

    /* RX thread stack. */
    K_KERNEL_STACK_MEMBER(rx_stack, CONFIG_DECT_RX_STACK_SIZE);

    /* RX thread control block. */
    struct k_thread rx_thread;

    /* RX fifo queue. */
    struct k_fifo rx_fifo;

    /* Buffers for passing received frame pointers and data to the
     * RX thread via rx_fifo object.
     */
    ot_dectnr_rx_frame rx_frames[CONFIG_OT_DECT_RX_BUFFERS];
};

static struct openthread_over_dectnr_phy_ctx ot_dectnr_ctx;

/*
 * DECT PHY TX process information
 */
struct dect_tx_process_info {
    /* 1st word reserved for use by fifo. */
    void *fifo_reserved;
    bool tx_in_progress;
    uint16_t dect_receiver_device_id; /* DECT receiver device ID */
    uint8_t process_nbr;
    uint8_t data[DECT_DATA_MAX_LEN]; /* TX Buffer for DECT PHY transmition */
    uint16_t dect_data_size;
    bool ack_required;
    bool ack_received;
    uint8_t retransmit_count;
    uint8_t last_redundancy_version;
    /* Delayable work for waiting frame aggregation or random access process */
    struct k_work_delayable tx_process_work;
    struct k_work random_backoff_work;
};

static struct dect_tx_process_info tx_processes[DECTNR_HARQ_PROCESSES + DECTNR_BEACON_PROCESSES];

static bool process_unicast_rx_frame(ot_dectnr_rx_frame *rx_frame);
static void process_ot_dectnr_addr_mapping(const uint8_t *data);

static void reset_tx_process(uint8_t process_nbr)
{
    LOG_DBG("reset tx_process %hhu", process_nbr);
    if (process_nbr >= DECTNR_HARQ_PROCESSES + DECTNR_BEACON_PROCESSES) {
        LOG_ERR("Invalid process number %hhu", process_nbr);
        return;
    }
    tx_processes[process_nbr].tx_in_progress = false;
    tx_processes[process_nbr].dect_data_size = 0;
    tx_processes[process_nbr].retransmit_count = 0;
    tx_processes[process_nbr].ack_required = false;
    tx_processes[process_nbr].ack_received = false;
    memset(tx_processes[process_nbr].data, 0, sizeof(tx_processes[process_nbr].data));
}

static int dect_set_radio_state(enum ot_dectnr_radio_state radio_state) {
    LOG_DBG("DECT radio state %d -> %d", ot_dectnr_ctx.radio_state, radio_state);
    ot_dectnr_ctx.radio_state = radio_state;
    return 0;
}

static void random_backoff_work_handler(struct k_work *work)
{
    uint16_t random_backoff_ms, max_backoff_ms;
    uint32_t random_value = sys_rand32_get();
    struct dect_tx_process_info *tx_process = CONTAINER_OF((struct k_work *)work, struct dect_tx_process_info, random_backoff_work);

    if (tx_process->retransmit_count > DECT_MAX_BACKOFF_COUNT) {
        LOG_WRN("Max backoff count reached");
        reset_tx_process(tx_process->process_nbr);
        return;
    }
    max_backoff_ms = 1 << (tx_process->retransmit_count + DECT_MIN_BACKOFF_EXPONENTIAL);
    random_backoff_ms = random_value % max_backoff_ms;

    random_backoff_ms = random_value % max_backoff_ms;
    k_work_reschedule(&tx_process->tx_process_work, K_MSEC(random_backoff_ms));
    tx_process->retransmit_count++;
    LOG_DBG("Retransmit %hhu time, delay %hu ms", tx_process->retransmit_count, random_backoff_ms);
}

static inline bool is_pending_event_set(enum pending_events event)
{
    return atomic_test_bit(pending_events, event);
}

static void set_pending_event(enum pending_events event)
{
    atomic_set_bit(pending_events, event);
    otSysEventSignalPending();
}

static void reset_pending_event(enum pending_events event)
{
    atomic_clear_bit(pending_events, event);
}

static const int16_t byte_per_mcs_and_length[5][16] = {
    { 0,  17,  33,  50,  67,  83,  99, 115, 133, 149, 165, 181, 197, 213, 233, 249},
    { 4,  37,  69, 103, 137, 169, 201, 233, 263, 295, 327, 359, 391, 423, 463, 495},
    { 7,  57, 107, 157, 205, 253, 295, 343, 399, 447, 495, 540, 596, 644, 692,  -1},
    {11,  77, 141, 209, 271, 335, 399, 463, 532, 596, 660,  -1,  -1,  -1,  -1,  -1},
    {18, 117, 217, 311, 407, 503, 604, 700,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1}};

static uint16_t get_peer_device_id_from_rx_frame(ot_dectnr_rx_frame *rx_frame)
{
    struct dect_phy_header_type2_format0_t *header_fmt0 = (void *)&rx_frame->pcc_info.hdr;
    uint16_t peer_device_id = (header_fmt0->transmitter_id_hi << 8) | header_fmt0->transmitter_id_lo;
    LOG_DBG("peer device id:%hu", peer_device_id);
    return peer_device_id;
}

static uint8_t get_sequence_number_from_rx_frame(ot_dectnr_rx_frame *rx_frame)
{
    return *(uint8_t *)rx_frame->data;
}
    
static void dect_mac_utils_get_packet_length(int16_t *data_size, uint32_t mcs, uint32_t *packet_length)
{
    for (*packet_length = 0; *packet_length < 16; (*packet_length)++)
    {
        if (byte_per_mcs_and_length[mcs][*packet_length] == -1) {
            packet_length--;
            break;
        }
        if (byte_per_mcs_and_length[mcs][*packet_length] >= *data_size)
        {
            break;
        }
    }
}

static int8_t harq_tx_next_redundancy_version(uint8_t current_redundancy_version)
{
    /* MAC spec ch. 5.5.1:
     * Hybrid ARQ redundancies shall be sent in the order {0, 2, 3, 1, 0, …}.
     * Initial transmission shall use redundancy version 0 for the selected HARQ process number.
     * Number of retransmissions is application dependent.
     */
    if (current_redundancy_version == 0) {
        return 2;
    } else if (current_redundancy_version == 2) {
        return 3;
    } else if (current_redundancy_version == 3) {
        return 1;
    } else if (current_redundancy_version == 1) {
        return 0;
    } else {
        LOG_ERR("Invalid redundancy version");
        return -1;
    }
}

/* DECT receive operation */
static int dect_receive(uint64_t start_time)
{
    int err;

    struct nrf_modem_dect_phy_rx_params rx_op_params = {
        .start_time = start_time,
        .handle = OT_DECTNR_RECEIVE_HANDLE,
        .network_id = (uint32_t)ot_pan_id,
        .mode = NRF_MODEM_DECT_PHY_RX_MODE_SEMICONTINUOUS,
        .rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
        .link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
        .rssi_level = CONFIG_OPENTHREAD_OVER_DECTNR_RSSI_LEVEL,
        .carrier = CONFIG_OPENTHREAD_OVER_DECTNR_PHY_FREQUENCY,
        .duration = UINT32_MAX,
        .filter.short_network_id = (uint8_t)(ot_pan_id & 0xff),
        .filter.is_short_network_id_used = 1,
        /* listen for beacon and unicast to me */
        .filter.receiver_identity = ot_dectnr_ctx.ot_addr_map.dev_id,
    };
    LOG_DBG("dect_receive. start_time: %"PRIu64"", start_time);
    err = nrf_modem_dect_phy_rx(&rx_op_params);
    if (err == 0) {
        dect_set_radio_state(OT_DECTNR_RADIO_STATE_RX);
        LOG_DBG("DECT Reception started");
    }

    return err;
}

/* DECT transmit operation. */
static int dect_transmit(struct dect_tx_process_info *tx_process)
{
    int err;
    uint32_t packet_length = 0;
    struct dect_phy_header_type1_format0_t header_type1;
    struct dect_phy_header_type2_format0_t header_type2;
    struct nrf_modem_dect_phy_tx_params tx_op_params;

    dect_mac_utils_get_packet_length(&tx_process->dect_data_size,
                                     CONFIG_OPENTHREAD_OVER_DECTNR_DEFAULT_TX_MCS,
                                     &packet_length);

    tx_op_params.bs_cqi = NRF_MODEM_DECT_PHY_BS_CQI_NOT_USED;
    tx_op_params.start_time = 0;
    tx_op_params.network_id = (uint32_t)ot_pan_id;
    tx_op_params.lbt_rssi_threshold_max = CONFIG_OPENTHREAD_OVER_DECTNR_LBT_THRESHOLD_MAX;
    tx_op_params.carrier = CONFIG_OPENTHREAD_OVER_DECTNR_PHY_FREQUENCY;
    tx_op_params.data = tx_process->data;
    tx_op_params.data_size = tx_process->dect_data_size;

    if (tx_process->process_nbr >= DECTNR_HARQ_PROCESSES) {
        /* Ack is not required. Set the type 1 header fields */
        header_type1.transmitter_id_hi = (ot_dectnr_ctx.ot_addr_map.dev_id >> 8);
        header_type1.transmitter_id_lo = (ot_dectnr_ctx.ot_addr_map.dev_id & 0xff);
        header_type1.packet_length = packet_length;
        header_type1.header_format = DECT_PHY_HEADER_FORMAT_000;
        header_type1.packet_length_type = DECT_PHY_HEADER_PKT_LENGTH_TYPE_SUBSLOTS;
        header_type1.short_network_id = (uint8_t)(ot_pan_id & 0xff);
        header_type1.df_mcs = CONFIG_OPENTHREAD_OVER_DECTNR_DEFAULT_TX_MCS;
        header_type1.transmit_power = CONFIG_OPENTHREAD_OVER_DECTNR_DEFAULT_TX_POWER;
        header_type1.reserved = 0;

        tx_op_params.phy_type = DECT_PHY_HEADER_TYPE1;
        tx_op_params.phy_header = (union nrf_modem_dect_phy_hdr *)&header_type1;
        tx_op_params.handle = OT_DECTNR_TX_PROCESS_TX_HANDLE_START + tx_process->process_nbr;
        tx_op_params.lbt_period = 0;

        err = nrf_modem_dect_phy_tx(&tx_op_params);
        if (err != 0)
        {
            LOG_ERR("nrf_modem_dect_phy_tx() returned %d", err);
            return err;
        }
    } else {
        /* Ack is required. Set the type 2 header fields with format 000 */
        header_type2.transmitter_id_hi = (ot_dectnr_ctx.ot_addr_map.dev_id >> 8);
        header_type2.transmitter_id_lo = (ot_dectnr_ctx.ot_addr_map.dev_id & 0xff);
        header_type2.receiver_identity_hi = tx_process->dect_receiver_device_id >> 8;
        header_type2.receiver_identity_lo = tx_process->dect_receiver_device_id & 0xff;
        header_type2.packet_length = packet_length;
        header_type2.packet_length_type = DECT_PHY_HEADER_PKT_LENGTH_TYPE_SUBSLOTS;
        header_type2.format = DECT_PHY_HEADER_FORMAT_000;
        header_type2.short_network_id = (uint8_t)(ot_pan_id & 0xff);
        header_type2.df_mcs = CONFIG_OPENTHREAD_OVER_DECTNR_DEFAULT_TX_MCS;
        header_type2.transmit_power = CONFIG_OPENTHREAD_OVER_DECTNR_DEFAULT_TX_POWER;
        if (tx_process->retransmit_count > 0) {
            header_type2.df_new_data_indication_toggle = 0; /* no new data */
            header_type2.df_redundancy_version =
                harq_tx_next_redundancy_version(tx_process->last_redundancy_version);
        } else {
            header_type2.df_new_data_indication_toggle = 1; /* to be toggled later */
            header_type2.df_redundancy_version = 0;  /* 1st tx */
        }
        tx_process->last_redundancy_version = header_type2.df_redundancy_version;
        header_type2.df_harq_process_number = tx_process->process_nbr;
        header_type2.spatial_streams = 2;
        header_type2.feedback.format1.format = 1;
        header_type2.feedback.format1.CQI = 1;
        header_type2.feedback.format1.harq_process_number0 = tx_process->process_nbr;
        header_type2.feedback.format1.transmission_feedback0 = 1;
        header_type2.feedback.format1.buffer_status = 0;

        tx_op_params.phy_type = DECT_PHY_HEADER_TYPE2;
        tx_op_params.phy_header = (union nrf_modem_dect_phy_hdr *)&header_type2;
        tx_op_params.handle = OT_DECTNR_TX_PROCESS_TX_HANDLE_START + tx_process->process_nbr;

        struct nrf_modem_dect_phy_rx_params rx_op_params;

        rx_op_params.start_time = 0;
        rx_op_params.handle = OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START + tx_process->process_nbr;
        rx_op_params.network_id = (uint32_t)ot_pan_id;
        rx_op_params.mode = NRF_MODEM_DECT_PHY_RX_MODE_SINGLE_SHOT;
        rx_op_params.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF;
        rx_op_params.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED;
        rx_op_params.rssi_level = CONFIG_OPENTHREAD_OVER_DECTNR_RSSI_LEVEL;
        rx_op_params.carrier = CONFIG_OPENTHREAD_OVER_DECTNR_PHY_FREQUENCY;
        rx_op_params.duration =
            (DECTNR_HARQ_FEEDBACK_RX_DELAY_SUBSLOTS + DECTNR_HARQ_FEEDBACK_RX_SUBSLOTS) *
            DECTNR_RADIO_SUBSLOT_DURATION_IN_MODEM_TICKS;
        rx_op_params.filter.short_network_id = (uint8_t)(ot_pan_id & 0xff);
        rx_op_params.filter.is_short_network_id_used = 1;
        /* listen for the HARQ */
        rx_op_params.filter.receiver_identity = (header_type2.transmitter_id_hi << 8) | (header_type2.transmitter_id_lo);
        tx_op_params.lbt_period = NRF_MODEM_DECT_LBT_PERIOD_MIN;
        tx_process->ack_required = true;
        struct nrf_modem_dect_phy_tx_rx_params tx_rx_params = {
            .tx = tx_op_params,
            .rx = rx_op_params,
        };
        LOG_INF("dect_transmit process_nbr: %hhu sequence: %u", tx_process->process_nbr, tx_process->data[0]);
        err = nrf_modem_dect_phy_tx_rx(&tx_rx_params);
        if (err != 0)
        {
            LOG_ERR("nrf_modem_dect_phy_tx_rx() returned %d", err);
            return err;
        }
    }
    LOG_HEXDUMP_DBG(tx_process->data, tx_process->dect_data_size, "");
    dect_set_radio_state(OT_DECTNR_RADIO_STATE_TX);
    set_pending_event(PENDING_EVENT_TX_DONE);

    return err;
}

/* DECT HARQ feedback operation */
static int dect_harq_feedback(const struct nrf_modem_dect_phy_pcc_event *evt,
                              const struct dect_phy_header_type2_format0_t *header)
{
    uint16_t receiver_dev_id = (header->receiver_identity_hi << 8) | header->receiver_identity_lo;

    if (receiver_dev_id == ot_dectnr_ctx.ot_addr_map.dev_id) {
        LOG_DBG("RxID 0x%02X%02X Device ID 0x%02X%02X",
            header->receiver_identity_hi, header->receiver_identity_lo,
            ot_dectnr_ctx.ot_addr_map.dev_id >> 8, ot_dectnr_ctx.ot_addr_map.dev_id & 0xff);
    } else {
        LOG_ERR("Not for me. RxID 0x%02X%02X Device ID 0x%02X%02X",
            header->receiver_identity_hi, header->receiver_identity_lo,
            ot_dectnr_ctx.ot_addr_map.dev_id >> 8, ot_dectnr_ctx.ot_addr_map.dev_id & 0xff);
        return -EINVAL;
    }
    uint16_t len_slots = header->packet_length + 1;
    /* HARQ feedback requested */
    union nrf_modem_dect_phy_hdr phy_header;

    harq_feedback_header.format = DECT_PHY_HEADER_FORMAT_001;
    harq_feedback_header.df_mcs = CONFIG_OPENTHREAD_OVER_DECTNR_DEFAULT_TX_MCS;
    harq_feedback_header.transmit_power = CONFIG_OPENTHREAD_OVER_DECTNR_DEFAULT_TX_POWER;
    harq_feedback_header.receiver_identity_hi = header->transmitter_id_hi;
    harq_feedback_header.receiver_identity_lo = header->transmitter_id_lo;
    harq_feedback_header.transmitter_id_hi = header->receiver_identity_hi;
    harq_feedback_header.transmitter_id_lo = header->receiver_identity_lo;
    harq_feedback_header.spatial_streams = header->spatial_streams;
    harq_feedback_header.feedback.format1.format = 1;
    harq_feedback_header.feedback.format1.CQI = 1;
    harq_feedback_header.feedback.format1.harq_process_number0 =
        header->df_harq_process_number;
    harq_feedback_header.short_network_id = (uint8_t)(ot_pan_id & 0xff);

    /* ACK/NACK: According to CRC: */
    harq_feedback_header.feedback.format1.transmission_feedback0 = 0;
    harq_feedback_header.feedback.format1.buffer_status = 0; //TODO: confirm buffer status is not required
    memcpy(&phy_header.type_2, &harq_feedback_header, sizeof(phy_header.type_2));
    harq_feedback_tx_params.network_id = (uint32_t)ot_pan_id;
    harq_feedback_tx_params.phy_header = &phy_header;
    harq_feedback_tx_params.start_time = evt->stf_start_time +
        (len_slots * DECTNR_RADIO_SUBSLOT_DURATION_IN_MODEM_TICKS) + 
        DECTNR_HARQ_FEEDBACK_TX_DELAY_SUBSLOTS * DECTNR_RADIO_SUBSLOT_DURATION_IN_MODEM_TICKS;
    int err = nrf_modem_dect_phy_tx_harq(&harq_feedback_tx_params);
    if (err) {
        printk("nrf_modem_dect_phy_tx_harq() failed: %d\n", err);
        return err;
    }
    dect_set_radio_state(OT_DECTNR_RADIO_STATE_TX);
    return 0;
}

/* prefill data for DECT PHY HARQ feedback operation */
static void dect_phy_prefill_harq_feedback_data(void)
{
    harq_feedback_tx_params.start_time = 0;
    harq_feedback_tx_params.handle = OT_DECTNR_HARQ_FEEDBACK_HANDLE;
    harq_feedback_tx_params.carrier = CONFIG_OPENTHREAD_OVER_DECTNR_PHY_FREQUENCY;
    harq_feedback_tx_params.phy_type = DECT_PHY_HEADER_TYPE2;
    harq_feedback_tx_params.lbt_period = 0;
    harq_feedback_tx_params.data_size = 4;
    harq_feedback_tx_params.bs_cqi = 1; /* MCS-0, no meaning in our case */

    harq_feedback_header.packet_length = 0; /* = 1 slot */
    harq_feedback_header.packet_length_type = 0; /* SLOT */
    harq_feedback_header.format = DECT_PHY_HEADER_FORMAT_001;
}

/* Callback after init operation. */
static void on_init(const struct nrf_modem_dect_phy_init_event *evt)
{
    if (evt->err) {
        LOG_ERR("DECT init operation failed, err %d", evt->err);
    }
    ot_dectnr_ctx.last_dect_op_result  = evt->err;
    k_sem_give(&modem_operation_sem);
}

/* Callback after deinit operation. */
static void on_deinit(const struct nrf_modem_dect_phy_deinit_event *evt)
{
    if (evt->err) {
        LOG_ERR("Deinit failed, err %d", evt->err);
        return;
    }
    ot_dectnr_ctx.last_dect_op_result = evt->err;
    k_sem_give(&modem_operation_sem);
}

static void on_activate(const struct nrf_modem_dect_phy_activate_event *evt)
{
    if (evt->err) {
        LOG_ERR("Activate failed, err %d", evt->err);
    }
    ot_dectnr_ctx.last_dect_op_result  = evt->err;
    k_sem_give(&modem_operation_sem);
}

static void on_deactivate(const struct nrf_modem_dect_phy_deactivate_event *evt)
{
    if (evt->err) {
        LOG_ERR("Deactivate failed, err %d", evt->err);
    }
    ot_dectnr_ctx.last_dect_op_result  = evt->err;
    k_sem_give(&modem_operation_sem);
}

static void on_configure(const struct nrf_modem_dect_phy_configure_event *evt)
{
    if (evt->err) {
        LOG_ERR("Configure failed, err %d", evt->err);
    }
    ot_dectnr_ctx.last_dect_op_result  = evt->err;
    k_sem_give(&modem_operation_sem);
}

/* Callback after link configuration operation. */
static void on_link_config(const struct nrf_modem_dect_phy_link_config_event *evt)
{
    LOG_DBG("link_config cb time %"PRIu64" status %d", ot_dectnr_ctx.last_modem_event_time, evt->err);
}

static void on_radio_config(const struct nrf_modem_dect_phy_radio_config_event *evt)
{
    LOG_DBG("radio_config cb time %"PRIu64" status %d", ot_dectnr_ctx.last_modem_event_time, evt->err);
}

/* Callback after capability get operation. */
static void on_capability_get(const struct nrf_modem_dect_phy_capability_get_event *evt)
{
    LOG_DBG("capability_get cb time %"PRIu64" status %d", ot_dectnr_ctx.last_modem_event_time, evt->err);
}

static void on_bands_get(const struct nrf_modem_dect_phy_band_get_event *evt)
{
    LOG_DBG("bands_get cb status %d", evt->err);
}

static void on_latency_info_get(const struct nrf_modem_dect_phy_latency_info_event *evt)
{
    LOG_DBG("latency_info_get cb status %d", evt->err);
}

/* Callback after time query operation. */
static void on_time_get(const struct nrf_modem_dect_phy_time_get_event *evt)
{
    LOG_DBG("time_get cb time %"PRIu64" status %d", ot_dectnr_ctx.last_modem_event_time, evt->err);
}

static void on_cancel(const struct nrf_modem_dect_phy_cancel_event *evt)
{
    LOG_DBG("on_cancel cb status %d", evt->err);
}

/* Operation complete notification. */
static void on_op_complete(const struct nrf_modem_dect_phy_op_complete_event *evt)
{
    int ret;

    if (evt->err != 0) {
        LOG_ERR("op_complete cb time %"PRIu64" handle: %d err %X", ot_dectnr_ctx.last_modem_event_time, evt->handle, evt->err);
    }
    if (evt->handle == OT_DECTNR_RECEIVE_HANDLE) {
        if (evt->err == NRF_MODEM_DECT_PHY_SUCCESS) {
            LOG_DBG("DECT RX success in op_complete");
        } else {
            LOG_ERR("DECT RX failed in op_complete, err %X", evt->err);
        }
        if (ot_dectnr_ctx.radio_state == OT_DECTNR_RADIO_STATE_RX) {
            set_pending_event(PENDING_EVENT_DECT_IDLE);
        }
    }
    if (evt->handle == OT_DECTNR_HARQ_FEEDBACK_HANDLE) {
        if (evt->err == NRF_MODEM_DECT_PHY_SUCCESS) {
            LOG_DBG("DECT HARQ Feedback TX success in op_complete");
        } else {
            LOG_ERR("DECT HARQ Feedback TX failed in op_complete, err %X", evt->err);
        }
        ret = dect_receive(ot_dectnr_ctx.last_modem_event_time + 2 * DECTNR_RADIO_SUBSLOT_DURATION_IN_MODEM_TICKS);
        if (ret != 0) {
            LOG_ERR("DECT RX failed in op_complete, err %X", evt->err);
        }
    }
    if (evt->handle >= OT_DECTNR_TX_PROCESS_TX_HANDLE_START && evt->handle < OT_DECTNR_TX_PROCESS_TX_HANDLE_START + DECTNR_HARQ_PROCESSES + DECTNR_BEACON_PROCESSES) {
        if (evt->err == 0) {
            LOG_DBG("DECT TX process %d completed", evt->handle - OT_DECTNR_TX_PROCESS_TX_HANDLE_START);
        } else {
            LOG_ERR("DECT TX process %d failed, err %X", evt->handle - OT_DECTNR_TX_PROCESS_TX_HANDLE_START, evt->err);
        }
        if (tx_processes[evt->handle - OT_DECTNR_TX_PROCESS_TX_HANDLE_START].ack_required) {
            LOG_DBG("Tx process %d wait for ack!", evt->handle - OT_DECTNR_TX_PROCESS_TX_HANDLE_START);
        } else {
            reset_tx_process(evt->handle - OT_DECTNR_TX_PROCESS_TX_HANDLE_START);
            set_pending_event(PENDING_EVENT_DECT_IDLE);
        }
    }
    if (evt->handle >= OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START && evt->handle < OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START + DECTNR_HARQ_PROCESSES) {
        if (evt->err == 0) {
            LOG_DBG("DECT TX process %d RX completed", evt->handle - OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START);
            if (!tx_processes[evt->handle - OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START].ack_received && tx_processes[evt->handle - OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START].retransmit_count < DECT_MAX_BACKOFF_COUNT) { 
                LOG_WRN("Ack not received. Tx process %d retransmit", evt->handle - OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START);
                k_work_submit(&tx_processes[evt->handle - OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START].random_backoff_work);
                set_pending_event(PENDING_EVENT_DECT_IDLE);
            } else {
                reset_tx_process(evt->handle - OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START);
                set_pending_event(PENDING_EVENT_DECT_IDLE);
            }
        } else {
            LOG_ERR("DECT TX process %d RX failed, err %X", evt->handle - OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START, evt->err);
            if (evt->err == NRF_MODEM_DECT_PHY_ERR_COMBINED_OP_FAILED) {
                LOG_WRN("Ack not received. Tx process %d retransmit", evt->handle - OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START);
                k_work_submit(&tx_processes[evt->handle - OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START].random_backoff_work);
                set_pending_event(PENDING_EVENT_DECT_IDLE);
            } else {
                reset_tx_process(evt->handle - OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START);
                set_pending_event(PENDING_EVENT_DECT_IDLE);
            }
        }
    }
}

static bool pcc_is_ack(const struct nrf_modem_dect_phy_pcc_event *evt)
{
    if (evt->header_status == NRF_MODEM_DECT_PHY_HDR_STATUS_VALID &&
        evt->phy_type == DECT_PHY_HEADER_TYPE2) {
        struct dect_phy_header_type2_format1_t *header_fmt1 = (void *)&evt->hdr;
        if ((header_fmt1->format == DECT_PHY_HEADER_FORMAT_001) &&
            (header_fmt1->feedback.format1.format == 1)) {
            return true;
        }
    }
    return false;
}

static bool pcc_is_beacon(const struct nrf_modem_dect_phy_pcc_event *evt)
{
    if (evt->header_status == NRF_MODEM_DECT_PHY_HDR_STATUS_VALID &&
        evt->phy_type == DECT_PHY_HEADER_TYPE1) {
        return true;
    }
    return false;
}

/* Physical Control Channel reception notification. */
static void on_pcc(const struct nrf_modem_dect_phy_pcc_event *evt)
{
    /* Provide HARQ feedback if requested */
    if (evt->header_status == NRF_MODEM_DECT_PHY_HDR_STATUS_VALID &&
        evt->phy_type == DECT_PHY_HEADER_TYPE2) {
        struct dect_phy_header_type2_format0_t *header_fmt0 = (void *)&evt->hdr;
        struct dect_phy_header_type2_format1_t *header_fmt1 = (void *)&evt->hdr;
        /* If FORMAT 0 PCC header received, provide HARQ feedback */
        if (header_fmt0->format == DECT_PHY_HEADER_FORMAT_000) {
            int err = dect_harq_feedback(evt, header_fmt0);
            if (err) {
                LOG_ERR("dect_harq_feedback failed: %d", err);
                return;
            }
        }
        /* If FORMAT 1 PCC header received, process ACK/NACK */
        else if (header_fmt1->format == DECT_PHY_HEADER_FORMAT_001) {
            /* Find TX process by TX ID */
            if (header_fmt1->feedback.format1.format == 1) {
                if (header_fmt1->feedback.format1.transmission_feedback0) {
                    /* Handle ACK: clear the HARQ process resources */
                    LOG_INF("ACK received for process %hhu", header_fmt1->feedback.format1.harq_process_number0);
                    tx_processes[header_fmt1->feedback.format1.harq_process_number0].ack_received = true;
                } else {
                    /* Handle NACK: retransmit */
                    LOG_INF("NACK received for process %hhu", header_fmt1->feedback.format1.harq_process_number0);
                }
            }
        }
    } else if (evt->header_status == NRF_MODEM_DECT_PHY_HDR_STATUS_VALID &&
               evt->phy_type == DECT_PHY_HEADER_TYPE1) {
        LOG_DBG("Beacon received");
    }
    memcpy(&ot_dectnr_ctx.last_pcc_event, evt, sizeof(struct nrf_modem_dect_phy_pcc_event));
}

/* Physical Control Channel CRC error notification. */
static void on_pcc_crc_err(const struct nrf_modem_dect_phy_pcc_crc_failure_event *evt)
{
    LOG_DBG("pcc_crc_err cb time %"PRIu64"", ot_dectnr_ctx.last_modem_event_time);
    ot_rx_result = OT_ERROR_FCS;
    set_pending_event(PENDING_EVENT_RX_FAILED);
}

/* Physical Data Channel reception notification. */
static void on_pdc(const struct nrf_modem_dect_phy_pdc_event *evt)
{
    if (ot_dectnr_ctx.last_pcc_event.transaction_id != evt->transaction_id) {
        LOG_ERR("Transaction ID mismatch: %u != %u", ot_dectnr_ctx.last_pcc_event.transaction_id, evt->transaction_id);
        return;
    }

    if (pcc_is_ack(&ot_dectnr_ctx.last_pcc_event)) {
        /* ACK received. No need to process the data. */
        LOG_DBG("ACK received");
        return;
    }
    if (evt->data == NULL) {
        LOG_ERR("Invalid data pointer");
        return;
    }
    if (evt->snr >= 127) {
        LOG_ERR("SNR Not known or not detectable.");
        return;
    }

    for (int i = 0; i < CONFIG_OT_DECT_RX_BUFFERS; i++) {
        if (ot_dectnr_ctx.rx_frames[i].status != RX_FRAME_STATUS_FREE) {
            /* Find next free slot */
            continue;
        }
        memcpy(&ot_dectnr_ctx.rx_frames[i].pcc_info, &ot_dectnr_ctx.last_pcc_event, sizeof(struct nrf_modem_dect_phy_pcc_event));
        memcpy(ot_dectnr_ctx.rx_frames[i].data, evt->data, evt->len);
        ot_dectnr_ctx.rx_frames[i].snr = evt->snr;
        ot_dectnr_ctx.rx_frames[i].rssi_2 = evt->rssi_2;
        ot_dectnr_ctx.rx_frames[i].time = otPlatTimeGet();
        ot_dectnr_ctx.last_rssi = evt->rssi_2 / 2;
        ot_dectnr_ctx.rx_frames[i].status = RX_FRAME_STATUS_RECEIVED;
        k_fifo_put(&ot_dectnr_ctx.rx_fifo, &ot_dectnr_ctx.rx_frames[i]);
        return;
    }
    LOG_ERR("Not enough rx frames allocated for 15.4 driver!");
}

/* Physical Data Channel CRC error notification. */
static void on_pdc_crc_err(const struct nrf_modem_dect_phy_pdc_crc_failure_event *evt)
{
    LOG_DBG("pdc_crc_err cb time %"PRIu64"", ot_dectnr_ctx.last_modem_event_time);
    ot_rx_result = OT_ERROR_FCS;
    set_pending_event(PENDING_EVENT_RX_FAILED);
}

/* RSSI measurement result notification. */
static void on_rssi(const struct nrf_modem_dect_phy_rssi_event *evt)
{
    LOG_DBG("rssi cb time %"PRIu64" carrier %d", ot_dectnr_ctx.last_modem_event_time, evt->carrier);
}

static void on_stf_cover_seq_control(const struct nrf_modem_dect_phy_stf_control_event *evt)
{
    LOG_WRN("Unexpectedly in %s\n", __func__);
}

static void dect_phy_event_handler(const struct nrf_modem_dect_phy_event *evt)
{
    ot_dectnr_ctx.last_modem_event_time = evt->time;

    switch (evt->id) {
    case NRF_MODEM_DECT_PHY_EVT_INIT:
        on_init(&evt->init);
        break;
    case NRF_MODEM_DECT_PHY_EVT_DEINIT:
        on_deinit(&evt->deinit);
        break;
    case NRF_MODEM_DECT_PHY_EVT_ACTIVATE:
        on_activate(&evt->activate);
        break;
    case NRF_MODEM_DECT_PHY_EVT_DEACTIVATE:
        on_deactivate(&evt->deactivate);
        break;
    case NRF_MODEM_DECT_PHY_EVT_CONFIGURE:
        on_configure(&evt->configure);
        break;
    case NRF_MODEM_DECT_PHY_EVT_RADIO_CONFIG:
        on_radio_config(&evt->radio_config);
        break;
    case NRF_MODEM_DECT_PHY_EVT_COMPLETED:
        on_op_complete(&evt->op_complete);
        break;
    case NRF_MODEM_DECT_PHY_EVT_CANCELED:
        on_cancel(&evt->cancel);
        break;
    case NRF_MODEM_DECT_PHY_EVT_RSSI:
        on_rssi(&evt->rssi);
        break;
    case NRF_MODEM_DECT_PHY_EVT_PCC:
        on_pcc(&evt->pcc);
        break;
    case NRF_MODEM_DECT_PHY_EVT_PCC_ERROR:
        on_pcc_crc_err(&evt->pcc_crc_err);
        break;
    case NRF_MODEM_DECT_PHY_EVT_PDC:
        on_pdc(&evt->pdc);
        break;
    case NRF_MODEM_DECT_PHY_EVT_PDC_ERROR:
        on_pdc_crc_err(&evt->pdc_crc_err);
        break;
    case NRF_MODEM_DECT_PHY_EVT_TIME:
        on_time_get(&evt->time_get);
        break;
    case NRF_MODEM_DECT_PHY_EVT_CAPABILITY:
        on_capability_get(&evt->capability_get);
        break;
    case NRF_MODEM_DECT_PHY_EVT_BANDS:
        on_bands_get(&evt->band_get);
        break;
    case NRF_MODEM_DECT_PHY_EVT_LATENCY:
        on_latency_info_get(&evt->latency_get);
        break;
    case NRF_MODEM_DECT_PHY_EVT_LINK_CONFIG:
        on_link_config(&evt->link_config);
        break;
    case NRF_MODEM_DECT_PHY_EVT_STF_CONFIG:
        on_stf_cover_seq_control(&evt->stf_cover_seq_control);
        break;
    default:
        LOG_ERR("Unknown DECT PHY event %d", evt->id);
        break;
    }
}

static void dect_rx_thread(void *arg1, void *arg2, void *arg3)
{
    struct net_pkt *pkt;
    ot_dectnr_rx_frame *rx_frame;
    uint8_t pkt_len = 0;

    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    while (1) {
        pkt = NULL;
        rx_frame = k_fifo_get(&ot_dectnr_ctx.rx_fifo, K_FOREVER);
        if (rx_frame == NULL) {
            LOG_ERR("Failed to get rx_frame from fifo");
            continue;
        }
        if (rx_frame->status != RX_FRAME_STATUS_RECEIVED) {
            LOG_ERR("Frame type is unused, skipping");
            continue;
        }

        if (pcc_is_beacon(&rx_frame->pcc_info)) {
            if (*(enum ot_dectnr_beacon_type *)rx_frame->data == OT_DECTNR_BEACON_TYPE_OT_ADDR_MAPPING) {
                LOG_DBG("Address mapping frame received");
                process_ot_dectnr_addr_mapping(rx_frame->data + OT_DECTNR_BEACON_TYPE_SIZE);
                rx_frame->status = RX_FRAME_STATUS_FREE;
                continue;
            } else if (*(enum ot_dectnr_beacon_type *)rx_frame->data == OT_DECTNR_BEACON_TYPE_OT_MAC_BROADCAST_FRAME) {
                LOG_DBG("Broadcast frame received");
            } else {
                LOG_ERR("Unknown beacon type: %u", *(enum ot_dectnr_beacon_type *)rx_frame->data);
                rx_frame->status = RX_FRAME_STATUS_FREE;
                continue;
            }
        } else {
            if (process_unicast_rx_frame(rx_frame)) {
                LOG_DBG("Unicast frame received");
                if (rx_frame->status == RX_FRAME_STATUS_PENDING) {
                    LOG_ERR("Pending frame received, skipping");
                    continue;
                }
            } else {
                LOG_ERR("Failed to process received frame");
                goto drop;
            }
        }

        pkt_len = *(uint8_t *)((uint8_t *)rx_frame->data + OT_DECTNR_UNICAST_SEQUENCE_SIZE);
        if (pkt_len > OT_RADIO_FRAME_MAX_SIZE || pkt_len == 0) {
            LOG_ERR("Invalid PSDU length: %hu", pkt_len);
            goto drop;
        }
#if defined(CONFIG_NET_BUF_DATA_SIZE)
        __ASSERT_NO_MSG(pkt_len <= CONFIG_NET_BUF_DATA_SIZE);
#endif

        /* Block the RX thread until net_pkt is available, so that we
         * don't drop already ACKed frame in case of temporary net_pkt
         * scarcity. The nRF 802154 radio driver will accumulate any
         * incoming frames until it runs out of internal buffers (and
         * thus stops acknowledging consecutive frames).
         */
        pkt = net_pkt_rx_alloc_with_buffer(ot_dectnr_ctx.iface, pkt_len,
                           AF_UNSPEC, 0, K_FOREVER);

        if (net_pkt_write(pkt, rx_frame->data + OT_DECTNR_UNICAST_SEQUENCE_SIZE + IEEE802154_PHY_HEADER_SIZE, pkt_len)) {
            goto drop;
        }

        net_pkt_set_ieee802154_lqi(pkt, rx_frame->snr);
        net_pkt_set_ieee802154_rssi_dbm(pkt, rx_frame->rssi_2 / 2);

#if defined(CONFIG_NET_PKT_TIMESTAMP)
        net_pkt_set_timestamp_ns(pkt, rx_frame->time * NSEC_PER_USEC);
#endif

        LOG_INF("Caught a packet (%u) (LQI: %u)", pkt_len, rx_frame->snr);
        LOG_HEXDUMP_DBG(rx_frame->data + OT_DECTNR_UNICAST_SEQUENCE_SIZE + IEEE802154_PHY_HEADER_SIZE, pkt_len, "");

        if (net_recv_data(ot_dectnr_ctx.iface, pkt) < 0) {
            LOG_ERR("Packet dropped by NET stack");
            goto drop;
        }

        rx_frame->status = RX_FRAME_STATUS_FREE;
        continue;

drop:
        rx_frame->status = RX_FRAME_STATUS_FREE;
        net_pkt_unref(pkt);
    }
}

static void process_ot_dectnr_addr_mapping(const uint8_t *data)
{
    if (data == NULL) {
        LOG_ERR("Invalid data pointer");
        return;
    }

    ot_dectnr_address_mapping_t *addr_mapping = (ot_dectnr_address_mapping_t *)data;
    LOG_DBG("process_ot_dectnr_addr_mapping: %hu %hu", addr_mapping->dev_id, addr_mapping->rloc);
    LOG_DBG("ext_addr: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
            addr_mapping->ext_addr.m8[0], addr_mapping->ext_addr.m8[1],
            addr_mapping->ext_addr.m8[2], addr_mapping->ext_addr.m8[3],
            addr_mapping->ext_addr.m8[4], addr_mapping->ext_addr.m8[5],
            addr_mapping->ext_addr.m8[6], addr_mapping->ext_addr.m8[7]);
    if (addr_mapping->dev_id == 0) {
        LOG_ERR("Invalid DECT device ID: %hu", addr_mapping->dev_id);
        return;
    }
    if (addr_mapping->dev_id == ot_dectnr_ctx.ot_addr_map.dev_id) {
        LOG_DBG("Received own DECT device ID: %hu", addr_mapping->dev_id);
        return;
    }
    if (addr_mapping->rloc == 0xffff) {
        LOG_ERR("Invalid DECT RLOC: %hu", addr_mapping->rloc);
        return;
    }
    /* Update peer address mapping table */
    for (int i = 0; i < CONFIG_OPENTHREAD_MAX_CHILDREN; i++) {
        if (ot_dectnr_ctx.peer_devices[i].device_id == addr_mapping->dev_id) {
            // The device is in the table, update rloc and ext addr
            ot_dectnr_ctx.peer_devices[i].rloc = addr_mapping->rloc;
            ot_dectnr_ctx.peer_devices[i].last_activity_time = otPlatTimeGet();
            memcpy(&ot_dectnr_ctx.peer_devices[i].ext_addr, &addr_mapping->ext_addr, sizeof(ot_dectnr_ctx.peer_devices[i].ext_addr));
            LOG_DBG("Updated DECT device ID: %hu RLOC: %hu", addr_mapping->dev_id, addr_mapping->rloc);
            return;
        }
    }
    // The device is not in the table, check if there is space for it
    // If the slot is empty or if the device has timed out,
    // add the new device to the table
    for (int i = 0; i < CONFIG_OPENTHREAD_MAX_CHILDREN; i++) {
        if (ot_dectnr_ctx.peer_devices[i].device_id == 0 ||
            (otPlatTimeGet() - ot_dectnr_ctx.peer_devices[i].last_activity_time >
                OT_DECTNR_PEER_DEVICE_TIMEOUT)) {
            ot_dectnr_ctx.peer_devices[i].device_id = addr_mapping->dev_id;
            memcpy(&ot_dectnr_ctx.peer_devices[i].ext_addr, &addr_mapping->ext_addr, sizeof(ot_dectnr_ctx.peer_devices[i].ext_addr));
            ot_dectnr_ctx.peer_devices[i].rloc = addr_mapping->rloc;
            ot_dectnr_ctx.peer_devices[i].last_activity_time = otPlatTimeGet();
            LOG_INF("Added receiver device ID: %u", ot_dectnr_ctx.peer_devices[i].device_id);
            return;
        }
    }
}

/* Process received unicast frame to check the frame sequence */
static bool process_unicast_rx_frame(ot_dectnr_rx_frame *rx_frame)
{
    uint16_t peer_device_id = get_peer_device_id_from_rx_frame(rx_frame);
    uint8_t sequence_number = get_sequence_number_from_rx_frame(rx_frame);

    for (int i = 0; i < CONFIG_OPENTHREAD_MAX_CHILDREN; i++) {
        if (ot_dectnr_ctx.peer_devices[i].device_id == peer_device_id) {
            // The peer and destination pair is in the table, check if the sequence number is expected
            // If yes, put the frame to the FIFO. Then update the expected sequence number and time of the device.
            // If not, keep it in the buffer for later processing
            LOG_DBG("device:%hu %u %u", peer_device_id, sequence_number, ot_dectnr_ctx.peer_devices[i].next_seq_from_peer);
            if (ot_dectnr_ctx.peer_devices[i].last_activity_time < rx_frame->time - OT_DECTNR_PENDING_RX_FRAME_TIMEOUT_MS) {
                // The sequence number is out of date, renew the device
                ot_dectnr_ctx.peer_devices[i].next_seq_from_peer = sequence_number;
            }
            if (sequence_number < ot_dectnr_ctx.peer_devices[i].next_seq_from_peer) {
                // The frame is old, drop it
                LOG_ERR("Old frame from device:%hu Expected: %u",
                    peer_device_id, ot_dectnr_ctx.peer_devices[i].next_seq_from_peer);
                return false;
            } else if (sequence_number == ot_dectnr_ctx.peer_devices[i].next_seq_from_peer) {
                // The frame is as expected
                ot_dectnr_ctx.peer_devices[i].next_seq_from_peer++;
                if (ot_dectnr_ctx.peer_devices[i].pending_frame_count > 0) {
                    k_work_reschedule(&ot_dectnr_ctx.peer_devices[i].pending_rx_frame_work, K_NO_WAIT);
                }
                return true;
            } else {
                // The frame is over the expected frame, keep it in the buffer for later processing
                ot_dectnr_ctx.peer_devices[i].pending_frame_count++;
                rx_frame->status = RX_FRAME_STATUS_PENDING;
                LOG_INF("New frame from device:%hu Expected: %u", peer_device_id,
                    ot_dectnr_ctx.peer_devices[i].next_seq_from_peer);
                if (ot_dectnr_ctx.peer_devices[i].pending_frame_count > OT_DECTNR_MAX_PENDING_FRAME_COUNT) {
                    k_work_reschedule(&ot_dectnr_ctx.peer_devices[i].pending_rx_frame_work, K_NO_WAIT);
                } else {
                    k_work_reschedule(&ot_dectnr_ctx.peer_devices[i].pending_rx_frame_work,
                            K_MSEC(OT_DECTNR_PENDING_RX_FRAME_TIMEOUT_MS));
                }
                return true;
            }
        }
    }

    // The device is not in the table, check if there is space for it
    // Check if the slot is empty or if the device has timed out
    // If yes, add the new device to the table
    for (int i = 0; i < CONFIG_OPENTHREAD_MAX_CHILDREN; i++) {
        if (ot_dectnr_ctx.peer_devices[i].device_id == 0 ||
            (rx_frame->time - ot_dectnr_ctx.peer_devices[i].last_activity_time >
                OT_DECTNR_PEER_DEVICE_TIMEOUT)) {
            ot_dectnr_ctx.peer_devices[i].device_id = peer_device_id;
            ot_dectnr_ctx.peer_devices[i].next_seq_from_peer = sequence_number + 1;  
            ot_dectnr_ctx.peer_devices[i].last_activity_time = rx_frame->time;
            LOG_ERR("Add new peer device: %u to index %d", peer_device_id, i);
            return true;
        }
    }
    // No space for new device, drop the frame
    LOG_ERR("No space for new peer device ID: %u", peer_device_id);
    return false;
}

/* Dect PHY init parameters. */
static struct nrf_modem_dect_phy_config_params dect_phy_config_params = {
    .band_group_index = ((CONFIG_OPENTHREAD_OVER_DECTNR_PHY_FREQUENCY >= 525 &&
                          CONFIG_OPENTHREAD_OVER_DECTNR_PHY_FREQUENCY <= 551)) ? 1 : 0,
    .harq_rx_process_count = DECTNR_HARQ_PROCESSES,
    .harq_rx_expiry_time_us = 5000000,
};

/* Allocate net buffer for transmit */
static void packet_buffer_init(void)
{
    net_tx_pkt = net_pkt_alloc(K_NO_WAIT);
    __ASSERT_NO_MSG(net_tx_pkt != NULL);

    net_tx_buf = net_pkt_get_reserve_tx_data(OT_RADIO_FRAME_MAX_SIZE,
                         K_NO_WAIT);
    __ASSERT_NO_MSG(net_tx_buf != NULL);

    net_pkt_append_buffer(net_tx_pkt, net_tx_buf);

    ot_transmit_frame.mPsdu = net_tx_buf->data;
}

void platformRadioInit(void)
{
    packet_buffer_init();
    k_fifo_init(&ot_dectnr_ctx.rx_fifo);

    k_thread_create(&ot_dectnr_ctx.rx_thread, ot_dectnr_ctx.rx_stack,
            CONFIG_DECT_RX_STACK_SIZE,
            dect_rx_thread, NULL, NULL, NULL,
            K_PRIO_COOP(2), 0, K_NO_WAIT);
}

static inline void handle_tx_done(otInstance *aInstance)
{
    ot_transmit_frame.mInfo.mTxInfo.mIsSecurityProcessed =
        net_pkt_ieee802154_frame_secured(net_tx_pkt);
    ot_transmit_frame.mInfo.mTxInfo.mIsHeaderUpdated = net_pkt_ieee802154_mac_hdr_rdy(net_tx_pkt);
    otPlatRadioTxDone(aInstance, &ot_transmit_frame, NULL, OT_ERROR_NONE);
    ot_state = OT_RADIO_STATE_RECEIVE;
}

static void openthread_handle_received_frame(otInstance *instance,
                         struct net_pkt *pkt)
{
    otRadioFrame recv_frame;
    memset(&recv_frame, 0, sizeof(otRadioFrame));

    recv_frame.mPsdu = net_buf_frag_last(pkt->buffer)->data;
    /* Length inc. CRC. */
    recv_frame.mLength = net_buf_frags_len(pkt->buffer);
    recv_frame.mChannel = ot_channel;
    recv_frame.mInfo.mRxInfo.mLqi = net_pkt_ieee802154_lqi(pkt);
    recv_frame.mInfo.mRxInfo.mRssi = net_pkt_ieee802154_rssi_dbm(pkt);

#if defined(CONFIG_NET_PKT_TIMESTAMP)
    recv_frame.mInfo.mRxInfo.mTimestamp = net_pkt_timestamp_ns(pkt) / NSEC_PER_USEC;
#endif
    otPlatRadioReceiveDone(instance, &recv_frame, OT_ERROR_NONE);
    net_pkt_unref(pkt);
}

#if defined(CONFIG_OPENTHREAD_NAT64_TRANSLATOR)

static otMessage *openthread_ip4_new_msg(otInstance *instance, otMessageSettings *settings)
{
    return otIp4NewMessage(instance, settings);
}

static otError openthread_nat64_send(otInstance *instance, otMessage *message)
{
    return otNat64Send(instance, message);
}

#else /* CONFIG_OPENTHREAD_NAT64_TRANSLATOR */

static otMessage *openthread_ip4_new_msg(otInstance *instance, otMessageSettings *settings)
{
    return NULL;
}

static otError openthread_nat64_send(otInstance *instance, otMessage *message)
{
    return OT_ERROR_DROP;
}

#endif /* CONFIG_OPENTHREAD_NAT64_TRANSLATOR */

static void openthread_handle_frame_to_send(otInstance *instance,
                        struct net_pkt *pkt)
{
    otError error;
    struct net_buf *buf;
    otMessage *message;
    otMessageSettings settings;
    bool is_ip6 = PKT_IS_IPv6(pkt);

    NET_DBG("Sending %s packet to ot stack", is_ip6 ? "IPv6" : "IPv4");

    settings.mPriority = OT_MESSAGE_PRIORITY_NORMAL;
    settings.mLinkSecurityEnabled = true;

    message = is_ip6 ? otIp6NewMessage(instance, &settings)
             : openthread_ip4_new_msg(instance, &settings);
    if (!message) {
        NET_ERR("Cannot allocate new message buffer");
        goto exit;
    }

    if (IS_ENABLED(CONFIG_OPENTHREAD)) {
        /* Set multicast loop so the stack can process multicast packets for
         * subscribed addresses.
         */
        otMessageSetMulticastLoopEnabled(message, true);
    }

    for (buf = pkt->buffer; buf; buf = buf->frags) {
        if (otMessageAppend(message, buf->data, buf->len) != OT_ERROR_NONE) {
            NET_ERR("Error while appending to otMessage");
            otMessageFree(message);
            goto exit;
        }
    }

    error = is_ip6 ? otIp6Send(instance, message) : openthread_nat64_send(instance, message);

    if (error != OT_ERROR_NONE) {
        NET_ERR("Error while calling %s [error: %d]",
            is_ip6 ? "otIp6Send" : "openthread_nat64_send", error);
    }

exit:
    net_pkt_unref(pkt);
}

/**
 * Notify OpenThread task about new rx message.
 */
int notify_new_rx_frame(struct net_pkt *pkt)
{
    LOG_DBG("notify_new_rx_frame");
    k_fifo_put(&rx_pkt_fifo, pkt);
    set_pending_event(PENDING_EVENT_FRAME_RECEIVED);

    return 0;
}

/**
 * Notify OpenThread task about new tx message.
 */
int notify_new_tx_frame(struct net_pkt *pkt)
{
    LOG_DBG("notify_new_tx_frame");
    k_fifo_put(&tx_pkt_fifo, pkt);
    set_pending_event(PENDING_EVENT_FRAME_TO_SEND);

    return 0;
}

void platformRadioProcess(otInstance *aInstance)
{
    bool event_pending = false;

    if (is_pending_event_set(PENDING_EVENT_FRAME_TO_SEND)) {
        struct net_pkt *evt_pkt;

        reset_pending_event(PENDING_EVENT_FRAME_TO_SEND);
        while ((evt_pkt = (struct net_pkt *) k_fifo_get(&tx_pkt_fifo, K_NO_WAIT)) != NULL) {
            if (IS_ENABLED(CONFIG_OPENTHREAD_COPROCESSOR_RCP)) {
                net_pkt_unref(evt_pkt);
            } else {
                openthread_handle_frame_to_send(aInstance, evt_pkt);
            }
        }
    }
    if (is_pending_event_set(PENDING_EVENT_FRAME_RECEIVED)) {
        struct net_pkt *rx_pkt;

        reset_pending_event(PENDING_EVENT_FRAME_RECEIVED);
        while ((rx_pkt = (struct net_pkt *) k_fifo_get(&rx_pkt_fifo, K_NO_WAIT)) != NULL) {
            openthread_handle_received_frame(aInstance, rx_pkt);
        }
    }

    if (is_pending_event_set(PENDING_EVENT_RX_FAILED)) {
        reset_pending_event(PENDING_EVENT_RX_FAILED);
        otPlatRadioReceiveDone(aInstance, NULL, ot_rx_result);
    }
    if (is_pending_event_set(PENDING_EVENT_DECT_IDLE)) {
        /* Get pending tx process from fifo and transmit */
        struct dect_tx_process_info *tx_process;
        tx_process = (struct dect_tx_process_info *) k_fifo_get(&dect_tx_fifo, K_NO_WAIT);
        if (tx_process != NULL) {
            int err;
            err = dect_transmit(tx_process);
            if (err != 0) {
                LOG_ERR("dect_transmit() returned %d", err);
            }
        } else {
            LOG_DBG("No pending tx process. Start DECT reception");
            if (dect_receive(0) != 0) {
                LOG_ERR("DECT Reception failed.");
            }
        }
        reset_pending_event(PENDING_EVENT_DECT_IDLE);
    }
    if (is_pending_event_set(PENDING_EVENT_TX_DONE)) {
        reset_pending_event(PENDING_EVENT_TX_DONE);
        handle_tx_done(aInstance);
    }

    if (event_pending) {
        otSysEventSignalPending();
    }
}

uint16_t platformRadioChannelGet(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);

    return ot_channel;
}

#define WINDOW_SIZE 128

static bool is_sequence_before(unsigned char seq1, unsigned char seq2) {
    int diff = (int)seq2 - (int)seq1;

    if (diff > 0 && diff < WINDOW_SIZE) {
        return true;
    }

    if (diff < -WINDOW_SIZE) {
        return true;
    }

    return false;
}

void pending_rx_frame_work_handler(struct k_work *work)
{
    struct ot_dectnr_peer_device *peer_device = CONTAINER_OF((struct k_work_delayable *)work, struct ot_dectnr_peer_device, pending_rx_frame_work);

    ot_dectnr_rx_frame *closest_frame = NULL;
    LOG_INF("Find pending frame from: %hu", peer_device->device_id);
    for (int i = 0; i < CONFIG_OT_DECT_RX_BUFFERS; i++) {
        if (ot_dectnr_ctx.rx_frames[i].status != RX_FRAME_STATUS_PENDING) {
            continue;
        }
        if (get_peer_device_id_from_rx_frame(&ot_dectnr_ctx.rx_frames[i]) != peer_device->device_id) {
            /* Skip frames from other devices */
            continue;
        }
        if (closest_frame == NULL) {
            closest_frame = &ot_dectnr_ctx.rx_frames[i];
        } else if (is_sequence_before(get_sequence_number_from_rx_frame(&ot_dectnr_ctx.rx_frames[i]),
                                      get_sequence_number_from_rx_frame(closest_frame))) {
            closest_frame = &ot_dectnr_ctx.rx_frames[i];
        }
    }

    if (closest_frame) {
        LOG_DBG("Found pending frame. SEQ: %u", get_sequence_number_from_rx_frame(closest_frame));
        peer_device->next_seq_from_peer = get_sequence_number_from_rx_frame(closest_frame);
        closest_frame->status = RX_FRAME_STATUS_RECEIVED;
        peer_device->pending_frame_count--;
        k_fifo_put(&ot_dectnr_ctx.rx_fifo, closest_frame);
    } else {
        LOG_DBG("No matching pending frame found");
    }
}

void tx_process_work_handler(struct k_work *work)
{
    struct dect_tx_process_info *tx_process = CONTAINER_OF((struct k_work_delayable *)work, struct dect_tx_process_info, tx_process_work);

    LOG_DBG("tx_process:%hhu dect_data_size:%hu DECT radio state:%hu",
            tx_process->process_nbr, tx_process->dect_data_size, ot_dectnr_ctx.radio_state);
    /* If DECT Radio is in receiving state, stop the receiving task */
    if (ot_dectnr_ctx.radio_state == OT_DECTNR_RADIO_STATE_RX) {
        int err;
        /* Wait for radio operation to complete. */
        err = nrf_modem_dect_phy_cancel(OT_DECTNR_RECEIVE_HANDLE);
        if (err == 0) {
            k_fifo_put(&dect_tx_fifo, tx_process);
        } else {
            LOG_ERR("Failed to stop dect phy rx");
        }
    } else if (ot_dectnr_ctx.radio_state == OT_DECTNR_RADIO_STATE_TX) {
        /* DECT Radio is in transmitting state. Reschedule the task */
        k_work_submit(&tx_process->random_backoff_work);
    } else {
        /* DECT Radio is disabled */
        LOG_ERR("DECT Radio is in disabled state.");
    }
}

static int send_ot_address_mapping_beacon(ot_dectnr_address_mapping_t ot_addr_map)
{
    /* Allocate free broadcast tx process and put to tx fifo */
    for (int i = DECTNR_HARQ_PROCESSES; i < DECTNR_HARQ_PROCESSES + DECTNR_BEACON_PROCESSES; i++) {
        if (!tx_processes[i].tx_in_progress) {
            tx_processes[i].dect_data_size = OT_DECTNR_BEACON_TYPE_SIZE + sizeof(ot_addr_map);
            tx_processes[i].data[0] = OT_DECTNR_BEACON_TYPE_OT_ADDR_MAPPING;
            memcpy(tx_processes[i].data + OT_DECTNR_BEACON_TYPE_SIZE, &ot_addr_map, sizeof(ot_addr_map));
            tx_processes[i].tx_in_progress = true;
            k_work_reschedule(&tx_processes[i].tx_process_work, K_NO_WAIT);
            return 0;
        }
    }
    LOG_ERR("No available broadcast process");
    return -ENOMEM;
}

void address_mapping_beacon_work_handler(struct k_work *work)
{
    if(send_ot_address_mapping_beacon(ot_dectnr_ctx.ot_addr_map) != 0) {
        LOG_ERR("Failed to send DECT OT address mapping beacon");
    }
    k_work_reschedule(&ot_dectnr_ctx.address_mapping_beacon_work, K_MSEC(OT_DECTNR_ADDR_MAPPING_BEACON_INTERVAL_MS));
}

static int send_mac_broadcast_frame(struct otRadioFrame *ot_transmit_frame)
{
    /* Allocate free broadcast tx process and put to tx fifo */
    for (int i = DECTNR_HARQ_PROCESSES; i < DECTNR_HARQ_PROCESSES + DECTNR_BEACON_PROCESSES; i++) {
        if (!tx_processes[i].tx_in_progress) {
            tx_processes[i].dect_data_size = OT_DECTNR_BEACON_TYPE_SIZE + IEEE802154_PHY_HEADER_SIZE + ot_transmit_frame->mLength;
            tx_processes[i].data[0] = OT_DECTNR_BEACON_TYPE_OT_MAC_BROADCAST_FRAME;
            tx_processes[i].data[1] = (uint8_t)ot_transmit_frame->mLength;
            memcpy(tx_processes[i].data + OT_DECTNR_BEACON_TYPE_SIZE + IEEE802154_PHY_HEADER_SIZE, ot_transmit_frame->mPsdu, ot_transmit_frame->mLength);
            tx_processes[i].tx_in_progress = true;
            k_work_reschedule(&tx_processes[i].tx_process_work, K_NO_WAIT);
            return 0;
        }
    }
    LOG_ERR("No available broadcast process");
    return -ENOMEM;
}

static uint16_t ot_addr_to_dect_dev_id(struct otRadioFrame *ot_transmit_frame)
{
    uint8_t offset = IEEE802154_MAC_DST_ADDR_OFFSET;
    uint16_t dev_id = 0;
    struct ieee802154_fcf *fs;

    /* Validate the PSDU length and structure */
    if (ot_transmit_frame->mLength < OT_RADIO_FRAME_MIN_SIZE || ot_transmit_frame->mLength > OT_RADIO_FRAME_MAX_SIZE)
    {
        LOG_ERR("Invalid PSDU length: %zu", ot_transmit_frame->mLength);
        return 0;
    }
    if (ot_transmit_frame->mPsdu == NULL) {
        LOG_ERR("Invalid msdu pointer");
        return 0;
    }

    fs = (struct ieee802154_fcf *)ot_transmit_frame->mPsdu;
    if (fs->fc.dst_addr_mode == IEEE802154_MAC_ADDRESS_MODE_LONG) // Extended Addressing Mode
    {
        if (offset + IEEE802154_EXT_ADDRESS_SIZE > ot_transmit_frame->mLength) {
            LOG_INF("Extended dst address parse fail");
            return 0;
        }
        for (int i = 0; i < CONFIG_OPENTHREAD_MAX_CHILDREN; i++) {
            if (memcmp((const void *)(ot_transmit_frame->mPsdu + offset), &ot_dectnr_ctx.peer_devices[i].ext_addr, sizeof(ot_dectnr_ctx.peer_devices[i].ext_addr)) == 0) {
                // The device is in the table, return device id
                dev_id = ot_dectnr_ctx.peer_devices[i].device_id;
                LOG_INF("Found device ID: %hu", dev_id);
                return dev_id;
            }
        }
    }
    else if (fs->fc.dst_addr_mode == IEEE802154_MAC_ADDRESS_MODE_SHORT) // Short Addressing Mode
    {
        if (offset + IEEE802154_SHORT_ADDRESS_SIZE > ot_transmit_frame->mLength) {
            LOG_INF("Short dst address parse fail");
            return 0;
        }
        for (int i = 0; i < CONFIG_OPENTHREAD_MAX_CHILDREN; i++) {
            if (memcmp((const void *)(ot_transmit_frame->mPsdu + offset), &ot_dectnr_ctx.peer_devices[i].rloc, sizeof(ot_dectnr_ctx.peer_devices[i].rloc)) == 0) {
                // The device is in the table, return device id
                dev_id = ot_dectnr_ctx.peer_devices[i].device_id;
                LOG_INF("Found device ID: %hu", dev_id);
                return dev_id;
            }
        }
    }
    else
    {
        LOG_INF("Unsupported dst addressing mode: %d", fs->fc.dst_addr_mode);
        return 0;
    }
    return 0;
}

static int process_mac_unicast_tx_frame(struct otRadioFrame *ot_transmit_frame, struct dect_tx_process_info *tx_process)
{
    uint8_t sequence_number;

    tx_process->dect_receiver_device_id = ot_addr_to_dect_dev_id(ot_transmit_frame);
    if (tx_process->dect_receiver_device_id == 0) {
        LOG_ERR("Fail to get RX ID from OT MAC frame");
        return -EINVAL;
    }
    LOG_INF("dect_receiver_id: %hu", tx_process->dect_receiver_device_id);
    for (int i = 0; i < CONFIG_OPENTHREAD_MAX_CHILDREN; i++) {
        if (ot_dectnr_ctx.peer_devices[i].device_id == tx_process->dect_receiver_device_id) {
            // The device is in the table, increase the sequence number
            sequence_number = ot_dectnr_ctx.peer_devices[i].next_seq_to_peer;
            ot_dectnr_ctx.peer_devices[i].next_seq_to_peer++;
            goto processed;
        }
    }
    LOG_ERR("Cannot find device ID: %hu in the table", tx_process->dect_receiver_device_id);
    return -EINVAL;
processed:
    tx_process->dect_data_size = sizeof(sequence_number) + ot_transmit_frame->mLength + IEEE802154_PHY_HEADER_SIZE;
    tx_process->data[0] = sequence_number;
    tx_process->data[1] = (uint8_t)ot_transmit_frame->mLength;
    memcpy(tx_process->data + IEEE802154_PHY_HEADER_SIZE + sizeof(sequence_number), ot_transmit_frame->mPsdu, ot_transmit_frame->mLength);
    return 0;
}

static int send_mac_unicast_frame(struct otRadioFrame *ot_transmit_frame)
{
    int ret;

    /* Allocate free unicast tx process and put to tx fifo */
    for (int i = 0; i < DECTNR_HARQ_PROCESSES; i++) {
        if (!tx_processes[i].tx_in_progress) {
            /* Parse received ID and increase sequence number */
            ret = process_mac_unicast_tx_frame(ot_transmit_frame, &tx_processes[i]);
            if (ret != 0) {
                LOG_ERR("Failed to get unicast sequence number");
                return ret;
            }
            tx_processes[i].tx_in_progress = true;
            k_work_reschedule(&tx_processes[i].tx_process_work, K_NO_WAIT);
            return 0;
        }
    }
    LOG_ERR("No available unicast tx process");
    return -ENOMEM;
}

static int process_radio_tx_frame(void)
{
    struct ieee802154_fcf *fs = (struct ieee802154_fcf *)ot_transmit_frame.mPsdu;
    LOG_DBG("Frame type: %u AR: %d dst/src addr mode: %u %u", fs->fc.frame_type, fs->fc.ar, fs->fc.dst_addr_mode, fs->fc.src_addr_mode);

    if (fs->fc.frame_type == IEEE802154_MAC_FRAME_TYPE_BEACON) {
        LOG_DBG("802.15.4 beacon frame");
        return send_mac_broadcast_frame(&ot_transmit_frame);
    } else if (fs->fc.frame_type == IEEE802154_MAC_FRAME_TYPE_ACK) {
        LOG_DBG("802.15.4 ack frame, not supported");
        return OT_ERROR_FAILED;
    }

    if ((fs->fc.dst_addr_mode == IEEE802154_MAC_ADDRESS_MODE_SHORT) &&
        (*(uint16_t *)(ot_transmit_frame.mPsdu + IEEE802154_MAC_DST_ADDR_OFFSET) == IEEE802154_MAC_BROADCAST_ADDR)) {
        LOG_DBG("Send 802.15.4 broadcast frame");
        return send_mac_broadcast_frame(&ot_transmit_frame);
    } else {
        LOG_DBG("Send 802.15.4 unicast frame");
        return send_mac_unicast_frame(&ot_transmit_frame);
    }
}

void otPlatRadioSetPanId(otInstance *aInstance, otPanId aPanId)
{
    ARG_UNUSED(aInstance);

    LOG_INF("otPlatRadioSetPanId: %x", aPanId);
    if (aPanId == 0xFFFF) {
        LOG_ERR("Invalid PAN ID: %x", aPanId);
        return;
    }
    ot_pan_id = aPanId;
}

void otPlatRadioSetExtendedAddress(otInstance *aInstance,
                                   const otExtAddress *aExtAddress)
{
    ARG_UNUSED(aInstance);
    memcpy(&ot_dectnr_ctx.ot_addr_map.ext_addr, aExtAddress, sizeof(struct otExtAddress));
}

void otPlatRadioSetShortAddress(otInstance *aInstance, uint16_t aShortAddress)
{
    ARG_UNUSED(aInstance);
}

bool otPlatRadioIsEnabled(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);

    return (ot_state != OT_RADIO_STATE_DISABLED) ? true : false;
}

otError otPlatRadioEnable(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);

    if (ot_state != OT_RADIO_STATE_DISABLED && ot_state != OT_RADIO_STATE_SLEEP) {
        return OT_ERROR_INVALID_STATE;
    }

    ot_state = OT_RADIO_STATE_SLEEP;
    return OT_ERROR_NONE;
}

otError otPlatRadioDisable(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);

    if (ot_state != OT_RADIO_STATE_DISABLED && ot_state != OT_RADIO_STATE_SLEEP) {
        return OT_ERROR_INVALID_STATE;
    }

    ot_state = OT_RADIO_STATE_DISABLED;
    return OT_ERROR_NONE;
}

otError otPlatRadioSleep(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);

    if (ot_state != OT_RADIO_STATE_SLEEP && ot_state != OT_RADIO_STATE_RECEIVE) {
        return OT_ERROR_INVALID_STATE;
    }
    /* Put the radio to sleep mode. Currently sleep mode of DECT NR+ radio is not supported */
    ot_state = OT_RADIO_STATE_SLEEP;

    return OT_ERROR_NONE;
}

otError otPlatRadioReceive(otInstance *aInstance, uint8_t aChannel)
{
    ARG_UNUSED(aInstance);

    if (ot_state == OT_RADIO_STATE_DISABLED) {
        return OT_ERROR_INVALID_STATE;
    }

    ot_channel = aChannel;

    LOG_DBG("otPlatRadioReceive: %u current state: %d", ot_channel, ot_state);
    if (ot_state == OT_RADIO_STATE_SLEEP) {
        if (dect_receive(0) != 0) {
            LOG_ERR("DECT Reception failed");
            return OT_ERROR_FAILED;
        }
        ot_state = OT_RADIO_STATE_RECEIVE;
    } else if (ot_state == OT_RADIO_STATE_TRANSMIT) {
        ot_state = OT_RADIO_STATE_RECEIVE;
    }

    return OT_ERROR_NONE;
}
#if 0
otError platformRadioTransmitCarrier(otInstance *aInstance, bool aEnable)
{
    return OT_ERROR_NOT_IMPLEMENTED;
}
#endif
otRadioState otPlatRadioGetState(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);

    return ot_state;
}

otError otPlatRadioTransmit(otInstance *aInstance, otRadioFrame *aPacket)
{
    ARG_UNUSED(aInstance);
    ARG_UNUSED(aPacket);

    __ASSERT_NO_MSG(aPacket == &ot_transmit_frame);
    __ASSERT_NO_MSG(ot_transmit_frame.mLength <= OT_RADIO_FRAME_MAX_SIZE);

    if (ot_state != OT_RADIO_STATE_RECEIVE &&
        !(ot_state == OT_RADIO_STATE_SLEEP &&
          ot_radio_caps & OT_RADIO_CAPS_SLEEP_TO_TX)) {
        LOG_ERR("otPlatRadioTransmit: invalid state %d", ot_state);
        return OT_ERROR_INVALID_STATE;
    }

    if (process_radio_tx_frame() == 0) {
        ot_state = OT_RADIO_STATE_TRANSMIT;
        return OT_ERROR_NONE;
    }
    return OT_ERROR_FAILED;
}

otRadioFrame *otPlatRadioGetTransmitBuffer(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);

    return &ot_transmit_frame;
}

int8_t otPlatRadioGetRssi(otInstance *aInstance)
{
    LOG_INF("%s %d", __func__, ot_dectnr_ctx.last_rssi);

    return ot_dectnr_ctx.last_rssi;
}

static void ot_state_changed_handler(uint32_t flags, void *context)
{
    struct otInstance *aInstance = (struct otInstance *)context;

    if (flags & OT_CHANGED_THREAD_ROLE) {
        switch (otThreadGetDeviceRole(aInstance)) {
        case OT_DEVICE_ROLE_CHILD:
        case OT_DEVICE_ROLE_ROUTER:
        case OT_DEVICE_ROLE_LEADER:
        case OT_DEVICE_ROLE_DISABLED:
        case OT_DEVICE_ROLE_DETACHED:
        default:
            LOG_DBG("Thread role changed: %d", otThreadGetDeviceRole(aInstance));
            break;
        }
        goto send_beacon;
    }

    if (flags & OT_CHANGED_IP6_ADDRESS_REMOVED) {
        LOG_DBG("Ipv6 address removed");
        goto send_beacon;
    }

    if (flags & OT_CHANGED_IP6_ADDRESS_ADDED) {
        LOG_DBG("Ipv6 address added");
        goto send_beacon;
    }
    return;
send_beacon:
    const otIp6Address *ext_addr = otThreadGetLinkLocalIp6Address(aInstance);
    const otIp6Address *rloc = otThreadGetRloc(aInstance);
    if (ext_addr != NULL) {
        LOG_DBG("Ext Address: %x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x",
            ext_addr->mFields.m8[0], ext_addr->mFields.m8[1],
            ext_addr->mFields.m8[2], ext_addr->mFields.m8[3],
            ext_addr->mFields.m8[4], ext_addr->mFields.m8[5],
            ext_addr->mFields.m8[6], ext_addr->mFields.m8[7],
            ext_addr->mFields.m8[8], ext_addr->mFields.m8[9],
            ext_addr->mFields.m8[10], ext_addr->mFields.m8[11],
            ext_addr->mFields.m8[12], ext_addr->mFields.m8[13],
            ext_addr->mFields.m8[14], ext_addr->mFields.m8[15]);
    }
    if (rloc == NULL) {
        LOG_DBG("RLOC Address: %x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x",
            rloc->mFields.m8[0], rloc->mFields.m8[1],
            rloc->mFields.m8[2], rloc->mFields.m8[3],
            rloc->mFields.m8[4], rloc->mFields.m8[5],
            rloc->mFields.m8[6], rloc->mFields.m8[7],
            rloc->mFields.m8[8], rloc->mFields.m8[9],
            rloc->mFields.m8[10], rloc->mFields.m8[11],
            rloc->mFields.m8[12], rloc->mFields.m8[13],
            rloc->mFields.m8[14], rloc->mFields.m8[15]);
    }
    ot_dectnr_ctx.ot_addr_map.rloc = rloc->mFields.m8[14] << 8 | rloc->mFields.m8[15];
    k_work_reschedule(&ot_dectnr_ctx.address_mapping_beacon_work, K_NO_WAIT);
}

otRadioCaps otPlatRadioGetCaps(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);

    LOG_INF("otPlatRadioGetCaps: %x", ot_radio_caps);
    otSetStateChangedCallback(aInstance, ot_state_changed_handler, aInstance);
    return ot_radio_caps;
}

void otPlatRadioSetRxOnWhenIdle(otInstance *aInstance, bool aRxOnWhenIdle)
{
    ARG_UNUSED(aInstance);

    LOG_INF("RxOnWhenIdle=%d", aRxOnWhenIdle ? 1 : 0);
    if (aRxOnWhenIdle) {
        ot_radio_caps |= OT_RADIO_CAPS_RX_ON_WHEN_IDLE;
    } else {
        ot_radio_caps &= ~OT_RADIO_CAPS_RX_ON_WHEN_IDLE;
    }
}

bool otPlatRadioGetPromiscuous(otInstance *aInstance)
{
    LOG_ERR("otPlatRadioSetPromiscuous is not supported");

    return false;
}

void otPlatRadioSetPromiscuous(otInstance *aInstance, bool aEnable)
{
    LOG_ERR("otPlatRadioSetPromiscuous is not supported");
}

otError otPlatRadioEnergyScan(otInstance *aInstance, uint8_t aScanChannel,
                  uint16_t aScanDuration)
{
    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioGetCcaEnergyDetectThreshold(otInstance *aInstance,
                           int8_t *aThreshold)
{
    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioSetCcaEnergyDetectThreshold(otInstance *aInstance,
                           int8_t aThreshold)
{
    return OT_ERROR_NOT_IMPLEMENTED;
}

void otPlatRadioEnableSrcMatch(otInstance *aInstance, bool aEnable)
{
    return;
}

otError otPlatRadioAddSrcMatchShortEntry(otInstance *aInstance,
                     const uint16_t aShortAddress)
{
    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioAddSrcMatchExtEntry(otInstance *aInstance,
                       const otExtAddress *aExtAddress)
{
    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioClearSrcMatchShortEntry(otInstance *aInstance,
                       const uint16_t aShortAddress)
{
    return OT_ERROR_NOT_IMPLEMENTED;
}

otError otPlatRadioClearSrcMatchExtEntry(otInstance *aInstance,
                     const otExtAddress *aExtAddress)
{
    return OT_ERROR_NOT_IMPLEMENTED;
}

void otPlatRadioClearSrcMatchShortEntries(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);
}

void otPlatRadioClearSrcMatchExtEntries(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);
}

int8_t otPlatRadioGetReceiveSensitivity(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);

    return CONFIG_OPENTHREAD_DEFAULT_RX_SENSITIVITY;
}

otError otPlatRadioGetTransmitPower(otInstance *aInstance, int8_t *aPower)
{
    ARG_UNUSED(aInstance);

    if (aPower == NULL) {
        return OT_ERROR_INVALID_ARGS;
    }

    *aPower = ot_tx_power;

    return OT_ERROR_NONE;
}

otError otPlatRadioSetTransmitPower(otInstance *aInstance, int8_t aPower)
{
    ARG_UNUSED(aInstance);

    ot_tx_power = aPower;

    return OT_ERROR_NONE;
}

uint64_t otPlatTimeGet(void)
{
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

#if defined(CONFIG_NET_PKT_TXTIME)
uint64_t otPlatRadioGetNow(otInstance *aInstance)
{
    ARG_UNUSED(aInstance);

    return otPlatTimeGet();
}
#endif

#if !defined(CONFIG_OPENTHREAD_THREAD_VERSION_1_1)
void otPlatRadioSetMacKey(otInstance *aInstance, uint8_t aKeyIdMode, uint8_t aKeyId,
              const otMacKeyMaterial *aPrevKey, const otMacKeyMaterial *aCurrKey,
              const otMacKeyMaterial *aNextKey, otRadioKeyType aKeyType)
{
    ARG_UNUSED(aInstance);
    LOG_INF("otPlatRadioSetMacKey not implemented. Use software TX security instead");
}

void otPlatRadioSetMacFrameCounter(otInstance *aInstance,
                   uint32_t aMacFrameCounter)
{
    ARG_UNUSED(aInstance);
    LOG_INF("otPlatRadioSetMacFrameCounter not implemented. Use software TX security instead");
}

void otPlatRadioSetMacFrameCounterIfLarger(otInstance *aInstance, uint32_t aMacFrameCounter)
{
    ARG_UNUSED(aInstance);
    LOG_INF("otPlatRadioSetMacFrameCounterIfLarger not implemented. Use software TX security instead");
}
#endif

static void dectnr_ot_l2_init(struct net_if *iface)
{
    struct openthread_over_dectnr_phy_ctx *ctx = net_if_get_device(iface)->data;

    ctx->iface = iface;
    /* Copy 8 bytes device id */
    hwinfo_get_device_id((void *)ctx->eui64, sizeof(ctx->eui64));
    /* Company ID f4-ce-36  */
    ctx->eui64[0] = 0xF4;
    ctx->eui64[1] = 0xCE;
    ctx->eui64[2] = 0x36;
    net_if_set_link_addr(iface, ctx->eui64, sizeof(ctx->eui64), NET_LINK_IEEE802154);

    ieee802154_init(iface);
}

static int dectnr_dev_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    int err;

    for (int i = 0; i < CONFIG_OT_DECT_RX_BUFFERS; i++) {
        ot_dectnr_ctx.rx_frames[i].status = RX_FRAME_STATUS_FREE;
    }

    /* Init resource for HARQ process */
    for (int i = 0; i < DECTNR_HARQ_PROCESSES + DECTNR_BEACON_PROCESSES; i++) {
        tx_processes[i].process_nbr = i;
        reset_tx_process(i);
        k_work_init_delayable(&tx_processes[i].tx_process_work, tx_process_work_handler);
        k_work_init(&tx_processes[i].random_backoff_work, random_backoff_work_handler);
    }
    for (int i = 0; i < CONFIG_OPENTHREAD_MAX_CHILDREN; i++) {
        k_work_init_delayable(&ot_dectnr_ctx.peer_devices[i].pending_rx_frame_work, pending_rx_frame_work_handler);
    }
    dect_phy_prefill_harq_feedback_data();
    err = nrf_modem_lib_init();
    if (err) {
        LOG_ERR("modem init failed, err %d", err);
        return -ENODEV;
    } 
    err = nrf_modem_dect_phy_event_handler_set(dect_phy_event_handler);
    if (err) {
        LOG_ERR("nrf_modem_dect_phy_event_handler_set failed, err %d", err);
        k_panic();
    }
    err = nrf_modem_dect_phy_init();
    if (err) {
        LOG_ERR("nrf_modem_dect_phy_init failed, err %d", err);
        k_panic();
    }
    k_sem_take(&modem_operation_sem, K_FOREVER);
    if (ot_dectnr_ctx.last_dect_op_result  != NRF_MODEM_DECT_PHY_SUCCESS) {
        return -EIO;
    }

    err = nrf_modem_dect_phy_configure(&dect_phy_config_params);
    if (err) {
        LOG_ERR("nrf_modem_dect_phy_configure failed, err %d", err);
        return err;
    }
    k_sem_take(&modem_operation_sem, K_FOREVER);
    if (ot_dectnr_ctx.last_dect_op_result  != NRF_MODEM_DECT_PHY_SUCCESS) {
        return -EIO;
    }

    err = nrf_modem_dect_phy_activate(NRF_MODEM_DECT_PHY_RADIO_MODE_LOW_LATENCY);
    if (err) {
        LOG_ERR("nrf_modem_dect_phy_activate failed, err %d", err);
        return err;
    }
    k_sem_take(&modem_operation_sem, K_FOREVER);
    if (ot_dectnr_ctx.last_dect_op_result  != NRF_MODEM_DECT_PHY_SUCCESS) {
        return -EIO;
    }

    dect_set_radio_state(OT_DECTNR_RADIO_STATE_DISABLED);
    hwinfo_get_device_id((void *)&ot_dectnr_ctx.ot_addr_map.dev_id, sizeof(ot_dectnr_ctx.ot_addr_map.dev_id));
    LOG_INF("Dect NR+ PHY initialized, device ID: %d", ot_dectnr_ctx.ot_addr_map.dev_id);
    k_work_init_delayable(&ot_dectnr_ctx.address_mapping_beacon_work, address_mapping_beacon_work_handler);

    k_sem_give(&modem_operation_sem);

    return 0;
}

static const struct ieee802154_radio_api dectnr_radio_api = {
    .iface_api.init = dectnr_ot_l2_init,
};

/* OPENTHREAD L2 */
#define L2_LAYER OPENTHREAD_L2
#define L2_CTX_TYPE NET_L2_GET_CTX_TYPE(OPENTHREAD_L2)

NET_DEVICE_INIT(dectnr_openthread_l2, "dectnr_openthread_l2",
                dectnr_dev_init, NULL, &ot_dectnr_ctx,
                NULL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                &dectnr_radio_api, L2_LAYER, L2_CTX_TYPE, OPENTHREAD_MTU);