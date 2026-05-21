#include <arpa/inet.h>
#include <math.h>
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

#define ENV_TEMP_MAX_AGE_S 60u
#define ENV_TEMP_HEARTBEAT_S 45u
#define ENV_TEMP_HEARTBEAT_TICKS pdMS_TO_TICKS(ENV_TEMP_HEARTBEAT_S * 1000)

static float g_current_temp = 24.5f;
static float g_last_notified_temp = 24.5f;
static TickType_t g_last_notify_tick = 0;
static coap_resource_t *g_env_temp_resource = NULL;
static int g_notify_count = 0;
static bool g_suppress_threshold = false;

static uint16_t float32_to_float16(float f) {
  uint32_t x;
  memcpy(&x, &f, sizeof(x));
  uint32_t sign = (x >> 16) & 0x8000;
  int32_t exp = ((x >> 23) & 0xFF) - 127 + 15;
  uint32_t mant = (x >> 13) & 0x3FF;
  if (exp <= 0)
    return (uint16_t)sign;
  if (exp >= 31)
    return (uint16_t)(sign | 0x7C00);
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

static void hnd_env_temp_get(coap_resource_t *resource, coap_session_t *session,
                             const coap_pdu_t *request,
                             const coap_string_t *query, coap_pdu_t *response) {
  (void)session;
  (void)request;
  (void)query;

  uint8_t buf[6];
  size_t len = encode_env_temp_cbor(g_current_temp, buf);

  coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);

  unsigned char encoded[4];
  coap_add_option(response, COAP_OPTION_CONTENT_FORMAT,
                  coap_encode_var_safe(encoded, sizeof(encoded),
                                       COAP_MEDIATYPE_APPLICATION_CBOR),
                  encoded);
  coap_add_option(
      response, COAP_OPTION_MAXAGE,
      coap_encode_var_safe(encoded, sizeof(encoded), ENV_TEMP_MAX_AGE_S),
      encoded);
  ESP_LOGI(TAG, "CoAP response payload bytes: %u", (unsigned)len);
  coap_add_data(response, len, buf);

  ESP_LOGI(TAG, "GET /env/temp -> %.2f C (6 B CBOR)", g_current_temp);
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

  // Temperature simulation parameters
  const float baseline = 24.5f;
  const float amp_c = 1.0f;
  const float period_s = 20.0f;
  TickType_t last_update = xTaskGetTickCount();
  g_last_notify_tick = xTaskGetTickCount();

  while (1) {
    // Process IO events (including sending pending notifications)
    coap_io_process(ctx, 100);

    TickType_t now = xTaskGetTickCount();
    if ((now - last_update) < pdMS_TO_TICKS(5000))
      continue;

    last_update = now;

    // Update simulated temperature
    float t_s = (float)(now * portTICK_PERIOD_MS) / 1000.0f;
    float noise = ((float)(esp_random() % 1000) / 1000.0f - 0.5f) * 0.2f;
    g_current_temp =
        baseline + amp_c * sinf(2.0f * (float)M_PI * t_s / period_s) + noise;

    // Check notification triggers
    float diff = fabsf(g_current_temp - g_last_notified_temp);
    TickType_t since = now - g_last_notify_tick;
    bool threshold = diff > 0.5f && !g_suppress_threshold;
    bool heartbeat = since >= ENV_TEMP_HEARTBEAT_TICKS;
    bool should_notify = threshold || heartbeat;

    if (threshold) {
      g_notify_count++;
      if (g_notify_count >= 10)
        g_suppress_threshold = true;
    }

    if (heartbeat) {
      ESP_LOGI(TAG, "heartbeat reset: re-enabling threshold notifications");
      g_notify_count = 0;
      g_suppress_threshold = false;
    }

    if (should_notify) {
      ESP_LOGI(TAG, "notify (%s): T=%.2f C, Δ=%.2f C, %lds since last",
               threshold ? "threshold" : "heartbeat", g_current_temp, diff,
               (long)(since * portTICK_PERIOD_MS / 1000));
      g_last_notified_temp = g_current_temp;
      g_last_notify_tick = xTaskGetTickCount();
      if (g_env_temp_resource)
        coap_resource_notify_observers(g_env_temp_resource, NULL);
    }
  }
}

void start_coap_server(void) {
  xTaskCreate(coap_server_task, "coap_server", 6144, NULL, 5, NULL);
}
