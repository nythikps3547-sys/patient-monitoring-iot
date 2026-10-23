#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "secrets.h"   // Keep this file private; do not upload it to public GitHub.

// AI Thinker ESP32-CAM pin mapping
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

static httpd_handle_t camera_httpd = NULL;  // Port 80: status/web page
static httpd_handle_t stream_httpd = NULL;  // Port 81: MJPEG stream

static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
static const char *STREAM_BOUNDARY =
    "\r\n--123456789000000000000987654321\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Port 80: camera page plus compatibility stream endpoint. Port 81 remains
// the preferred dedicated stream server for the PatientCare dashboard.
static esp_err_t index_handler(httpd_req_t *req) {
  const char page[] =
      "<!doctype html><html><head>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>PatientCare Camera</title></head>"
      "<body style='margin:0;padding:16px;background:#111;color:#fff;"
      "text-align:center;font-family:Arial,sans-serif'>"
      "<h2>PatientCare Live Camera</h2>"
      "<img id='cam' alt='Camera stream loading' "
      "style='width:100%;max-width:640px;height:auto;background:#222'>"
      "<p id='status'>Connecting to camera stream...</p>"
      "<script>"
      "const streamUrl=location.protocol+'//'+location.hostname+':81/stream';"
      "const img=document.getElementById('cam');"
      "img.src=streamUrl;"
      "img.onload=()=>document.getElementById('status').textContent='Live stream connected';"
      "img.onerror=()=>document.getElementById('status').textContent='Stream unavailable. Check Wi-Fi and port 81.';"
      "</script></body></html>";

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

// Port 81: continuous MJPEG stream. Kept on a separate server so a long-running
// stream request does not occupy the server that handles the webpage.
static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");

  char part_buf[64];

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
      Serial.println("ERROR: Failed to capture camera frame.");
      return ESP_FAIL;
    }

    if (fb->format != PIXFORMAT_JPEG) {
      esp_camera_fb_return(fb);
      Serial.println("ERROR: Camera frame is not JPEG.");
      return ESP_FAIL;
    }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));

    if (res == ESP_OK) {
      int hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART,
                          static_cast<unsigned int>(fb->len));
      if (hlen < 0 || hlen >= (int)sizeof(part_buf)) {
        res = ESP_FAIL;
      } else {
        res = httpd_resp_send_chunk(req, part_buf, hlen);
      }
    }

    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(fb->buf), fb->len);
    }

    esp_camera_fb_return(fb);

    if (res != ESP_OK) {
      // A browser closing the stream normally reaches this path.
      break;
    }

    delay(1);
  }

  return res;
}

static void startCameraServer() {
  httpd_config_t page_config = HTTPD_DEFAULT_CONFIG();
  page_config.server_port = 80;
  page_config.ctrl_port = 32768;
  page_config.max_uri_handlers = 4;
  page_config.lru_purge_enable = true;

  httpd_uri_t index_uri = {};
  index_uri.uri = "/";
  index_uri.method = HTTP_GET;
  index_uri.handler = index_handler;
  index_uri.user_ctx = NULL;

  httpd_uri_t compatibility_stream_uri = {};
  compatibility_stream_uri.uri = "/stream";
  compatibility_stream_uri.method = HTTP_GET;
  compatibility_stream_uri.handler = stream_handler;
  compatibility_stream_uri.user_ctx = NULL;

  if (httpd_start(&camera_httpd, &page_config) == ESP_OK) {
    if (httpd_register_uri_handler(camera_httpd, &index_uri) != ESP_OK) {
      Serial.println("ERROR: Could not register camera webpage handler.");
    }
    if (httpd_register_uri_handler(camera_httpd, &compatibility_stream_uri) != ESP_OK) {
      Serial.println("WARNING: Could not register port-80 compatibility stream endpoint.");
    }
  } else {
    Serial.println("ERROR: Camera webpage server failed to start on port 80.");
  }

  httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port = 81;
  stream_config.ctrl_port = 32769;  // Must differ from the port-80 server.
  stream_config.max_uri_handlers = 2;
  stream_config.lru_purge_enable = true;

  httpd_uri_t stream_uri = {};
  stream_uri.uri = "/stream";
  stream_uri.method = HTTP_GET;
  stream_uri.handler = stream_handler;
  stream_uri.user_ctx = NULL;

  if (httpd_start(&stream_httpd, &stream_config) == ESP_OK) {
    if (httpd_register_uri_handler(stream_httpd, &stream_uri) != ESP_OK) {
      Serial.println("ERROR: Could not register stream handler.");
    }
  } else {
    Serial.println("ERROR: Stream server failed to start on port 81.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("PatientCare ESP32-CAM starting...");

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  // Correct D0-D7 order for the AI Thinker camera.
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  const bool has_psram = psramFound();
  config.frame_size = has_psram ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count = has_psram ? 2 : 1;
  config.fb_location = has_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  Serial.printf("PSRAM: %s\n", has_psram ? "found" : "not found; using QVGA");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    Serial.println("Check board selection, PSRAM setting, camera module and power.");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi connection failed. Check secrets.h and Wi-Fi signal.");
    return;
  }

  Serial.println("\nWi-Fi connected!");
  Serial.print("Camera webpage: http://");
  Serial.println(WiFi.localIP());
  Serial.print("Live stream URL: http://");
  Serial.print(WiFi.localIP());
  Serial.println(":81/stream");
  Serial.println("Open the stream URL directly on a device connected to the same Wi-Fi.");
  Serial.print("Compatibility stream URL: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/stream");

  startCameraServer();
}

void loop() {
  delay(1000);
}
