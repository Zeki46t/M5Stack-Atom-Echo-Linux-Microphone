/*
 * Atom Echo -> microphone for Linux
 *
 *  1. USB serial: framed 16 kHz / 16-bit / mono PCM on UART0 (USB-C port).
 *     Frame = A5 5A | seq (1) | 512 bytes PCM s16le | xor (1)
 *     The Linux bridge (host/atom_echo_bridge.py) turns it into a PipeWire mic.
 *  2. Bluetooth: Classic HFP "hands-free" headset. Pair it from Linux and it
 *     shows up as a Bluetooth headset microphone. Both modes run at once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/i2s_pdm.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"

/* ---------------- configuration ---------------- */
#define MIC_SAMPLE_RATE 16000
#define MIC_CLK_PIN     GPIO_NUM_33   /* SPM1423 PDM mic on the Atom Echo */
#define MIC_DATA_PIN    GPIO_NUM_23
#define MIC_GAIN        8.0f          /* USB serial mic gain */
#define BT_GAIN_EXTRA   3.0f          /* extra gain for Bluetooth only, on top of MIC_GAIN */

#define FRAME_SAMPLES   256           /* 16 ms per frame */
#define PAYLOAD_BYTES   (FRAME_SAMPLES * 2)
#define FRAME_BYTES     (2 + 1 + PAYLOAD_BYTES + 1)

#define SERIAL_PORT     UART_NUM_0
#define SERIAL_BAUD     500000

#define BT_NAME         "Atom Echo Mic"
#define BT_RING_BYTES   2048

static const char *TAG = "atom_mic";

static i2s_chan_handle_t s_rx;
static RingbufHandle_t   s_rb;
static volatile uint32_t s_rb_fill;   /* bytes currently queued for Bluetooth */
static volatile int      s_sco_mode;  /* 0 = no audio link, 1 = CVSD 8 kHz, 2 = mSBC 16 kHz */
static volatile bool     s_slc_up;    /* HFP service level connection established */

/* ---------------- microphone ---------------- */
static void mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = FRAME_SAMPLES / 2;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx));

    /* Stereo read: the mic outputs on one clock edge, and which one depends on
     * its SEL pin, so the capture task picks the active slot automatically. */
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .clk = MIC_CLK_PIN,
            .din = MIC_DATA_PIN,
            .invert_flags = { .clk_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

static void serial_init(void)
{
    uart_config_t cfg = {
        .baud_rate = SERIAL_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(SERIAL_PORT, 256, 4096, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(SERIAL_PORT, &cfg));
}

/* ---------------- Bluetooth audio feed ---------------- */
static void bt_ring_drain(void)
{
    size_t n;
    void *d;
    while ((d = xRingbufferReceiveUpTo(s_rb, &n, 0, BT_RING_BYTES)) != NULL) {
        vRingbufferReturnItem(s_rb, d);
    }
    s_rb_fill = 0;
}

static void bt_push(const int16_t *mono16k)
{
    int16_t loud[FRAME_SAMPLES];
    int16_t half[FRAME_SAMPLES / 2];
    const void *p = loud;
    size_t len = PAYLOAD_BYTES;

    /* extra Bluetooth-only gain, clamped so it cannot wrap around */
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        float v = (float)mono16k[i] * BT_GAIN_EXTRA;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        loud[i] = (int16_t)v;
    }

    if (s_sco_mode == 1) { /* narrowband link: 16 kHz -> 8 kHz (average pairs) */
        for (int i = 0; i < FRAME_SAMPLES / 2; i++) {
            half[i] = (int16_t)(((int32_t)loud[2 * i] + loud[2 * i + 1]) / 2);
        }
        p = half;
        len = sizeof(half);
    }
    if (xRingbufferSend(s_rb, p, len, 0) == pdTRUE) {
        __atomic_fetch_add(&s_rb_fill, (uint32_t)len, __ATOMIC_RELAXED);
        esp_hf_client_outgoing_data_ready();
    }
}

/* Called by the Bluetooth stack when it wants microphone samples for the SCO link. */
static uint32_t hf_outgoing_cb(uint8_t *buf, uint32_t sz)
{
    if (s_rb_fill < sz) {
        return 0;
    }
    uint32_t got = 0;
    while (got < sz) {
        size_t n = 0;
        uint8_t *d = xRingbufferReceiveUpTo(s_rb, &n, 0, sz - got);
        if (!d) {
            break;
        }
        memcpy(buf + got, d, n);
        vRingbufferReturnItem(s_rb, d);
        got += n;
    }
    __atomic_fetch_sub(&s_rb_fill, got, __ATOMIC_RELAXED);
    return got;
}

static void hf_incoming_cb(const uint8_t *buf, uint32_t len)
{
    (void)buf; (void)len; /* we are a mic only: ignore audio from the PC */
}

/* ---------------- capture task ---------------- */
static void capture_task(void *arg)
{
    int16_t raw[FRAME_SAMPLES * 2];
    int16_t mono[FRAME_SAMPLES];
    uint8_t frame[FRAME_BYTES];
    uint8_t seq = 0;
    int32_t score[2] = {0, 0};
    float prev_in = 0, prev_out = 0;

    frame[0] = 0xA5;
    frame[1] = 0x5A;

    while (1) {
        size_t total = 0;
        while (total < sizeof(raw)) {
            size_t got = 0;
            esp_err_t e = i2s_channel_read(s_rx, (uint8_t *)raw + total, sizeof(raw) - total, &got, 1000);
            if (e != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            total += got;
        }

        /* pick the slot that carries signal (more sample-to-sample movement) */
        int32_t act[2] = {0, 0};
        for (int i = 1; i < FRAME_SAMPLES; i++) {
            for (int c = 0; c < 2; c++) {
                act[c] += abs((int)raw[2 * i + c] - (int)raw[2 * (i - 1) + c]);
            }
        }
        for (int c = 0; c < 2; c++) {
            score[c] = (score[c] * 9 + act[c]) / 10;
        }
        int ch = score[1] > score[0] ? 1 : 0;

        /* DC blocker + gain */
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            float x = (float)raw[2 * i + ch];
            float y = x - prev_in + 0.995f * prev_out;
            prev_in = x;
            prev_out = y;
            float v = y * MIC_GAIN;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            mono[i] = (int16_t)v;
        }

        /* USB serial frame */
        frame[2] = seq;
        memcpy(&frame[3], mono, PAYLOAD_BYTES);
        uint8_t x = seq;
        for (int i = 0; i < PAYLOAD_BYTES; i++) {
            x ^= frame[3 + i];
        }
        frame[3 + PAYLOAD_BYTES] = x;
        uart_write_bytes(SERIAL_PORT, frame, FRAME_BYTES);
        seq++;

        /* Bluetooth, only while the PC has an audio link open */
        if (s_sco_mode) {
            bt_push(mono);
        }
    }
}

/* ---------------- Bluetooth ---------------- */
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(TAG, "pairing failed, status %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT: /* Secure Simple Pairing "just works" */
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: { /* legacy pairing fallback, PIN 0000 */
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }
    default:
        break;
    }
}

static void hf_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT: {
        esp_hf_client_connection_state_t st = param->conn_stat.state;
        ESP_LOGI(TAG, "HFP connection state %d", st);
        if (st == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            s_slc_up = true;
        } else if (st == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            s_slc_up = false;
            s_sco_mode = 0;
            bt_ring_drain();
        }
        break;
    }
    case ESP_HF_CLIENT_AUDIO_STATE_EVT: {
        esp_hf_client_audio_state_t st = param->audio_stat.state;
        ESP_LOGI(TAG, "HFP audio state %d", st);
        if (st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            s_sco_mode = 0;
            bt_ring_drain();
            s_sco_mode = 1; /* CVSD, 8 kHz */
        } else if (st == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            s_sco_mode = 0;
            bt_ring_drain();
            s_sco_mode = 2; /* mSBC, 16 kHz */
        } else if (st == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            s_sco_mode = 0;
            bt_ring_drain();
        }
        break;
    }
    default:
        break;
    }
}

/* After a reboot, try to reconnect to the PC we were paired with.
 * Backs off (15 s up to 5 min) so a failing or already-connected peer
 * does not flood the link. The PC can also connect to us at any time. */
static void reconnect_task(void *arg)
{
    uint32_t delay_s = 15;
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (1) {
        if (s_slc_up) {
            delay_s = 15;
        } else {
            int n = esp_bt_gap_get_bond_device_num();
            if (n > 0) {
                esp_bd_addr_t *list = malloc(sizeof(esp_bd_addr_t) * n);
                if (list) {
                    int cnt = n;
                    if (esp_bt_gap_get_bond_device_list(&cnt, list) == ESP_OK && cnt > 0) {
                        ESP_LOGI(TAG, "reconnecting to bonded device");
                        esp_hf_client_connect(list[0]);
                    }
                    free(list);
                }
            }
            if (delay_s < 300) {
                delay_s *= 2;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
    }
}

static void bt_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_bt_dev_set_device_name(BT_NAME));

    /* No input / no output: Linux pairs without a PIN prompt */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);

    /* Advertise as an audio hands-free device so desktops show a headset icon */
    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_AV;
    cod.minor = 2; /* hands-free */
    cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_CAPTURING;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);

    ESP_ERROR_CHECK(esp_hf_client_register_callback(hf_cb));
    ESP_ERROR_CHECK(esp_hf_client_init());
    ESP_ERROR_CHECK(esp_hf_client_register_data_callback(hf_incoming_cb, hf_outgoing_cb));

    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));
    ESP_LOGI(TAG, "Bluetooth ready, discoverable as \"%s\"", BT_NAME);
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    /* keep log text out of the audio stream as much as possible */
    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    s_rb = xRingbufferCreate(BT_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    configASSERT(s_rb);

    serial_init();
    mic_init();
    bt_init();

    xTaskCreatePinnedToCore(capture_task, "capture", 4096, NULL, 6, NULL, 1);
    xTaskCreate(reconnect_task, "bt_reconn", 3072, NULL, 3, NULL);
}
