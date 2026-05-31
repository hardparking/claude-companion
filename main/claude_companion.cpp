// claude_companion — M5Stack Fire as an ambient "Claude is working" indicator.
//
// Connects to WiFi, runs a tiny HTTP server, and animates a dancing 8-bit
// Space-Invaders-style alien on the LCD. The motion + color reflect what
// Claude is doing:
//   idle | thinking | working | waiting | done
// Set the state with:  GET http://<ip>/state?s=working
//
// Built for ESP-IDF + M5GFX (no Arduino).

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "led_strip.h"

#include <M5GFX.h>

static const char *TAG = "claude_fire";

// Background color — pure black, for that arcade-cabinet look.
#define BG565 0x0000

// ---- config -----------------------------------------------------------------
// WiFi credentials live in wifi_secrets.h, which is gitignored. Copy
// wifi_secrets.h.example to wifi_secrets.h and fill in your network.
#include "wifi_secrets.h"
#define MDNS_HOST "claude-fire"   // reachable as claude-fire.local (if mDNS works)

// ---- display ----------------------------------------------------------------
static M5GFX display;
static M5Canvas canvas(&display);     // 240x240 off-screen back-buffer

// ---- on-board LED bars ------------------------------------------------------
// The M5Stack Fire has 10 SK6812 RGB LEDs (two side bars of 5) on GPIO 15.
// We mirror the screen state onto them: same per-state color, matching motion.
#define LED_GPIO    15
#define LED_COUNT   10
#define LED_MAX     0.35f             // global brightness cap (these bars are bright)
static led_strip_handle_t g_leds = nullptr;

// Light every LED to one color scaled by `bright` (0..1). SK6812 wants GRB,
// which the strip is configured for, so we pass plain R,G,B here.
static void led_fill(uint32_t rgb, float bright) {
  if (!g_leds) return;
  bright *= LED_MAX;
  uint8_t r = (uint8_t)(((rgb >> 16) & 0xFF) * bright);
  uint8_t g = (uint8_t)(((rgb >> 8) & 0xFF) * bright);
  uint8_t b = (uint8_t)((rgb & 0xFF) * bright);
  for (int i = 0; i < LED_COUNT; i++) led_strip_set_pixel(g_leds, i, r, g, b);
}

// ---- 8-bit invader sprite ---------------------------------------------------
// Classic 11x8 "crab" invader, two animation frames for the leg/arm shuffle.
// Each row is 11 bits; the MSB (bit 10) is the leftmost pixel.
#define INV_W 11
#define INV_H 8
static const uint16_t INV_A[INV_H] = {260, 136, 508, 886, 2047, 1533, 1285, 216};
static const uint16_t INV_B[INV_H] = {260, 1161, 1533, 1911, 2047, 1022, 260, 514};

// ---- state ------------------------------------------------------------------
enum State { ST_IDLE = 0, ST_THINKING, ST_WORKING, ST_WAITING, ST_DONE };
static std::atomic<int> g_state{ST_IDLE};
static std::atomic<uint32_t> g_state_since_ms{0};
static char g_ip[20] = "...";

static inline uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static const char *state_name(int s) {
  switch (s) {
    case ST_IDLE:     return "idle";
    case ST_THINKING: return "thinking";
    case ST_WORKING:  return "working";
    case ST_WAITING:  return "waiting";
    case ST_DONE:     return "done";
  }
  return "?";
}

static int parse_state(const char *s) {
  if (!strcmp(s, "idle"))     return ST_IDLE;
  if (!strcmp(s, "thinking")) return ST_THINKING;
  if (!strcmp(s, "working"))  return ST_WORKING;
  if (!strcmp(s, "waiting"))  return ST_WAITING;
  if (!strcmp(s, "done"))     return ST_DONE;
  return -1;
}

static void set_state(int s) {
  g_state.store(s);
  g_state_since_ms.store(now_ms());
  ESP_LOGI(TAG, "state -> %s", state_name(s));
}

// ---- animation --------------------------------------------------------------
// gaussian bump
static inline float g_bump(float x, float k) { return expf(-x * x * k); }

// double-thump heartbeat in [0..~1.2], period ~1.1s
static float heartbeat(float t) {
  float p = fmodf(t, 1.10f);
  return g_bump(p - 0.10f, 130.f) + 0.65f * g_bump(p - 0.32f, 130.f);
}

static void draw_frame(float secs) {
  int s = g_state.load();
  uint32_t since = now_ms() - g_state_since_ms.load();
  if (s == ST_DONE && since > 2600) { set_state(ST_IDLE); s = ST_IDLE; since = 0; }
  // Watchdog: active states relax to idle if no update arrives. Hooks fire
  // constantly during real work, so this only trips once Claude goes quiet —
  // it makes the display "stuck" impossible even if a Stop hook is missed or
  // fire-and-forget updates land out of order.
  if ((s == ST_THINKING || s == ST_WORKING) && since > 12000) {
    set_state(ST_IDLE); s = ST_IDLE; since = 0;
  }

  // --- per-state choreography --------------------------------------------
  uint32_t col = 0x66E0C0;   // invader body color
  uint32_t lbl = 0xAAAAAA;   // label color
  float cx = 120.f, cy = 118.f;     // body center
  float pxw = 15.f, pxh = 15.f;     // fat-pixel size (w/h = squash & stretch)
  float flipHz = 1.6f;              // leg-shuffle speed

  switch (s) {
    case ST_IDLE:                                  // calm idle sway
      col = 0x4FB0C0; lbl = 0x4FB0C0;
      cy += 5.f * sinf(secs * 2.0f);
      flipHz = 1.2f;
      break;
    case ST_THINKING:                              // marching side to side
      col = 0xD97757; lbl = 0xD97757;
      cx += 30.f * sinf(secs * 1.7f);
      cy += 3.f * sinf(secs * 3.4f);
      flipHz = 3.4f;
      break;
    case ST_WORKING: {                             // full-on dance: strut + hop + wiggle
      col = 0xF59030; lbl = 0xF5A858;
      cx += 34.f * sinf(secs * 3.6f);
      cy += 16.f * fabsf(sinf(secs * 7.5f));       // hops
      float sq = 0.14f * sinf(secs * 15.f);        // squash & stretch
      pxw = 15.f * (1.f + sq); pxh = 15.f * (1.f - sq);
      flipHz = 8.f;
      break;
    }
    case ST_WAITING: {                             // tense heartbeat pulse
      col = 0xFFC166; lbl = 0xFFC166;
      float hb = heartbeat(secs);
      pxw = pxh = 15.f * (1.f + 0.12f * hb);
      cy += 2.f * sinf(secs * 1.6f);
      flipHz = 1.0f;
      break;
    }
    case ST_DONE: {                                // victory jumps
      col = 0x7ED98A; lbl = 0x7ED98A;
      float ph = since / 1000.f;
      cy -= 34.f * expf(-ph * 2.6f) * fabsf(sinf(ph * 11.f));
      float sq = 0.18f * expf(-ph * 2.6f) * sinf(ph * 22.f);
      pxw = 15.f * (1.f + sq); pxh = 15.f * (1.f - sq);
      flipHz = 9.f;
      break;
    }
  }

  canvas.fillScreen(BG565);

  // Twinkling starfield (deterministic positions, no RNG needed).
  for (int i = 0; i < 26; i++) {
    int sx = (i * 97 + 13) % 240;
    int sy = (i * 53 + 29) % 240;
    float tw = 0.5f + 0.5f * sinf(secs * 2.5f + i * 1.3f);
    uint8_t b = (uint8_t)(45 + tw * 130);
    canvas.drawPixel(sx, sy, canvas.color565(b, b, b));
  }

  // The invader itself: pick the leg frame, draw as fat pixels.
  const uint16_t *rows = (fmodf(secs * flipHz, 1.f) < 0.5f) ? INV_A : INV_B;
  uint16_t bodyc = canvas.color565((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
  float x0 = cx - INV_W * pxw / 2.f;
  float y0 = cy - INV_H * pxh / 2.f;
  for (int r = 0; r < INV_H; r++) {
    uint16_t bits = rows[r];
    for (int c = 0; c < INV_W; c++) {
      if (bits & (1u << (INV_W - 1 - c))) {
        canvas.fillRect((int)(x0 + c * pxw), (int)(y0 + r * pxh),
                        (int)(pxw + 0.9f), (int)(pxh + 0.9f), bodyc);
      }
    }
  }

  canvas.setTextColor(canvas.color565((lbl >> 16) & 0xFF, (lbl >> 8) & 0xFF, lbl & 0xFF));
  canvas.setTextDatum(textdatum_t::bottom_center);
  canvas.setTextSize(2);
  canvas.drawString(state_name(s), 120, 234);

  canvas.setTextColor(canvas.color565(0x55, 0x55, 0x55));
  canvas.setTextDatum(textdatum_t::top_center);
  canvas.setTextSize(1);
  canvas.drawString(g_ip, 120, 6);

  canvas.pushSprite((display.width() - 240) / 2, (display.height() - 240) / 2);
}

// Mirror the current state onto the LED bars. Colors match draw_frame's palette;
// each state gets motion that echoes the alien's: idle breathes, thinking sweeps
// a dot along the bar, working pulses fast, waiting throbs on the heartbeat, done
// flashes a decaying victory burst. State/watchdog transitions are owned by
// draw_frame (which runs first each frame), so we just read g_state here.
static void update_leds(float secs) {
  if (!g_leds) return;
  int s = g_state.load();
  uint32_t since = now_ms() - g_state_since_ms.load();

  switch (s) {
    case ST_IDLE: {                                // slow cyan breathe
      float b = 0.10f + 0.18f * (0.5f + 0.5f * sinf(secs * 2.0f));
      led_fill(0x4FB0C0, b);
      break;
    }
    case ST_THINKING: {                            // orange dot sweeping the bar
      led_fill(0xD97757, 0.05f);
      float pos = fmodf(secs * 1.7f, 2.0f);        // 0..2 -> ping-pong over 0..9
      pos = pos < 1.0f ? pos : 2.0f - pos;
      float head = pos * (LED_COUNT - 1);
      for (int i = 0; i < LED_COUNT; i++) {
        float d = i - head;
        float glow = g_bump(d, 0.7f);              // soft comet around the head
        if (glow > 0.02f) led_strip_set_pixel(g_leds, i,
            (uint8_t)(0xD9 * glow * LED_MAX),
            (uint8_t)(0x77 * glow * LED_MAX),
            (uint8_t)(0x57 * glow * LED_MAX));
      }
      break;
    }
    case ST_WORKING: {                             // fast orange pulse, full bar
      float b = 0.45f + 0.55f * fabsf(sinf(secs * 7.5f));
      led_fill(0xF59030, b);
      break;
    }
    case ST_WAITING: {                             // amber heartbeat throb
      float b = 0.10f + 0.55f * heartbeat(secs);
      led_fill(0xFFC166, b > 1.f ? 1.f : b);
      break;
    }
    case ST_DONE: {                                // decaying green victory burst
      float ph = since / 1000.f;
      float b = expf(-ph * 2.6f) * (0.5f + 0.5f * fabsf(sinf(ph * 11.f)));
      led_fill(0x7ED98A, 0.08f + 0.92f * b);
      break;
    }
  }
  led_strip_refresh(g_leds);
}

static void led_start(void) {
  led_strip_config_t scfg = {};
  scfg.strip_gpio_num = LED_GPIO;
  scfg.max_leds = LED_COUNT;
  scfg.led_model = LED_MODEL_SK6812;
  scfg.led_pixel_format = LED_PIXEL_FORMAT_GRB;
  led_strip_rmt_config_t rcfg = {};
  rcfg.clk_src = RMT_CLK_SRC_DEFAULT;
  rcfg.resolution_hz = 10 * 1000 * 1000;          // 10 MHz, standard for SK6812
  if (led_strip_new_rmt_device(&scfg, &rcfg, &g_leds) != ESP_OK) {
    ESP_LOGE(TAG, "led strip init failed");
    g_leds = nullptr;
    return;
  }
  led_strip_clear(g_leds);
}

static void anim_task(void *arg) {
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  if (!canvas.createSprite(240, 240)) { ESP_LOGE(TAG, "canvas alloc failed"); vTaskDelete(NULL); }

  led_start();

  uint32_t t0 = now_ms();
  while (true) {
    float secs = (now_ms() - t0) / 1000.f;
    draw_frame(secs);
    update_leds(secs);
    vTaskDelay(pdMS_TO_TICKS(30));   // ~33 fps
  }
}

// ---- http server ------------------------------------------------------------
static esp_err_t state_handler(httpd_req_t *req) {
  char q[64] = {0}, val[24] = {0};
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "s", val, sizeof(val)) == ESP_OK) {
    int s = parse_state(val);
    if (s >= 0) {
      set_state(s);
      httpd_resp_set_type(req, "text/plain");
      httpd_resp_sendstr(req, state_name(s));
      return ESP_OK;
    }
  }
  httpd_resp_set_status(req, "400 Bad Request");
  httpd_resp_sendstr(req, "use ?s=idle|thinking|working|waiting|done");
  return ESP_OK;
}

static esp_err_t root_handler(httpd_req_t *req) {
  char buf[160];
  snprintf(buf, sizeof(buf),
           "claude-fire ok\nip: %s\nstate: %s\ntry: /state?s=working\n",
           g_ip, state_name(g_state.load()));
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, buf);
  return ESP_OK;
}

static void start_http(void) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.lru_purge_enable = true;
  httpd_handle_t srv = NULL;
  if (httpd_start(&srv, &cfg) == ESP_OK) {
    httpd_uri_t u_state = {.uri = "/state", .method = HTTP_GET, .handler = state_handler};
    httpd_uri_t u_root  = {.uri = "/",      .method = HTTP_GET, .handler = root_handler};
    httpd_register_uri_handler(srv, &u_state);
    httpd_register_uri_handler(srv, &u_root);
    ESP_LOGI(TAG, "http server up");
  } else {
    ESP_LOGE(TAG, "http server failed");
  }
}

// ---- wifi -------------------------------------------------------------------
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    strcpy(g_ip, "reconnecting");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, " CLAUDE-FIRE ONLINE  ->  http://%s/", g_ip);
    ESP_LOGI(TAG, "===========================================");
  }
}

static void wifi_start(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&ic));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL));
  wifi_config_t wc = {};
  strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
  strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
  wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
}

static void start_mdns(void) {
  if (mdns_init() != ESP_OK) return;
  mdns_hostname_set(MDNS_HOST);
  mdns_instance_name_set("Claude Fire");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

// ---- main -------------------------------------------------------------------
extern "C" void app_main(void) {
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  display.begin();
  display.setRotation(1);
  display.setBrightness(180);
  display.fillScreen(BG565);

  set_state(ST_IDLE);
  xTaskCreatePinnedToCore(anim_task, "anim", 6144, NULL, 5, NULL, 1);

  wifi_start();
  start_mdns();
  start_http();
}
