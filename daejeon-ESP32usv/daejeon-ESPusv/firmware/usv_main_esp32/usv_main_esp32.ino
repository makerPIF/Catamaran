/* =====================================================================
   대전중앙중학교 USV (쌍동선 무인정) - 메인 비행/항행 컨트롤러
   Board : ESP32 NodeMCU (ESP32-WROOM-32 DevKit V1)
   Sensor: MPU6050 (I2C) + u-blox NEO-8M (UART2)
   Motor : BLDC A2212 1400KV x2 + BLHeli_S ESC(3D/양방향 모드) x2
   RC    : FlySky FS-iA6B  iBUS (안전 스위치 / 비상 수동조종)
   Link  : 미니 공유기(SSID: 대전중앙중학교) -> WebSocket 텔레메트리 & 조종

   빌드 준비 (Arduino IDE)
   -----------------------------------------------------------------
   1) 보드 매니저 URL:
      https://espressif.github.io/arduino-esp32/package_esp32_index.json
      -> "esp32 by Espressif Systems" 설치, 보드: "ESP32 Dev Module"
   2) 라이브러리 매니저에서 설치
        - ArduinoJson              (Benoit Blanchon)
        - Adafruit MPU6050         (+ Adafruit Unified Sensor, Adafruit BusIO)
        - TinyGPSPlus              (Mikal Hart)
        - ESP32Servo               (Kevin Harrington)
        - IBusBM                   (Bart Mellink)
   3) ZIP으로 설치 (라이브러리 매니저에 없음)
        - https://github.com/me-no-dev/ESPAsyncWebServer
        - https://github.com/me-no-dev/AsyncTCP
   4) 도구 > Partition Scheme > "Default 4MB with spiffs"
      도구 > Flash Size > 4MB

   ※ ESC는 반드시 BLHeliSuite32/BLHeliSuite에서 "Bidirectional(3D)" 를
      ON 으로 바꿔야 후진이 됩니다. 자세한 절차는 docs/HARDWARE.md 참조.
   ===================================================================== */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPSPlus.h>
#include <ESP32Servo.h>
#include <IBusBM.h>

/* ===================== 1. 사용자 설정 ===================== */
// 미니 공유기 정보
static const char* WIFI_SSID = "대전중앙중학교";
static const char* WIFI_PASS = "2026USV";

// 고정 IP (공유기 LAN 대역에 맞춰 수정하세요)
//  GL.iNet 계열 기본 대역 : 192.168.8.x   (게이트웨이 192.168.8.1)
//  TP-Link 계열 기본 대역 : 192.168.0.x   (게이트웨이 192.168.0.1)
IPAddress LOCAL_IP (192, 168, 8, 20);   // 이 보드(USV 본체)
IPAddress GATEWAY  (192, 168, 8, 1);    // 공유기
IPAddress SUBNET   (255, 255, 255, 0);
IPAddress DNS1     (192, 168, 8, 1);

// 배터리 (3S LiPo 기준)
static const float VBAT_DIV_RATIO = 5.545f;  // (100k + 22k) / 22k
static const float VBAT_CAL       = 1.000f;  // 실측/표시값 으로 보정
static const float VBAT_WARN      = 10.8f;   // 3.60V/셀 : 경고
static const float VBAT_CRIT      = 10.2f;   // 3.40V/셀 : 즉시 복귀

// 조종 특성
static const float STEER_GAIN     = 0.65f;   // 회전 민감도 (0.4~0.9)
static const float THR_SLEW       = 2.0f;    // 초당 스로틀 변화 한계 (급가속 방지)
static const float DEADBAND       = 0.05f;   // 스틱/키 중립 불감대
static const float THR_LIMIT      = 0.85f;   // 최대 출력 제한 (모터 보호)

// 페일세이프
static const uint32_t GCS_TIMEOUT_MS  = 1500; // GCS 명령 끊김 판정
static const uint32_t RC_TIMEOUT_MS   = 1000; // RC 신호 끊김 판정
static const uint32_t TELEM_PERIOD_MS = 100;  // 10 Hz 텔레메트리
static const uint32_t FS_HOLD_MS      = 5000; // 경보 사유를 화면에 남겨 두는 시간

/* ===================== 2. 핀 배치 ===================== */
#define PIN_ESC_L     25   // 좌현 ESC 신호
#define PIN_ESC_R     26   // 우현 ESC 신호
#define PIN_SDA       21   // MPU6050
#define PIN_SCL       22
#define PIN_GPS_RX    16   // ESP32 RX2  <- NEO-8M TX
#define PIN_GPS_TX    17   // ESP32 TX2  -> NEO-8M RX
#define PIN_IBUS_RX    4   // ESP32 RX1  <- FS-iA6B iBUS(서보) 단자
#define PIN_IBUS_TX   13   // 사용 안 함(더미)
#define PIN_VBAT      34   // ADC1_CH6 (입력 전용 핀)
#define PIN_BUZZER    27
#define PIN_LED       2    // 온보드 LED

/* ===================== 3. ESC 펄스 정의 (3D 모드) ===================== */
static const int PULSE_NEUTRAL = 1500;  // 정지
static const int PULSE_FWD_MAX = 2000;  // 최대 전진
static const int PULSE_REV_MAX = 1000;  // 최대 후진
static const int PULSE_DEAD    = 30;    // 중립 부근 무출력 폭(us)

/* ===================== 4. 전역 객체 ===================== */
AsyncWebServer  server(80);
AsyncWebSocket  ws("/ws");
Adafruit_MPU6050 mpu;
TinyGPSPlus      gps;
HardwareSerial   GpsSerial(2);
HardwareSerial   IbusSerial(1);
IBusBM           ibus;
Servo escL, escR;

/* ===================== 5. 상태 변수 ===================== */
struct Attitude { float roll = 0, pitch = 0, yaw = 0; };
Attitude att;

float ax = 0, ay = 0, az = 0;     // g
float gx = 0, gy = 0, gz = 0;     // deg/s
float tempC = 0;
float gyroBiasX = 0, gyroBiasY = 0, gyroBiasZ = 0;

double lat = 0, lon = 0;
float  gpsSpeed = 0, gpsCourse = 0, gpsAlt = 0, hdop = 99;
uint8_t sats = 0;
bool   gpsFix = false;

float  vbat = 0;
int    rssi = 0;

float  cmdThr = 0, cmdStr = 0;    // GCS 명령 (-1..1)
float  outThr = 0, outStr = 0;    // 슬루 적용된 실제 출력
int    pulseL = PULSE_NEUTRAL, pulseR = PULSE_NEUTRAL;

bool   armed = false;
bool   mpuOk = false;
bool   rcOk  = false;
enum CtrlMode { MODE_RC, MODE_GCS };
CtrlMode mode = MODE_GCS;

const char* failsafeReason = "";
char     fsLatched[20] = "";      // 한 번 걸린 경보 사유를 잠시 유지
uint32_t fsLatchUntil = 0;

uint32_t seq = 0;
uint32_t lastGcsCmd = 0, lastRcFrame = 0;
uint32_t tTelem = 0, tImu = 0, tSlow = 0;

/* ===================== 6. 유틸 ===================== */
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float applyDeadband(float v) {
  if (fabs(v) < DEADBAND) return 0;
  return (v > 0) ? (v - DEADBAND) / (1 - DEADBAND)
                 : (v + DEADBAND) / (1 - DEADBAND);
}

void beep(uint16_t ms, uint8_t times = 1) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delay(ms);
    digitalWrite(PIN_BUZZER, LOW);  if (i < times - 1) delay(ms);
  }
}

/* 치명적 경보가 걸리면 시동을 끄고, 사유를 5초간 화면에 남깁니다.
   (멀리서 조종하던 학생이 "왜 멈췄는지"를 놓치지 않도록)            */
void tripFailsafe(const char* code) {
  if (strcmp(fsLatched, code) == 0 && millis() < fsLatchUntil) return;
  armed = false;
  cmdThr = cmdStr = 0; outThr = outStr = 0;
  strncpy(fsLatched, code, sizeof(fsLatched) - 1);
  fsLatchUntil = millis() + FS_HOLD_MS;
  beep(250, 3);
  Serial.printf("[FAILSAFE] %s\n", code);
}

/* ===================== 7. ESC ===================== */
void escWrite(int l, int r) {
  pulseL = constrain(l, PULSE_REV_MAX, PULSE_FWD_MAX);
  pulseR = constrain(r, PULSE_REV_MAX, PULSE_FWD_MAX);
  escL.writeMicroseconds(pulseL);
  escR.writeMicroseconds(pulseR);
}
void escNeutral() { escWrite(PULSE_NEUTRAL, PULSE_NEUTRAL); }

// -1..1 값을 3D 모드 펄스로 변환 (중립 부근에는 데드밴드를 둠)
int mixToPulse(float v) {
  v = clampf(v, -1.0f, 1.0f);
  if (fabs(v) < 0.02f) return PULSE_NEUTRAL;
  if (v > 0) return PULSE_NEUTRAL + PULSE_DEAD + (int)(v * (PULSE_FWD_MAX - PULSE_NEUTRAL - PULSE_DEAD));
  else       return PULSE_NEUTRAL - PULSE_DEAD + (int)(v * (PULSE_NEUTRAL - PULSE_REV_MAX - PULSE_DEAD));
}

/* ESC 초기화(아밍) : 3D 모드 ESC 는 중립 신호를 일정 시간 받아야 깨어남 */
void escArmSequence() {
  escL.setPeriodHertz(50);
  escR.setPeriodHertz(50);
  escL.attach(PIN_ESC_L, PULSE_REV_MAX, PULSE_FWD_MAX);
  escR.attach(PIN_ESC_R, PULSE_REV_MAX, PULSE_FWD_MAX);
  escNeutral();
  Serial.println(F("[ESC] 중립 신호 3초 출력 (ESC 기동음 대기)..."));
  delay(3000);
}

/* ===================== 8. 센서 ===================== */
void imuCalibrate() {
  Serial.println(F("[IMU] 자이로 바이어스 보정 - 배를 움직이지 마세요 (2초)"));
  const int N = 400;
  double sx = 0, sy = 0, sz = 0;
  sensors_event_t a, g, t;
  for (int i = 0; i < N; i++) {
    mpu.getEvent(&a, &g, &t);
    sx += g.gyro.x; sy += g.gyro.y; sz += g.gyro.z;
    delay(5);
  }
  gyroBiasX = sx / N; gyroBiasY = sy / N; gyroBiasZ = sz / N;
  Serial.println(F("[IMU] 보정 완료"));
}

void imuUpdate(float dt) {
  if (!mpuOk) return;
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  ax = a.acceleration.x / 9.80665f;
  ay = a.acceleration.y / 9.80665f;
  az = a.acceleration.z / 9.80665f;
  gx = (g.gyro.x - gyroBiasX) * 57.2957795f;
  gy = (g.gyro.y - gyroBiasY) * 57.2957795f;
  gz = (g.gyro.z - gyroBiasZ) * 57.2957795f;
  tempC = t.temperature;

  // 가속도계 기반 절대 각도
  float accRoll  = atan2f(ay, sqrtf(ax * ax + az * az)) * 57.2957795f;
  float accPitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2957795f;

  // 상보 필터 (파도 충격에 강하도록 자이로 비중 98%)
  const float K = 0.98f;
  att.roll  = K * (att.roll  + gx * dt) + (1 - K) * accRoll;
  att.pitch = K * (att.pitch + gy * dt) + (1 - K) * accPitch;

  // 요는 자이로 적분 + GPS 진행방위로 보정 (지자기 센서 미탑재)
  att.yaw += gz * dt;
  if (gpsFix && gpsSpeed > 0.6f) {           // 0.6 m/s 이상 이동 중일 때만 신뢰
    float err = gpsCourse - att.yaw;
    while (err >  180) err -= 360;
    while (err < -180) err += 360;
    att.yaw += 0.02f * err;
  }
  while (att.yaw >= 360) att.yaw -= 360;
  while (att.yaw <    0) att.yaw += 360;
}

void gpsUpdate() {
  while (GpsSerial.available()) gps.encode(GpsSerial.read());
  if (gps.location.isValid()) { lat = gps.location.lat(); lon = gps.location.lng(); }
  if (gps.speed.isValid())    gpsSpeed  = gps.speed.mps();
  if (gps.course.isValid())   gpsCourse = gps.course.deg();
  if (gps.altitude.isValid()) gpsAlt    = gps.altitude.meters();
  if (gps.hdop.isValid())     hdop      = gps.hdop.hdop();
  if (gps.satellites.isValid()) sats    = gps.satellites.value();
  gpsFix = gps.location.isValid() && sats >= 4 && gps.location.age() < 3000;
}

void vbatUpdate() {
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(PIN_VBAT);
  float mv = acc / 16.0f;
  float v  = (mv / 1000.0f) * VBAT_DIV_RATIO * VBAT_CAL;
  vbat = (vbat == 0) ? v : (vbat * 0.9f + v * 0.1f);   // 저역통과
}

/* ===================== 9. RC (iBUS) ===================== */
// FS-i6 기본 채널 배치
//   CH1 : 에일러론 (좌우 조향)
//   CH2 : 엘리베이터 (전후 스로틀)
//   CH5 (SwA/VrA) : 조종권 - 1000=GCS 자동, 2000=RC 수동
//   CH6 (SwB)     : 아밍 - 1000=해제(정지), 2000=아밍
int rcCh[10] = {0};

void rcUpdate() {
  int c1 = ibus.readChannel(0);
  if (c1 > 800) {                       // 유효 프레임 수신
    lastRcFrame = millis();
    for (int i = 0; i < 10; i++) rcCh[i] = ibus.readChannel(i);
  }
  rcOk = (millis() - lastRcFrame) < RC_TIMEOUT_MS;
}
static inline float rcNorm(int us) { return clampf((us - 1500) / 500.0f, -1.f, 1.f); }

/* ===================== 10. 제어 루프 ===================== */
void controlUpdate(float dt) {
  float tThr = 0, tStr = 0;
  failsafeReason = "";

  // --- 조종권 결정 ---
  bool rcWantsManual = rcOk && rcCh[4] > 1600;
  mode = rcWantsManual ? MODE_RC : MODE_GCS;

  // --- 아밍 판단 ---
  bool rcArm = rcOk ? (rcCh[5] > 1600) : true;   // RC 없으면 RC 조건은 무시
  bool gcsAlive = (millis() - lastGcsCmd) < GCS_TIMEOUT_MS;

  if (!rcArm) { armed = false; failsafeReason = "RC_DISARM"; }

  // --- 목표값 산출 ---
  if (mode == MODE_RC) {
    tStr = applyDeadband(rcNorm(rcCh[0]));
    tThr = applyDeadband(rcNorm(rcCh[1]));
  } else {
    if (gcsAlive) {
      tThr = cmdThr; tStr = cmdStr;
    } else {
      tThr = 0; tStr = 0;
      if (armed) failsafeReason = "GCS_LINK_LOST";
    }
  }

  // --- 치명적 안전 조건 ---
  if (vbat > 5 && vbat < VBAT_CRIT) {           // 5V 미만은 센서 미연결로 간주
    tripFailsafe("BATTERY_CRITICAL");
  }
  if (mpuOk && fabs(att.roll) > 70) {           // 전복 감지
    tripFailsafe("CAPSIZED");
  }
  if (millis() < fsLatchUntil) {                // 경보 유지 구간
    failsafeReason = fsLatched;
    tThr = 0; tStr = 0;
  }
  if (!armed) { tThr = 0; tStr = 0; }

  // --- 슬루 레이트 (급격한 출력 변화 억제) ---
  float maxStep = THR_SLEW * dt;
  outThr += clampf(tThr - outThr, -maxStep, maxStep);
  outStr += clampf(tStr - outStr, -maxStep * 2, maxStep * 2);

  // --- 차동 믹싱 ---
  //  전진/후진 : 양쪽 동일 방향
  //  제자리 회전 : 스로틀 0 에서 좌우 반대 방향 (3D ESC 라 가능)
  float l = outThr + outStr * STEER_GAIN;
  float r = outThr - outStr * STEER_GAIN;

  // 포화 시 비율 유지
  float peak = max(fabs(l), fabs(r));
  if (peak > 1.0f) { l /= peak; r /= peak; }

  l = clampf(l, -THR_LIMIT, THR_LIMIT);
  r = clampf(r, -THR_LIMIT, THR_LIMIT);

  escWrite(mixToPulse(l), mixToPulse(r));
}

/* ===================== 11. 텔레메트리 ===================== */
void sendTelemetry() {
  if (ws.count() == 0) return;

  StaticJsonDocument<640> d;
  d["seq"]  = seq++;
  d["t"]    = millis();
  d["roll"] = roundf(att.roll  * 10) / 10.0f;
  d["pitch"]= roundf(att.pitch * 10) / 10.0f;
  d["yaw"]  = roundf(att.yaw   * 10) / 10.0f;
  d["ax"]   = roundf(ax * 100) / 100.0f;
  d["ay"]   = roundf(ay * 100) / 100.0f;
  d["az"]   = roundf(az * 100) / 100.0f;
  d["gz"]   = roundf(gz * 10) / 10.0f;
  d["tmp"]  = roundf(tempC * 10) / 10.0f;
  d["lat"]  = lat;
  d["lon"]  = lon;
  d["spd"]  = roundf(gpsSpeed * 100) / 100.0f;
  d["cog"]  = roundf(gpsCourse * 10) / 10.0f;
  d["alt"]  = roundf(gpsAlt * 10) / 10.0f;
  d["sat"]  = sats;
  d["hdop"] = roundf(hdop * 100) / 100.0f;
  d["fix"]  = gpsFix ? 1 : 0;
  d["vbat"] = roundf(vbat * 100) / 100.0f;
  d["rssi"] = rssi;
  d["arm"]  = armed ? 1 : 0;
  d["mode"] = (mode == MODE_RC) ? "RC" : "GCS";
  d["rc"]   = rcOk ? 1 : 0;
  d["thr"]  = roundf(outThr * 100) / 100.0f;
  d["str"]  = roundf(outStr * 100) / 100.0f;
  d["pl"]   = pulseL;
  d["pr"]   = pulseR;
  d["fs"]   = failsafeReason;

  char buf[700];
  size_t n = serializeJson(d, buf, sizeof(buf));
  ws.textAll(buf, n);
}

/* ===================== 12. WebSocket 수신 ===================== */
void handleCommand(AsyncWebSocketClient* client, const char* payload, size_t len) {
  StaticJsonDocument<256> d;
  if (deserializeJson(d, payload, len)) return;
  const char* c = d["c"] | "";

  if (!strcmp(c, "drive")) {
    cmdThr = applyDeadband(clampf(d["thr"] | 0.0f, -1.f, 1.f));
    cmdStr = applyDeadband(clampf(d["str"] | 0.0f, -1.f, 1.f));
    lastGcsCmd = millis();
  }
  else if (!strcmp(c, "arm")) {
    bool want = (d["v"] | 0) != 0;
    // RC 가 살아있는데 RC 쪽 아밍 스위치가 내려가 있으면 아밍 거부
    if (want && rcOk && rcCh[5] < 1600) {
      client->text("{\"c\":\"err\",\"m\":\"RC 아밍 스위치(CH6)를 먼저 올리세요\"}");
    } else {
      armed = want;
      cmdThr = cmdStr = 0; outThr = outStr = 0;
      beep(80, armed ? 2 : 1);
    }
    lastGcsCmd = millis();
  }
  else if (!strcmp(c, "stop")) {
    armed = false; cmdThr = cmdStr = 0; outThr = outStr = 0;
    escNeutral(); beep(300, 1);
    lastGcsCmd = millis();
  }
  else if (!strcmp(c, "ping")) {
    // 왕복 지연 측정용 즉시 에코
    char out[64];
    snprintf(out, sizeof(out), "{\"c\":\"pong\",\"id\":%ld}", (long)(d["id"] | 0));
    client->text(out);
    lastGcsCmd = millis();
  }
  else if (!strcmp(c, "beep")) {
    beep(120, 3);
  }
}

void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("[WS] 접속 #%u %s\n", client->id(), client->remoteIP().toString().c_str());
      lastGcsCmd = millis();
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("[WS] 해제 #%u\n", client->id());
      cmdThr = cmdStr = 0;
      break;
    case WS_EVT_DATA: {
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        data[len] = 0;
        handleCommand(client, (const char*)data, len);
      }
      break;
    }
    default: break;
  }
}

/* ===================== 13. setup / loop ===================== */
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n===== 대전중앙중 USV 컨트롤러 ====="));

  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VBAT, ADC_11db);

  // --- ESC 먼저 (부팅 순간 모터가 돌지 않도록) ---
  escArmSequence();

  // --- IMU ---
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  if (mpu.begin(0x68, &Wire)) {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);   // 파도/모터 진동 억제
    mpuOk = true;
    imuCalibrate();
  } else {
    Serial.println(F("[IMU] MPU6050 미검출! SDA/SCL 배선을 확인하세요."));
  }

  // --- GPS ---
  GpsSerial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  // --- RC (iBUS) ---
  IbusSerial.begin(115200, SERIAL_8N1, PIN_IBUS_RX, PIN_IBUS_TX);
  ibus.begin(IbusSerial, IBUSBM_NOTIMER);

  // --- Wi-Fi ---
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                       // 지연 최소화 (중요)
  WiFi.setTxPower(WIFI_POWER_19_5dBm);        // 최대 송신 출력
  WiFi.config(LOCAL_IP, GATEWAY, SUBNET, DNS1);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("[WiFi] 접속 중"));
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300); Serial.print('.');
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] 연결됨  IP=%s  RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    digitalWrite(PIN_LED, HIGH);
    beep(80, 2);
  } else {
    Serial.println(F("\n[WiFi] 연결 실패 - RC 수동 조종만 가능합니다."));
    beep(400, 3);
  }

  // --- 웹서버 / WebSocket ---
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/health", HTTP_GET, [](AsyncWebServerRequest* r) {
    char b[160];
    snprintf(b, sizeof(b), "{\"ok\":1,\"rssi\":%d,\"up\":%lu,\"heap\":%u}",
             WiFi.RSSI(), millis() / 1000, ESP.getFreeHeap());
    r->send(200, "application/json", b);
  });

  // GCS(index.html) 를 보드에서도 서빙하고 싶으면 LittleFS 에 올린 뒤 아래 주석 해제
  // #include <LittleFS.h>  ... LittleFS.begin();
  // server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest* r) {
    r->send(200, "text/html; charset=utf-8",
            "<meta charset='utf-8'><h2>DJMS USV Controller</h2>"
            "<p>GCS(index.html)를 PC에서 열고 이 보드의 IP로 접속하세요.</p>"
            "<p>WebSocket: <code>/ws</code></p>");
  });
  server.begin();

  Serial.println(F("[SYS] 준비 완료. 아밍 전까지 모터는 정지 상태입니다."));
  tImu = millis();
}

void loop() {
  uint32_t now = millis();

  ibus.loop();                 // IBUSBM_NOTIMER 모드에서는 직접 호출 필요
  rcUpdate();
  gpsUpdate();

  // 200 Hz IMU + 제어
  if (now - tImu >= 5) {
    float dt = (now - tImu) / 1000.0f;
    tImu = now;
    imuUpdate(dt);
    controlUpdate(dt);
  }

  // 4 Hz 저속 항목
  if (now - tSlow >= 250) {
    tSlow = now;
    vbatUpdate();
    rssi = WiFi.isConnected() ? WiFi.RSSI() : -127;
    // 상태 LED : 아밍=점등, 대기=느린 깜빡임, 링크단절=빠른 깜빡임
    if (armed)                    digitalWrite(PIN_LED, HIGH);
    else if (WiFi.isConnected())  digitalWrite(PIN_LED, (now / 800) % 2);
    else                          digitalWrite(PIN_LED, (now / 150) % 2);
    // 저전압 경고음
    if (vbat > 5 && vbat < VBAT_WARN) { static uint32_t tb = 0;
      if (now - tb > 4000) { tb = now; beep(60, 2); } }
  }

  // 10 Hz 텔레메트리
  if (now - tTelem >= TELEM_PERIOD_MS) {
    tTelem = now;
    sendTelemetry();
    ws.cleanupClients();
  }
}
