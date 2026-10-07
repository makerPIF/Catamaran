/* =====================================================================
   대전중앙중학교 USV - 영상 송신 모듈  [ESP32-CAM AI-Thinker 버전]
   Board  : AI-Thinker ESP32-CAM (OV2640)
   기능   : MJPEG 스트리밍 + 스냅샷 + 화질/해상도 원격 조절
            + 플래시 LED 원격 제어(저조도·회수용 탐조등)

   ※ XIAO ESP32S3 Sense 버전(usv_cam_xiao.ino)과 완전히 같은 주소 체계를
     쓰므로, GCS(index.html)는 고치지 않고 그대로 쓰면 됩니다.
         http://192.168.8.30/stream   /capture   /control   /status

   ─────────────────────────────────────────────────────────────────
   ★ 이 보드는 USB 단자가 없습니다. USB-TTL(FTDI) 어댑터가 필요합니다.
   ─────────────────────────────────────────────────────────────────

   [1] 배선 (업로드할 때)

        ESP32-CAM          USB-TTL 어댑터
        ---------          --------------
        5V          <----  5V          ※ 3.3V로 주면 부팅에 실패합니다
        GND         <----  GND
        U0R (GPIO3) <----  TX
        U0T (GPIO1) ---->  RX
        IO0         <----  GND         ※ 업로드할 때만! 끝나면 반드시 분리

   [2] 아두이노 IDE 설정
        보드 매니저 URL :
          https://espressif.github.io/arduino-esp32/package_esp32_index.json
        보드             : "AI Thinker ESP32-CAM"
        PSRAM            : "Enabled"  (코어 3.x 에서는 자동)
        Partition Scheme : "Huge APP (3MB No OTA/1MB SPIFFS)"
        Upload Speed     : 115200  (실패하면 더 낮춰 보세요)
        라이브러리 추가 설치 불필요 (esp32 코어에 esp32-camera 포함)
        ※ esp32 코어 2.0.9 이상에서 검증했습니다. 2.x / 3.x 모두 동작합니다.

   [3] 업로드 순서
        ① IO0 - GND 연결  →  ② 보드 RESET 버튼 한 번  →  ③ 업로드
        ④ "Hard resetting..." 이 뜨면  IO0 - GND 분리  →  ⑤ RESET 한 번

   [4] 운용할 때 배선 (업로드가 끝난 뒤)
        5V  ← UBEC #2 의 5V (2A 이상)
        GND ← 공통 GND
        IO0 는 아무 데도 연결하지 않습니다 (떠 있어야 정상 부팅)

   접속 확인 : 브라우저에서  http://192.168.8.30/stream
   ===================================================================== */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

/* ===================== 1. 사용자 설정 ===================== */
static const char* WIFI_SSID = "대전중앙중학교";
static const char* WIFI_PASS = "2026USV";

IPAddress LOCAL_IP (192, 168, 8, 30);   // 카메라 모듈 고정 IP (XIAO 버전과 동일)
IPAddress GATEWAY  (192, 168, 8, 1);
IPAddress SUBNET   (255, 255, 255, 0);

// 초기 화질 : 장거리에서는 낮은 해상도가 훨씬 안정적입니다.
//   FRAMESIZE_QVGA (320x240)  : 최대 사거리 / 가장 안정
//   FRAMESIZE_VGA  (640x480)  : 균형            <-- 기본
//   FRAMESIZE_SVGA (800x600)  : 근거리 고화질
static framesize_t START_SIZE    = FRAMESIZE_VGA;
static int         START_QUALITY = 12;   // 10(고화질/고용량) ~ 30(저화질/저용량)

// XCLK 주파수. 중국산 저가 클론에서 영상이 깨지거나 초기화에 실패하면
// 10000000 (10MHz) 으로 낮추세요. 프레임레이트는 조금 떨어집니다.
static const int XCLK_HZ = 20000000;

// 브라운아웃 감지기 끄기
//   ESP32-CAM 은 송신 순간 전류가 크게 튀어, 전원이 조금만 약해도
//   "Brownout detector was triggered" 를 내며 무한 재부팅합니다.
//   근본 해결은 튼튼한 5V 2A 전원 + 5V-GND 사이 470uF 전해 커패시터이고,
//   아래는 보조 수단입니다.
#define DISABLE_BROWNOUT  1

/* ===================== 2. AI-Thinker ESP32-CAM 핀맵 ===================== */
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

#define LED_FLASH       4    // 흰색 고휘도 플래시 LED (매우 밝음)
#define LED_STATUS     33    // 빨간 상태 LED (LOW 에서 점등)

/* LEDC 채널 : 코어 2.x / 3.x 양쪽에서 동작하도록 분기 */
#define FLASH_CH        7
#define FLASH_FREQ   5000
#define FLASH_RES       8    // 0 ~ 255

/* ===================== 3. 전역 ===================== */
httpd_handle_t camera_httpd = NULL;
static float    g_fps = 0;
static uint32_t g_frames = 0;
static uint8_t  g_flash = 0;      // 플래시 밝기 0~255

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

/* ===================== 4. 플래시 LED ===================== */
/* 아두이노 ESP32 코어 2.x 와 3.x 는 LEDC API 가 다릅니다.
   3.x 부터 ledcWrite() 의 첫 인자가 '채널 번호'에서 '핀 번호'로 바뀌었습니다.
   두 버전 모두에서 동작하도록 아래처럼 갈라 둡니다.                        */
void flashSet(uint8_t v) {
  g_flash = v;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LED_FLASH, v);     // 코어 3.x : 핀 번호
#else
  ledcWrite(FLASH_CH, v);      // 코어 2.x : 채널 번호
#endif
}
void flashBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttachChannel(LED_FLASH, FLASH_FREQ, FLASH_RES, FLASH_CH);
#else
  ledcSetup(FLASH_CH, FLASH_FREQ, FLASH_RES);
  ledcAttachPin(LED_FLASH, FLASH_CH);
#endif
  flashSet(0);
}

/* ===================== 5. 핸들러 ===================== */
static esp_err_t stream_handler(httpd_req_t* req) {
  camera_fb_t* fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];
  int64_t last = esp_timer_get_time();

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "30");

  digitalWrite(LED_STATUS, LOW);        // 스트리밍 중 빨간 LED 점등
                                        // (흰색 플래시는 켜지 않습니다 - 눈이 부십니다)
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

  digitalWrite(LED_STATUS, HIGH);
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

/* /control?size=QVGA|VGA|SVGA&quality=10..30&vflip=0|1&hmirror=0|1&led=0..255 */
static esp_err_t control_handler(httpd_req_t* req) {
  char q[160] = {0}, v[16];
  sensor_t* s = esp_camera_sensor_get();
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
    if (httpd_query_key_value(q, "size", v, sizeof(v)) == ESP_OK) {
      if      (!strcmp(v, "QVGA")) s->set_framesize(s, FRAMESIZE_QVGA);
      else if (!strcmp(v, "HVGA")) s->set_framesize(s, FRAMESIZE_HVGA);
      else if (!strcmp(v, "VGA"))  s->set_framesize(s, FRAMESIZE_VGA);
      else if (!strcmp(v, "SVGA")) s->set_framesize(s, FRAMESIZE_SVGA);
    }
    if (httpd_query_key_value(q, "quality", v, sizeof(v)) == ESP_OK)
      s->set_quality(s, constrain(atoi(v), 10, 40));
    if (httpd_query_key_value(q, "vflip", v, sizeof(v)) == ESP_OK)
      s->set_vflip(s, atoi(v));
    if (httpd_query_key_value(q, "hmirror", v, sizeof(v)) == ESP_OK)
      s->set_hmirror(s, atoi(v));
    if (httpd_query_key_value(q, "led", v, sizeof(v)) == ESP_OK)
      flashSet(constrain(atoi(v), 0, 255));
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":1}");
  return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t* req) {
  char buf[220];
  sensor_t* s = esp_camera_sensor_get();
  snprintf(buf, sizeof(buf),
           "{\"ok\":1,\"board\":\"esp32cam\",\"rssi\":%d,\"fps\":%.1f,\"frames\":%lu,"
           "\"size\":%d,\"quality\":%d,\"led\":%u,\"heap\":%u,\"psram\":%u}",
           WiFi.RSSI(), g_fps, (unsigned long)g_frames,
           s->status.framesize, s->status.quality, g_flash,
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
    "font-family:system-ui'><h3 style='padding:8px'>DJMS USV CAM (ESP32-CAM)</h3>"
    "<img src='/stream' style='width:100%;max-width:800px'>"
    "<p style='padding:8px'>"
    "<a style='color:#6af' href='/control?led=0'>탐조등 끄기</a> · "
    "<a style='color:#6af' href='/control?led=60'>약하게</a> · "
    "<a style='color:#6af' href='/control?led=255'>최대</a><br>"
    "/stream /capture /status /control</p></body>");
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
  } else {
    Serial.println("[CAM] HTTP 서버 시작 실패");
  }
}

/* ===================== 6. setup / loop ===================== */
void setup() {
#if DISABLE_BROWNOUT
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif

  Serial.begin(115200);
  delay(500);
  Serial.println("\n===== DJMS USV Camera (ESP32-CAM AI-Thinker) =====");

  pinMode(LED_STATUS, OUTPUT);
  digitalWrite(LED_STATUS, HIGH);       // 소등 (LOW 에서 점등)
  flashBegin();                         // 플래시 LED는 꺼진 상태로 시작

  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;   c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;   c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;   c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;   c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk  = XCLK_GPIO_NUM;  c.pin_pclk  = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href  = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn  = PWDN_GPIO_NUM;  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = XCLK_HZ;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode    = CAMERA_GRAB_LATEST;    // 항상 최신 프레임 (지연 최소)
  c.fb_location  = CAMERA_FB_IN_PSRAM;

  if (psramFound()) {
    c.frame_size   = START_SIZE;
    c.jpeg_quality = START_QUALITY;
    c.fb_count     = 2;
  } else {
    // PSRAM 이 없거나 인식되지 않으면 해상도를 낮춰야 합니다
    Serial.println("[CAM] 경고: PSRAM 미검출 - 저해상도로 동작합니다");
    Serial.println("        도구 > PSRAM 설정과 보드 선택을 확인하세요");
    c.frame_size   = FRAMESIZE_QVGA;
    c.jpeg_quality = 15;
    c.fb_count     = 1;
    c.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[CAM] 초기화 실패 0x%x\n", err);
    Serial.println("      ① 카메라 리본 케이블이 끝까지 꽂혔는지");
    Serial.println("      ② 5V 전원이 충분한지 (2A 이상)");
    Serial.println("      ③ XCLK_HZ 를 10000000 으로 낮춰 볼 것");
    delay(3000);
    ESP.restart();
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, 0);            // 카메라를 거꾸로 달았다면 1
  s->set_hmirror(s, 0);
  s->set_brightness(s, 1);       // 수면 반사 대응
  s->set_saturation(s, 0);
  s->set_gainceiling(s, GAINCEILING_2X);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                     // 지연 최소화 (중요)
  WiFi.setTxPower(WIFI_POWER_19_5dBm);      // 최대 송신 출력
  WiFi.config(LOCAL_IP, GATEWAY, SUBNET, GATEWAY);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] 접속 중");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300); Serial.print('.');
    digitalWrite(LED_STATUS, !digitalRead(LED_STATUS));
  }
  digitalWrite(LED_STATUS, HIGH);
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

/* =====================================================================
   부록 · ESP32-CAM 을 USV 에 쓸 때 꼭 알아야 할 것
   ---------------------------------------------------------------------
   [A] 외장 안테나로 바꾸면 통달거리가 늘어납니다  ★ 이 프로젝트의 핵심
       보드 뒷면 IPEX(U.FL) 커넥터 옆에 작은 패드 3개가 나란히 있고,
       가운데 패드가 0옴 저항(또는 납땜 점)으로 한쪽에 붙어 있습니다.
       기본값은 PCB 패턴 안테나 쪽입니다.
         · 인두로 그 연결을 떼어 반대쪽(IPEX 쪽)으로 옮기면 외장 안테나가 켜집니다.
         · 2.4GHz 3dBi 안테나를 달면 기본 PCB 안테나보다 유리합니다.
       ※ 반드시 안테나를 먼저 연결한 뒤 전원을 넣으세요.
          안테나 없이 송신하면 RF 출력단이 손상될 수 있습니다.

   [B] 전원이 가장 흔한 고장 원인입니다
       · 5V 2A 이상을 직접 공급하세요. UBEC #2 에서 받습니다.
       · 5V 와 GND 사이에 470uF 전해 커패시터를 달면 재부팅이 크게 줄어듭니다.
       · 3.3V 핀으로 급전하지 마세요. 부팅에 실패합니다.
       · USB-TTL 어댑터의 5V 만으로는 전류가 모자랄 수 있습니다.

   [C] 발열
       스트리밍 중 칩이 상당히 뜨거워집니다.
       방수 인클로저 안에 넣는다면 알루미늄 방열판을 붙이고,
       가능하면 인클로저 금속면에 닿게 배치하세요.

   [D] GPIO0 를 건드리지 마세요
       GPIO0 는 카메라 XCLK 이면서 부팅 모드 선택 핀입니다.
       운용 중에는 아무것도 연결하지 않아야 정상 부팅합니다.

   [E] 플래시 LED(GPIO4) 활용
       해질 무렵이나 흐린 날 배를 찾을 때 탐조등으로 씁니다.
       브라우저 주소창에 아래를 입력하면 됩니다.
           http://192.168.8.30/control?led=255   (최대)
           http://192.168.8.30/control?led=60    (약하게)
           http://192.168.8.30/control?led=0     (끄기)
       ※ 매우 밝습니다. 사람 눈을 향하지 마세요.
       ※ 켜 두면 전류 소모와 발열이 늘어납니다. 필요할 때만 켜세요.

   [F] XIAO ESP32S3 Sense 와 비교
       항목          ESP32-CAM AI-Thinker      XIAO ESP32S3 Sense
       ------------  ------------------------  ------------------------
       가격          저렴                      비쌈
       USB 단자      없음 (FTDI 필요)          있음 (바로 업로드)
       외장 안테나   가능 (납땜 개조 필요)     가능 (기본 제공)
       크기/무게     40x27mm / 약 20g          21x17mm / 약 5g
       PSRAM         4MB                       8MB
       발열          큼                        작음
       전원 민감도   매우 민감                 둔감
       추천          예산 우선 · 외장안테나    초보자 · 소형 경량
   ===================================================================== */
