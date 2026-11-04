#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// --- TUYA CONSTANTS ---
#define TUYA_HEADER_0 0x55
#define TUYA_HEADER_1 0xAA

// Command words (4th byte of a frame)
#define TUYA_CMD_HEARTBEAT   0x00
#define TUYA_CMD_PRODUCT     0x01
#define TUYA_CMD_WORK_MODE   0x02
#define TUYA_CMD_WIFI_STATE  0x03
#define TUYA_CMD_WIFI_RESET  0x04   // MCU -> module (repeats while the board's button is held)
#define TUYA_CMD_SET_DP      0x06   // module -> MCU (control)
#define TUYA_CMD_REPORT_DP   0x07   // MCU -> module (status report)
#define TUYA_CMD_QUERY_DP    0x08   // module -> MCU (query all)
#define TUYA_CMD_GET_TIME    0x1C   // MCU -> module (get local time; we answer "not obtained")

// Network status byte for TUYA_CMD_WIFI_STATE (we're always "online" via Thread)
#define TUYA_WIFI_ONLINE     0x04

// Datapoints (Tuya PC power switch; protocol v3)
//  DP1 (bool) is BIDIRECTIONAL:
//    WRITE 0/1 -> board taps the PC power button (momentary)
//    READ    -> the MCU's *sensed* actual PC power state
#define DP_POWER      1

// Fixed buffer size to avoid heap allocation (thread safe)
#define RX_BUF_SIZE 512

// Outgoing frames are sent one at a time, each waiting up to this long for the MCU's
// reply, like ESPHome's tuya component. This MCU can miss back-to-back frames.
#define TUYA_REPLY_TIMEOUT_US 300000
#define TUYA_TX_QUEUE_LEN     8
#define TUYA_MAX_PAYLOAD      16

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t payload[TUYA_MAX_PAYLOAD];
} tuya_frame_t;

// Handshake / run state machine
enum TuyaInitState {
    TY_HEARTBEAT = 0,  // ping until the MCU answers
    TY_PRODUCT,        // query product info (0x01)
    TY_CONF,           // query working mode (0x02)
    TY_WIFI_STATUS,    // report "online" (0x03)
    TY_QUERY,          // query all datapoints (0x08)
    TY_RUNNING         // normal operation
};

typedef struct {
    bool power;   // DP1 (sensed PC power state)
} pcswitch_state_t;

// Called on every DP1 report from the MCU (changed or not).
typedef void (*tuya_state_change_cb_t)(const pcswitch_state_t *state);

// Called once when the board's button is held.
typedef void (*tuya_reset_cb_t)();

class TuyaPcSwitch {
public:
    TuyaPcSwitch();

    esp_err_t Init(int tx_pin, int rx_pin);

    // Call periodically (~every 50 ms): reads the UART, drives the handshake,
    // and runs the heartbeat / keep-alive timers.
    void Service();

    void Poll();
    void Heartbeat();    // 0x00 keep-alive
    void QueryStatus();  // 0x08 request a full datapoint dump

    // Tap the PC's power button (writes DP1). What a tap does to a running PC is up
    // to the OS, which can be set to shut down, sleep or ignore it.
    void PressPowerOn();
    void PressPowerOff();

    void SetStateCallback(tuya_state_change_cb_t cb);
    void SetResetCallback(tuya_reset_cb_t cb);

    pcswitch_state_t GetState() const { return m_state; }
    bool IsReady() const { return m_init_state == TY_RUNNING; }

private:
    pcswitch_state_t m_state;
    tuya_state_change_cb_t m_callback;
    tuya_reset_cb_t m_reset_callback;
    bool m_reset_requested;

    int m_uart_num;

    uint8_t rx_buffer[RX_BUF_SIZE];
    int rx_count;

    // Outgoing frames. Only the poll task writes to the UART, so the Matter thread
    // can queue a power press at any time.
    QueueHandle_t m_tx_queue;
    int m_expected_reply;   // command the MCU should answer with, -1 if none
    int64_t m_tx_time;

    // Handshake state machine
    TuyaInitState m_init_state;
    int64_t m_last_action_time;
    int m_attempts;
    int64_t m_hb_time;
    int64_t m_wifi_time;
    int64_t m_query_time;

    void RunStateMachine();
    void AdvanceState(TuyaInitState next);

    void SendProductQuery();
    void SendConfQuery();
    void SendWifiStatus(uint8_t status);

    void ProcessPacket(const uint8_t *data, int len);
    void ParseDatapoints(const uint8_t *data, int len);
    void SendCommand(uint8_t dp_id, uint8_t type, const uint8_t *value, int len);
    void Enqueue(uint8_t cmd, const uint8_t *payload, int len);
    void ProcessTxQueue();
    void SendFrame(uint8_t cmd, const uint8_t *payload, int payload_len);
    void NotifyStateChange();
};
