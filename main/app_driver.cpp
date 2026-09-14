#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
#include <esp_matter.h>
#include <app_priv.h>
#include <app/reporting/reporting.h>
#include <platform/CHIPDeviceLayer.h>
#include "iot_button.h"
#include "button_gpio.h"
#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>

#include "tuya_driver.h"

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t pc_switch_endpoint_id;
static TuyaPcSwitch pcsw;

#define BUTTON_GPIO_PIN 23

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

// MCU sensed a state change -> reflect the real PC power state into Matter.
static void ReportSensedTask(intptr_t context)
{
    report_onoff((bool)context);
}

static void tuya_state_change_callback(const pcswitch_state_t *state)
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(ReportSensedTask, (intptr_t)state->power);
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
        if (val->val.b) {
            // ON -> tap the PC power button.
            ESP_LOGI(TAG, "Matter: PC Power ON -> pressing button");
            pcsw.PressPowerOn();
        } else {
            // OFF -> send nothing; revert the attribute to the real sensed state
            // (runs after this update commits).
            ESP_LOGI(TAG, "Matter: PC Power OFF ignored (revert to sensed state)");
            chip::DeviceLayer::PlatformMgr().ScheduleWork(RevertToSensedTask, 0);
        }
    }
    return ESP_OK;
}

app_driver_handle_t app_driver_pcswitch_init()
{
    pcsw.Init(TUYA_TX_PIN, TUYA_RX_PIN);
    pcsw.SetStateCallback(tuya_state_change_callback);
    xTaskCreate(tuya_poll_task, "tuya_poll", 4096, NULL, 5, NULL);
    return (app_driver_handle_t)1;
}

static void app_driver_button_toggle_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Button: Commissioning Window");
    chip::CommissioningWindowManager & commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (!commissionMgr.IsCommissioningWindowOpen()) {
        commissionMgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(300),
                                                   chip::CommissioningWindowAdvertisement::kDnssdOnly);
    }
}

app_driver_handle_t app_driver_button_init()
{
    button_config_t btn_cfg = {0};
    button_gpio_config_t btn_gpio_cfg = { .gpio_num = BUTTON_GPIO_PIN, .active_level = 0 };
    button_handle_t btn_handle = NULL;
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &btn_handle);
    if (err == ESP_OK && btn_handle) {
        iot_button_register_cb(btn_handle, BUTTON_PRESS_DOWN, NULL, app_driver_button_toggle_cb, NULL);
        return (app_driver_handle_t)btn_handle;
    }
    return NULL;
}
