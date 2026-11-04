#include <atomic>
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
#include <esp_matter.h>
#include <app_priv.h>
#include <app/reporting/reporting.h>
#include <platform/CHIPDeviceLayer.h>

#include "tuya_driver.h"

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t pc_switch_endpoint_id;
static TuyaPcSwitch pcsw;

// Matter's startup can change OnOff by itself (StartUpOnOff defaults to Off), so no
// button press until it has finished: restarting the ESP must never touch the PC.
static std::atomic<bool> s_matter_started{false};

void app_driver_matter_started()
{
    s_matter_started = true;
}

// --- POLL TASK ---
static void tuya_poll_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Tuya Poll Task Started");
    vTaskDelay(pdMS_TO_TICKS(300));   // let the UART/MCU settle after boot
    while (1) {
        pcsw.Service();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Report the given on/off value to the OnOff cluster. report() reflects state to
// subscribers WITHOUT re-triggering the write callback, so there is no loop.
static void report_onoff(bool on)
{
    if (pc_switch_endpoint_id == 0) return;
    esp_matter_attr_val_t val = esp_matter_bool(on);
    esp_matter::attribute::report(pc_switch_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
}

// MCU reported the sensed PC state -> reflect it into Matter when the attribute
// disagrees (also corrects an On that never booted the PC, or a stale NVS value).
static void ReportSensedTask(intptr_t context)
{
    bool sensed = (bool)context;
    esp_matter_attr_val_t cur = esp_matter_invalid(NULL);
    if (esp_matter::attribute::get_val(pc_switch_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &cur) == ESP_OK &&
        cur.val.b == sensed) {
        return;
    }
    report_onoff(sensed);
}

static void tuya_state_change_callback(const pcswitch_state_t *state)
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(ReportSensedTask, (intptr_t)state->power);
}

// The Tuya board's button was held: forget the Matter pairing. This restarts when done.
static void tuya_reset_callback()
{
    ESP_LOGW(TAG, "Matter factory reset");
    esp_matter::factory_reset();
}

// After a UI/controller "Off", re-assert the real sensed state so the switch
// reverts (we never actually command the PC off).
static void RevertToSensedTask(intptr_t context)
{
    report_onoff(pcsw.GetState().power);
}

// --- Matter -> device (write path) ---
esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    if (endpoint_id == pc_switch_endpoint_id && cluster_id == OnOff::Id &&
        attribute_id == OnOff::Attributes::OnOff::Id) {
        bool pc_on = pcsw.GetState().power;
        if (val->val.b) {
            if (s_matter_started && !pc_on) {
                ESP_LOGI(TAG, "Matter: PC Power ON -> pressing button");
                pcsw.PressPowerOn();
            } else {
                ESP_LOGI(TAG, "Matter: PC Power ON, no button press");
            }
        } else {
            // What a press does to a running PC is up to the OS. Either way, revert the
            // attribute to the sensed state (runs after this update commits): it only
            // shows Off once the PC really is off.
            if (s_matter_started && pc_on) {
                ESP_LOGI(TAG, "Matter: PC Power OFF -> pressing button");
                pcsw.PressPowerOff();
            } else {
                ESP_LOGI(TAG, "Matter: PC Power OFF, no button press");
            }
            chip::DeviceLayer::PlatformMgr().ScheduleWork(RevertToSensedTask, 0);
        }
    }
    return ESP_OK;
}

app_driver_handle_t app_driver_pcswitch_init()
{
    pcsw.Init(TUYA_TX_PIN, TUYA_RX_PIN);
    pcsw.SetStateCallback(tuya_state_change_callback);
    pcsw.SetResetCallback(tuya_reset_callback);
    xTaskCreate(tuya_poll_task, "tuya_poll", 4096, NULL, 5, NULL);
    return (app_driver_handle_t)1;
}
