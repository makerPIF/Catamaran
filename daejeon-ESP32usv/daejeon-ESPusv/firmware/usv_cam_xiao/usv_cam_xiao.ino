/* =====================================================================
   대전중앙중학교 USV - 영상 송신 모듈
   Board  : Seeed Studio XIAO ESP32S3 Sense (OV2640 카메라 보드)
   기능   : MJPEG 스트리밍 (GCS의 <img> 태그가 바로 받아 표시)
            + 단일 스냅샷 + 화질/해상도 원격 조절 + 상태 JSON

   빌드 준비 (Arduino IDE)
   -----------------------------------------------------------------
   1) 보드 매니저 URL (메인 보드와 동일)
        https://espressif.github.io/arduino-esp32/package_esp32_index.json
   2) 보드 선택 : "XIAO_ESP32S3"
   3) 도구 설정 (중요!)
        - PSRAM            : "OPI PSRAM"      <-- 반드시 켜야 함
        - USB CDC On Boot  : "Enabled"
        - Partition Scheme : "Huge APP (3MB No OTA/1MB SPIFFS)"
   4) 카메라 확장보드(Sense 확장부)를 딸깍 소리가 날 때까지 결합할 것
   5) 라이브러리 추가 설치 불필요 (esp32 코어에 esp32-camera 포함)

   접속 확인 : 브라우저에서  http://192.168.8.30/stream
   ===================================================================== */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_timer.h"

/* ===================== 1. 사용자 설정 ===================== */
static const char* WIFI_SSID = "대전중앙중학교";
static const char* WIFI_PASS = "2026USV";

IPAddress LOCAL_IP (192, 168, 8, 30);   // 카메라 모듈 고정 IP
IPAddress GATEWAY  (192, 168, 8, 1);
IPAddress SUBNET   (255, 255, 255, 0);

// 초기 화질 : 장거리에서는 낮은 해상도가 훨씬 안정적입니다.
//   FRAMESIZE_QVGA (320x240)  : 최대 사거리 / 가장 안정
//   FRAMESIZE_VGA  (640x480)  : 균형                    <-- 기본
//   FRAMESIZE_SVGA (800x600)  : 근거리 고화질
static framesize_t START_SIZE    = FRAMESIZE_VGA;
static int         START_QUALITY = 14;   // 10(고화질/고용량) ~ 30(저화질/저용량)

/* ===================== 2. XIAO ESP32S3 Sense 카메라 핀맵 ===================== */
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   10
#define SIOD_GPIO_NUM   40
#define SIOC_GPIO_NUM   39
#define Y9_GPIO_NUM     48
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     12
#define Y6_GPIO_NUM     14
#define Y5_GPIO_NUM     16
#define Y4_GPIO_NUM     18
#define Y3_GPIO_NUM     17
#define Y2_GPIO_NUM     15
#define VSYNC_GPIO_NUM  38
#define HREF_GPIO_NUM   47
#define PCLK_GPIO_NUM   13

#define LED_PIN         21   // XIAO 온보드 LED (LOW 에서 점등)

/* ===================== 3. 전역 ===================== */
httpd_handle_t camera_httpd = NULL;
static float   g_fps = 0;
static uint32_t g_frames = 0;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

/* ===================== 4. 핸들러 ===================== */
static esp_err_t stream_handler(httpd_req_t* req) {
  camera_fb_t* fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];
  int64_t last = esp_timer_get_time();

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "30");

  digitalWrite(LED_PIN, LOW);            // 스트리밍 중 LED 점등

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }

    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);

    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;

    int64_t now = esp_timer_get_time();
    float dt = (now - last) / 1000000.0f;
    last = now;
    if (dt > 0) g_fps = g_fps * 0.9f + (1.0f / dt) * 0.1f;
    g_frames++;
  }

  digitalWrite(LED_PIN, HIGH);
  return res;
}

static esp_err_t capture_handler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=usv.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

/* /control?size=QVGA|VGA|SVGA&quality=10..30&vflip=0|1&hmirror=0|1 */
static esp_err_t control_handler(httpd_req_t* req) {
  char q[128] = {0}, v[16];
  sensor_t* s = esp_camera_sensor_get();
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
    if (httpd_query_key_value(q, "size", v, sizeof(v)) == ESP_OK) {
      if      (!strcmp(v, "QVGA")) s->set_framesize(s, FRAMESIZE_QVGA);
      else if (!strcmp(v, "VGA"))  s->set_framesize(s, FRAMESIZE_VGA);
      else if (!strcmp(v, "SVGA")) s->set_framesize(s, FRAMESIZE_SVGA);
      else if (!strcmp(v, "HVGA")) s->set_framesize(s, FRAMESIZE_HVGA);
    }
    if (httpd_query_key_value(q, "quality", v, sizeof(v)) == ESP_OK)
      s->set_quality(s, constrain(atoi(v), 10, 40));
    if (httpd_query_key_value(q, "vflip", v, sizeof(v)) == ESP_OK)
      s->set_vflip(s, atoi(v));
    if (httpd_query_key_value(q, "hmirror", v, sizeof(v)) == ESP_OK)
      s->set_hmirror(s, atoi(v));
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":1}");
  return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t* req) {
  char buf[200];
  sensor_t* s = esp_camera_sensor_get();
  snprintf(buf, sizeof(buf),
           "{\"ok\":1,\"rssi\":%d,\"fps\":%.1f,\"frames\":%lu,"
           "\"size\":%d,\"quality\":%d,\"heap\":%u,\"psram\":%u}",
           WiFi.RSSI(), g_fps, (unsigned long)g_frames,
           s->status.framesize, s->status.quality,
           ESP.getFreeHeap(), ESP.getFreePsram());
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr(req, buf);
  return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr(req,
    "<meta charset='utf-8'><body style='margin:0;background:#111;color:#eee;"
    "font-family:system-ui'><h3 style='padding:8px'>DJMS USV CAM</h3>"
    "<img src='/stream' style='width:100%;max-width:800px'>"
    "<p style='padding:8px'>/stream /capture /status /control</p></body>");
  return ESP_OK;
}

void startServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port      = 80;
  config.ctrl_port        = 32768;
  config.max_uri_handlers = 8;
  config.stack_size       = 8192;
  config.lru_purge_enable = true;

  httpd_uri_t uris[] = {
    { "/",        HTTP_GET, index_handler,   NULL },
    { "/stream",  HTTP_GET, stream_handler,  NULL },
    { "/capture", HTTP_GET, capture_handler, NULL },
    { "/control", HTTP_GET, control_handler, NULL },
    { "/status",  HTTP_GET, status_handler,  NULL },
  };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    for (auto& u : uris) httpd_register_uri_handler(camera_httpd, &u);
    Serial.println("[CAM] HTTP 서버 시작");
  }
}

/* ===================== 5. setup / loop ===================== */
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n===== DJMS USV Camera (XIAO ESP32S3 Sense) =====");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;   c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;   c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;   c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;   c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;   c.pin_pclk  = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href  = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;   c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.frame_size   = START_SIZE;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode    = CAMERA_GRAB_LATEST;      // 항상 최신 프레임 (지연 최소)
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.jpeg_quality = START_QUALITY;
  c.fb_count     = psramFound() ? 2 : 1;
  if (!psramFound()) {
    Serial.println("[CAM] 경고: PSRAM 미검출 - 도구>PSRAM을 'OPI PSRAM'으로 설정하세요");
    c.frame_size = FRAMESIZE_QVGA;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[CAM] 초기화 실패 0x%x - 카메라 커넥터를 확인하세요\n", err);
    delay(3000);
    ESP.restart();
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, 0);          // 카메라를 거꾸로 달았다면 1
  s->set_hmirror(s, 0);
  s->set_brightness(s, 1);     // 수면 반사 대응
  s->set_saturation(s, 0);
  s->set_gainceiling(s, GAINCEILING_2X);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.config(LOCAL_IP, GATEWAY, SUBNET, GATEWAY);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] 접속 중");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300); Serial.print('.');
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WiFi] 실패 - 재부팅"); delay(1000); ESP.restart();
  }
  Serial.printf("\n[WiFi] IP=%s  RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  Serial.printf("[CAM] 스트림 주소: http://%s/stream\n",
                WiFi.localIP().toString().c_str());

  startServer();
}

void loop() {
  // Wi-Fi 가 끊기면 스스로 복구
  static uint32_t t = 0;
  if (millis() - t > 5000) {
    t = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] 재접속 시도");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
  delay(100);
}
