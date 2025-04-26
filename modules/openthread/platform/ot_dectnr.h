#ifndef __OT_DECTNR__
#define __OT_DECTNR__

/* 802.15.4 MAC definitions */
#define IEEE802154_MAC_FRAME_TYPE_BEACON      0x0
#define IEEE802154_MAC_FRAME_TYPE_DATA        0x1
#define IEEE802154_MAC_FRAME_TYPE_ACK         0x2
#define IEEE802154_MAC_FRAME_TYPE_COMMAND     0x3
#define IEEE802154_MAC_BROADCAST_ADDR         0xFFFF
#define IEEE802154_MAC_ADDRESS_MODE_SHORT     0x2
#define IEEE802154_MAC_ADDRESS_MODE_LONG      0x3
#define IEEE802154_PHY_HEADER_SIZE            1
#define IEEE802154_SHORT_ADDRESS_SIZE         2
#define IEEE802154_EXT_ADDRESS_SIZE           8
#define IEEE802154_MAC_DST_ADDR_OFFSET        5

struct ieee802154_fcf {
    struct {
        uint16_t frame_type : 3;
        uint16_t security_enabled : 1;
        uint16_t frame_pending : 1;
        uint16_t ar : 1;
        uint16_t pan_id_comp : 1;
        uint16_t reserved : 1;
        uint16_t seq_num_suppr : 1;
        uint16_t ie_list : 1;
        uint16_t dst_addr_mode : 2;
        uint16_t frame_version : 2;
        uint16_t src_addr_mode : 2;
    } fc;
    uint8_t sequence;
};

/* DECT NR+ definitions */
#define DECTNR_RADIO_FRAME_DURATION_US         (10000)
#define DECTNR_RADIO_SLOT_DURATION_US          ((double)DECTNR_RADIO_FRAME_DURATION_US / 24)
#define DECTNR_RADIO_SLOT_DURATION_IN_MODEM_TICKS (US_TO_MODEM_TICKS(DECTNR_RADIO_SLOT_DURATION_US))
#define DECTNR_RADIO_SUBSLOT_DURATION_IN_MODEM_TICKS  ((DECTNR_RADIO_SLOT_DURATION_IN_MODEM_TICKS) / 2)
#define DECTNR_HARQ_PROCESSES                  4
#define DECTNR_BEACON_PROCESSES                2
#define DECTNR_HARQ_FEEDBACK_TX_DELAY_SUBSLOTS 2
#define DECTNR_HARQ_FEEDBACK_RX_DELAY_SUBSLOTS 2
#define DECTNR_HARQ_FEEDBACK_RX_SUBSLOTS       3
#define US_TO_MODEM_TICKS(x) ((uint64_t)(((x) * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ) / 1000))
#define SECONDS_TO_MODEM_TICKS(s) (((uint64_t)(s)) * 1000 * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ)

enum dect_phy_header_type {
    DECT_PHY_HEADER_TYPE1 = 0,
    DECT_PHY_HEADER_TYPE2,
};

enum dect_phy_header_format {
    DECT_PHY_HEADER_FORMAT_000 = 0,
    DECT_PHY_HEADER_FORMAT_001 = 1,
};

enum dect_phy_packet_length_type {
    DECT_PHY_HEADER_PKT_LENGTH_TYPE_SUBSLOTS = 0,
    DECT_PHY_HEADER_PKT_LENGTH_TYPE_SLOTS = 1,
};

struct dect_phy_header_type1_format0_t {
    uint32_t packet_length : 4;
    uint32_t packet_length_type : 1;
    uint32_t header_format : 3;
    uint32_t short_network_id : 8;
    uint32_t transmitter_id_hi : 8;
    uint32_t transmitter_id_lo : 8;
    uint32_t df_mcs : 3;
    uint32_t reserved : 1;
    uint32_t transmit_power : 4;
    uint32_t pad : 24;
};

typedef struct {
    uint8_t transmission_feedback0: 1;
    uint8_t harq_process_number0: 3;
    uint8_t format: 4;

    uint8_t CQI: 4;
    uint8_t buffer_status: 4;
} dect_phy_feedback_format_1_t;

typedef struct {
    uint8_t transmission_feedback0: 1;
    uint8_t harq_process_number0: 3;
    uint8_t format: 4;

    uint8_t CQI: 4;
    uint8_t transmission_feedback1: 1;
    uint8_t harq_process_number1: 3;
} dect_phy_feedback_format_3_t;

typedef struct {
    uint8_t harq_feedback_bitmap_proc3: 1;
    uint8_t harq_feedback_bitmap_proc2: 1;
    uint8_t harq_feedback_bitmap_proc1: 1;
    uint8_t harq_feedback_bitmap_proc0: 1;
    uint8_t format: 4;

    uint8_t CQI: 4;
    uint8_t harq_feedback_bitmap_proc7: 1;
    uint8_t harq_feedback_bitmap_proc6: 1;
    uint8_t harq_feedback_bitmap_proc5: 1;
    uint8_t harq_feedback_bitmap_proc4: 1;
} dect_phy_feedback_format_4_t;

typedef struct {
    uint8_t transmission_feedback: 1;
    uint8_t harq_process_number: 3;
    uint8_t format: 4;

    uint8_t codebook_index: 6;
    uint8_t mimo_feedback: 2;
} dect_phy_feedback_format_5_t;

typedef struct {
    uint8_t reserved: 1;
    uint8_t harq_process_number: 3;
    uint8_t format: 4;
    uint8_t CQI: 4;
    uint8_t buffer_status: 4;
} dect_phy_feedback_format_6_t;

typedef union {
    dect_phy_feedback_format_1_t format1;
    dect_phy_feedback_format_3_t format3;
    dect_phy_feedback_format_4_t format4;
    dect_phy_feedback_format_5_t format5;
    dect_phy_feedback_format_6_t format6;
} dect_phy_feedback_t;

struct dect_phy_header_type2_format0_t {
    uint8_t packet_length: 4;
    uint8_t packet_length_type: 1;
    uint8_t format: 3;

    uint8_t short_network_id;

    uint8_t transmitter_id_hi;
    uint8_t transmitter_id_lo;

    uint8_t df_mcs: 4;
    uint8_t transmit_power: 4;

    uint8_t receiver_identity_hi;
    uint8_t receiver_identity_lo;

    uint8_t df_harq_process_number: 3;
    uint8_t df_new_data_indication_toggle: 1;
    uint8_t df_redundancy_version: 2;
    uint8_t spatial_streams: 2;

    dect_phy_feedback_t feedback;
};

struct dect_phy_header_type2_format1_t {
    uint8_t packet_length: 4;
    uint8_t packet_length_type: 1;
    uint8_t format: 3;

    uint8_t short_network_id;

    uint8_t transmitter_id_hi;
    uint8_t transmitter_id_lo;

    uint8_t df_mcs: 4;
    uint8_t transmit_power: 4;

    uint8_t receiver_identity_hi;
    uint8_t receiver_identity_lo;

    uint8_t reserved: 6;
    uint8_t spatial_streams: 2;

    dect_phy_feedback_t feedback;
};

/* OT DECT NR+ definitions */
/* 500ms timeout for pending rx frame */
#define OT_DECTNR_PENDING_RX_FRAME_TIMEOUT_MS 500
/* 10 minutes timeout for peer device */
#define OT_DECTNR_PEER_DEVICE_TIMEOUT 600000000

//#define DECT_MAX_TBS      5600
//#define DECT_DATA_MAX_LEN (DECT_MAX_TBS / 8)
#define DECT_MAX_TBS      1992
#define DECT_DATA_MAX_LEN (DECT_MAX_TBS / 8)
#define DECT_MIN_BACKOFF_EXPONENTIAL 3
#define DECT_MAX_BACKOFF_COUNT 5

/* DECT NR+ operation handle for receive */
#define OT_DECTNR_RECEIVE_HANDLE                           0
/* DECT NR+ operation handle for transmitting HARQ feedback */
#define OT_DECTNR_HARQ_FEEDBACK_HANDLE                     1
/* DECT NR+ operation handle for transmitting */
#define OT_DECTNR_TX_PROCESS_TX_HANDLE_START              10
/* DECT NR+ operation handle for processing HARQ feedback */
#define OT_DECTNR_TX_PROCESS_HARQ_PROCESS_HANDLE_START    20
/* Interval of OT address to DECT device ID beacon */
#define OT_DECTNR_ADDR_MAPPING_BEACON_INTERVAL_MS       3000
/* Size of beacon type field in */
#define OT_DECTNR_BEACON_TYPE_SIZE         1
/* Size of unicast sequence number */
#define OT_DECTNR_UNICAST_SEQUENCE_SIZE    1
/* Number of maximum pending RX frame of a peer device */
#define OT_DECTNR_MAX_PENDING_FRAME_COUNT  5

enum ot_dectnr_rx_frame_status {
    RX_FRAME_STATUS_FREE,
    RX_FRAME_STATUS_RECEIVED,
    RX_FRAME_STATUS_PENDING
};

enum ot_dectnr_radio_state {
    OT_DECTNR_RADIO_STATE_DISABLED,
    OT_DECTNR_RADIO_STATE_RX,
    OT_DECTNR_RADIO_STATE_TX
};

enum ot_dectnr_beacon_type {
    OT_DECTNR_BEACON_TYPE_OT_ADDR_MAPPING = 0,
    OT_DECTNR_BEACON_TYPE_OT_MAC_BROADCAST_FRAME,
};

/* Address mapping from OT IPv6 address to DECT device id */
typedef struct ot_dectnr_address_mapping {
    uint16_t dev_id;
    uint16_t rloc;
    otExtAddress ext_addr;
} ot_dectnr_address_mapping_t;

struct ot_dectnr_peer_device {
    uint16_t device_id; /* Unique identifier for the peer device */
    uint16_t rloc; /* RLOC of peer device */
    otExtAddress ext_addr; /* Extended address of the receiver device */
    uint64_t last_activity_time; /* Last time a frame was sent or received from the peer */
    uint8_t next_seq_from_peer; /* Next expected sequence number from the peer */
    uint8_t next_seq_to_peer; /* Next sequence number to the peer */
    uint16_t pending_frame_count; /* Number of pending frames received from the peer */
    struct k_work_delayable pending_rx_frame_work; /* Work to process pending frame */
};

#endif /* __OT_DECTNR__ */
