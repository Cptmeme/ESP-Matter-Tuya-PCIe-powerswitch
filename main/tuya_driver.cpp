#include "tuya_driver.h"
#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "TUYA_PCSW";

#define BAUD_RATE 9600

TuyaPcSwitch::TuyaPcSwitch() {
    m_callback = nullptr;
    m_uart_num = UART_NUM_1;

    m_state.power = false;

    rx_count = 0;
    memset(rx_buffer, 0, RX_BUF_SIZE);

    m_init_state = TY_HEARTBEAT;
    m_last_action_time = 0;
    m_attempts = 0;
    m_hb_time = 0;
    m_wifi_time = 0;
    m_query_time = 0;
}

esp_err_t TuyaPcSwitch::Init(int tx_pin, int rx_pin) {
    uart_config_t uart_config = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install((uart_port_t)m_uart_num, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;

    err = uart_param_config((uart_port_t)m_uart_num, &uart_config);
    if (err != ESP_OK) return err;

    // tx_pin = ESP TX -> heater MCU RX ;  rx_pin = ESP RX <- heater MCU TX
    err = uart_set_pin((uart_port_t)m_uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    return err;
}

// --- Frame TX helpers ----------------------------------------------------

void TuyaPcSwitch::SendFrame(uint8_t cmd, const uint8_t *payload, int payload_len) {
    uint8_t frame[64];
    int idx = 0;

    frame[idx++] = TUYA_HEADER_0;
    frame[idx++] = TUYA_HEADER_1;
    frame[idx++] = 0x00; // version (module-originated)
    frame[idx++] = cmd;
    frame[idx++] = (payload_len >> 8) & 0xFF;
    frame[idx++] = payload_len & 0xFF;
    if (payload_len > 0 && payload) {
        memcpy(&frame[idx], payload, payload_len);
        idx += payload_len;
    }

    uint8_t cs = 0;
    for (int i = 0; i < idx; i++) cs += frame[i];
    frame[idx++] = cs;

    uart_write_bytes((uart_port_t)m_uart_num, (const char*)frame, idx);
}

void TuyaPcSwitch::SendCommand(uint8_t dp_id, uint8_t type, const uint8_t *value, int len) {
    // DP control payload: DP_ID(1) Type(1) Len(2) Value(len)
    uint8_t payload[16];
    int idx = 0;
    payload[idx++] = dp_id;
    payload[idx++] = type;
    payload[idx++] = (len >> 8) & 0xFF;
    payload[idx++] = len & 0xFF;
    memcpy(&payload[idx], value, len);
    idx += len;
    SendFrame(TUYA_CMD_SET_DP, payload, idx);
}

void TuyaPcSwitch::Heartbeat()        { SendFrame(TUYA_CMD_HEARTBEAT, nullptr, 0); }
void TuyaPcSwitch::QueryStatus()      { SendFrame(TUYA_CMD_QUERY_DP, nullptr, 0); }
void TuyaPcSwitch::SendProductQuery() { SendFrame(TUYA_CMD_PRODUCT, nullptr, 0); }
void TuyaPcSwitch::SendConfQuery()    { SendFrame(TUYA_CMD_WORK_MODE, nullptr, 0); }
void TuyaPcSwitch::SendWifiStatus(uint8_t status) {
    uint8_t s = status;
    SendFrame(TUYA_CMD_WIFI_STATE, &s, 1);
}

// --- Handshake / keep-alive state machine --------------------------------

void TuyaPcSwitch::AdvanceState(TuyaInitState next) {
    m_init_state = next;
    m_attempts = 0;
    m_last_action_time = 0;   // act immediately on the next tick
}

void TuyaPcSwitch::RunStateMachine() {
    int64_t now = esp_timer_get_time();

    switch (m_init_state) {
    case TY_HEARTBEAT:
        // Ping until the MCU answers. No force-advance: nothing works until it does.
        if (now - m_last_action_time > 1000000) { Heartbeat(); m_last_action_time = now; }
        break;

    case TY_PRODUCT:
        if (now - m_last_action_time > 1000000) {
            if (m_attempts++ < 5) { SendProductQuery(); m_last_action_time = now; }
            else { ESP_LOGW(TAG, "No product-info reply; proceeding"); AdvanceState(TY_CONF); }
        }
        break;

    case TY_CONF:
        if (now - m_last_action_time > 1000000) {
            if (m_attempts++ < 5) { SendConfQuery(); m_last_action_time = now; }
            else { ESP_LOGW(TAG, "No work-mode reply; proceeding"); AdvanceState(TY_WIFI_STATUS); }
        }
        break;

    case TY_WIFI_STATUS:
        SendWifiStatus(TUYA_WIFI_ONLINE);
        m_init_state = TY_QUERY;      // next tick queries DPs (~50 ms spacing)
        m_last_action_time = now;
        break;

    case TY_QUERY:
        QueryStatus();
        m_init_state = TY_RUNNING;
        m_hb_time = m_wifi_time = m_query_time = now;
        ESP_LOGI(TAG, "Tuya handshake complete -> RUNNING");
        break;

    case TY_RUNNING:
        if (now - m_hb_time    > 15000000) { Heartbeat();                       m_hb_time = now; }
        if (now - m_wifi_time  > 30000000) { SendWifiStatus(TUYA_WIFI_ONLINE);  m_wifi_time = now; }
        if (now - m_query_time > 60000000) { QueryStatus();                     m_query_time = now; }
        break;
    }
}

void TuyaPcSwitch::Service() {
    Poll();
    RunStateMachine();
}

// --- Frame RX ------------------------------------------------------------

void TuyaPcSwitch::Poll() {
    int remaining_space = RX_BUF_SIZE - rx_count;
    if (remaining_space <= 0) {
        rx_count = 0;
        remaining_space = RX_BUF_SIZE;
    }

    int len = uart_read_bytes((uart_port_t)m_uart_num, &rx_buffer[rx_count], remaining_space, pdMS_TO_TICKS(50));

    if (len > 0) {
        rx_count += len;

        while (rx_count >= 7) {

            if (rx_buffer[0] != TUYA_HEADER_0 || rx_buffer[1] != TUYA_HEADER_1) {
                memmove(rx_buffer, &rx_buffer[1], rx_count - 1);
                rx_count--;
                continue;
            }

            uint16_t payload_len = (rx_buffer[4] << 8) | rx_buffer[5];
            int total_len = 6 + payload_len + 1;

            if (total_len > RX_BUF_SIZE) {       // garbage length, drop the header byte
                memmove(rx_buffer, &rx_buffer[1], rx_count - 1);
                rx_count--;
                continue;
            }
            if (rx_count < total_len) {
                break; // wait for the rest of the packet
            }

            uint8_t calc_cs = 0;
            for (int i = 0; i < total_len - 1; i++) calc_cs += rx_buffer[i];

            if (calc_cs == rx_buffer[total_len - 1]) {
                ProcessPacket(rx_buffer, total_len);
            }

            int remaining = rx_count - total_len;
            if (remaining > 0) {
                memmove(rx_buffer, &rx_buffer[total_len], remaining);
            }
            rx_count = remaining;
        }
    }
}

void TuyaPcSwitch::ProcessPacket(const uint8_t *packet, int len) {
    uint8_t cmd = packet[3];

    switch (cmd) {
    case TUYA_CMD_HEARTBEAT:
        if (m_init_state == TY_HEARTBEAT) {
            ESP_LOGI(TAG, "MCU alive");
            AdvanceState(TY_PRODUCT);
        }
        break;

    case TUYA_CMD_PRODUCT:
        if (len > 7) ESP_LOGI(TAG, "Product info: %.*s", len - 7, (const char *)&packet[6]);
        if (m_init_state == TY_PRODUCT) AdvanceState(TY_CONF);
        break;

    case TUYA_CMD_WORK_MODE: {
        uint16_t plen = (packet[4] << 8) | packet[5];
        ESP_LOGI(TAG, "Work-mode reply: %u byte(s) %s", plen,
                 plen == 0 ? "(module reports status over serial)" : "(MCU expects status on a GPIO)");
        if (m_init_state == TY_CONF) AdvanceState(TY_WIFI_STATUS);
        break;
    }

    case TUYA_CMD_REPORT_DP:
        ParseDatapoints(packet, len);
        break;

    case TUYA_CMD_GET_TIME:
        // No reliable wall-clock before commissioning; ignore. Device still operates.
        break;

    default:
        break;
    }
}

void TuyaPcSwitch::ParseDatapoints(const uint8_t *packet, int len) {
    int pos = 6;
    int end = len - 1;
    bool changed = false;

    while (pos < end) {
        if (pos + 4 > end) break;

        uint8_t dp_id = packet[pos];
        uint16_t data_len = (packet[pos+2] << 8) | packet[pos+3];
        int val_idx = pos + 4;

        if (val_idx + data_len > end) break;

        int32_t val = 0;
        if (data_len == 1) val = packet[val_idx];
        else if (data_len == 4) {
            val = (packet[val_idx] << 24) | (packet[val_idx+1] << 16) |
                  (packet[val_idx+2] << 8) | packet[val_idx+3];
        }

        if (dp_id == DP_POWER) {
            bool new_power = (val == 1);
            if (m_state.power != new_power) { m_state.power = new_power; changed = true; }
        }

        pos += 4 + data_len;
    }

    if (changed) NotifyStateChange();
}

void TuyaPcSwitch::NotifyStateChange() {
    if (m_callback) m_callback(&m_state);
}

// --- Control -------------------------------------------------------------

void TuyaPcSwitch::PressPowerOn() {
    uint8_t val = 1;
    SendCommand(DP_POWER, 0x01, &val, 1);
    // Do NOT optimistically set m_state.power; let the MCU report the sensed
    // state so our reported value always reflects reality.
}

void TuyaPcSwitch::SetStateCallback(tuya_state_change_cb_t cb) {
    m_callback = cb;
}
