#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "coap3/coap.h"

static const char *TAG = "coap_demo";
#define COAP_PORT 5683

static float g_current_temp = 24.5f;
static coap_resource_t *g_env_temp_resource = NULL;
static uint32_t g_notify_count = 0;

// CBOR encoder for {"t": <float16>} — six bytes.
//   A1            map(1)
//   61 74         text(1) "t"
//   F9 hh ll      float16, big-endian, IEEE 754 half-precision

static uint16_t float32_to_float16(float f) {
  uint32_t x;
  memcpy(&x, &f, sizeof(x));
  uint32_t sign = (x >> 16) & 0x8000;
  int32_t exp = ((x >> 23) & 0xFF) - 127 + 15;
  uint32_t mant = (x >> 13) & 0x3FF;
  if (exp <= 0)
    return (uint16_t)sign; // underflow → ±0
  if (exp >= 31)
    return (uint16_t)(sign | 0x7C00); // overflow → ±inf
  return (uint16_t)(sign | ((uint32_t)exp << 10) | mant);
}

static size_t encode_env_temp_cbor(float value, uint8_t out[6]) {
  uint16_t h = float32_to_float16(value);
  out[0] = 0xA1;
  out[1] = 0x61;
  out[2] = 0x74;
  out[3] = 0xF9;
  out[4] = (uint8_t)(h >> 8);
  out[5] = (uint8_t)(h & 0xFF);
  return 6;
}

// /env/temp GET handler — libcoap-3 opaque-PDU API
static void hnd_env_temp_get(coap_resource_t *resource, coap_session_t *session,
                             const coap_pdu_t *request,
                             const coap_string_t *query, coap_pdu_t *response) {
  (void)session;
  (void)request;
  (void)query;

  uint8_t buf[6];
  size_t len = encode_env_temp_cbor(g_current_temp, buf);

  coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT); // 2.05

  unsigned char encoded[4];
  coap_add_option(response, COAP_OPTION_CONTENT_FORMAT,
                  coap_encode_var_safe(encoded, sizeof(encoded),
                                       COAP_MEDIATYPE_APPLICATION_CBOR),
                  encoded);
  ESP_LOGI(TAG, "CoAP response payload bytes: %u", (unsigned)len);
  coap_add_data(response, len, buf);

  ESP_LOGI(TAG, "GET /env/temp -> %.2f C (6 B CBOR)", g_current_temp);
}

// Simulates a drifting sensor. Called from the main loop every 5 s.
// Notifies observers on every change so Observe is easy to verify.
static void update_temp_and_notify(coap_context_t *ctx) {
  float delta =
      ((float)(esp_random() % 1000) / 1000.0f - 0.5f) * 0.6f; // ±0.3
  if ((esp_random() % 10) == 0)
    delta += (esp_random() & 1) ? 1.5f : -1.5f;
  g_current_temp += delta;

  g_notify_count++;
  ESP_LOGI(TAG, "[%u] temp=%.2f C, notifying observers", g_notify_count, g_current_temp);
  if (g_env_temp_resource)
    coap_resource_notify_observers(g_env_temp_resource, NULL);
}

static void coap_server_task(void *pvParameters) {
  (void)pvParameters;

  coap_address_t addr;
  coap_address_init(&addr);
  addr.addr.sin6.sin6_family = AF_INET6;
  addr.addr.sin6.sin6_port = htons(COAP_PORT);
  addr.addr.sin6.sin6_addr = in6addr_any;

  coap_set_log_level(COAP_LOG_WARN);
  coap_context_t *ctx = coap_new_context(NULL);
  if (!ctx) {
    ESP_LOGE(TAG, "coap_new_context failed");
    vTaskDelete(NULL);
    return;
  }
  coap_context_set_block_mode(ctx, COAP_BLOCK_USE_LIBCOAP);

  if (!coap_new_endpoint(ctx, &addr, COAP_PROTO_UDP)) {
    ESP_LOGE(TAG, "coap_new_endpoint failed");
    coap_free_context(ctx);
    vTaskDelete(NULL);
    return;
  }

  g_env_temp_resource = coap_resource_init(coap_make_str_const("env/temp"), 0);
  coap_register_handler(g_env_temp_resource, COAP_REQUEST_GET,
                        hnd_env_temp_get);
  coap_resource_set_get_observable(g_env_temp_resource, 1);
  coap_add_resource(ctx, g_env_temp_resource);

  ESP_LOGI(TAG, "CoAP server listening on UDP/%d, resource /env/temp",
           COAP_PORT);

  TickType_t last_update = xTaskGetTickCount();
  while (1) {
    coap_io_process(ctx, 1000);
    if ((xTaskGetTickCount() - last_update) >= pdMS_TO_TICKS(5000)) {
      last_update = xTaskGetTickCount();
      update_temp_and_notify(ctx);
    }
  }
}

void start_coap_server(void) {
  xTaskCreate(coap_server_task, "coap_server", 6144, NULL, 5, NULL);
}