#include <M5Unified.h>
#include <PubSubClient.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

namespace {

constexpr uint32_t SAMPLING_HZ = 100;
constexpr uint32_t SAMPLE_INTERVAL_MS = 1000 / SAMPLING_HZ;
constexpr size_t SAMPLES_PER_BATCH = 50;
// 50サンプルの最悪ケース（各値が最大桁数）は約4,595バイト。上限を4.5 KiBに置き、
// IoT CoreとFirehoseの5 KiB課金単位に1メッセージで収まるようにする。
constexpr size_t PAYLOAD_BUFFER_BYTES = 4800;
constexpr size_t PAYLOAD_LIMIT_BYTES = 4608;
constexpr char AWS_IOT_THING_NAME[] = "m5sticks3-01";
// 装着位置。手首などを追加するときはここを変える。
constexpr char PLACEMENT[] = "barbell";
constexpr uint32_t DASHBOARD_REFRESH_MS = 250;
// まぶしさを抑えた、オリーブ・砂・土色のスポーツ配色です。
constexpr uint16_t COLOR_BACKGROUND = 0x2684;  // 深いオリーブ
constexpr uint16_t COLOR_CARD = 0x3A46;        // カーキ
constexpr uint16_t COLOR_TEXT = 0xF739;        // 砂色
constexpr uint16_t COLOR_MUTED = 0xB551;       // 落ち着いたベージュ
constexpr uint16_t COLOR_AMBER = 0xE4A5;       // アンバー
constexpr uint16_t COLOR_GREEN = 0x950D;       // モスグリーン
constexpr uint16_t COLOR_RED = 0xD348;         // テラコッタ

WiFiClientSecure tlsClient;
PubSubClient mqttClient(tlsClient);

// 加速度はmg、角速度は0.1 dpsの整数で保持する。単位はschema_version 2の仕様。
struct ImuSample {
  uint32_t offsetMs;
  int16_t ax;
  int16_t ay;
  int16_t az;
  int16_t gx;
  int16_t gy;
  int16_t gz;
};

ImuSample samples[SAMPLES_PER_BATCH];
size_t sampleCount = 0;
uint64_t sequence = 0;
uint32_t setId = 0;
uint32_t batchStartedAtMs = 0;
uint64_t batchStartWallMs = 0;
uint32_t nextSampleAtMs = 0;
String sessionId;
String telemetryTopic;
String awsIoTEndpoint;
ImuSample latestSample {};
bool hasLatestSample = false;
uint32_t lastPayloadBytes = 0;
uint32_t lastDashboardAtMs = 0;
uint32_t lastSetIdOnScreen = UINT32_MAX;
bool dashboardInitialized = false;

// float値を固定小数の整数へ丸める。センサーが飽和しても範囲外にならないよう抑える。
int16_t toFixedPoint(float value, float scale) {
  const float scaled = roundf(value * scale);
  if (scaled >= 32767.0f) {
    return 32767;
  }
  if (scaled <= -32768.0f) {
    return -32768;
  }
  return static_cast<int16_t>(scaled);
}

// NTP同期済みの壁時計からUnix epochミリ秒を取る。
uint64_t wallClockMs() {
  struct timeval tv {};
  gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec) / 1000ULL;
}

void drawHeader(const char* label, uint32_t statusColor) {
  M5.Display.fillScreen(COLOR_BACKGROUND);
  M5.Display.fillRoundRect(8, 7, M5.Display.width() - 16, 27, COLOR_CARD);
  M5.Display.setTextColor(COLOR_TEXT, COLOR_CARD);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(17, 13);
  M5.Display.print(label);
  M5.Display.fillCircle(M5.Display.width() - 26, 20, 5, statusColor);
}

void showMessage(const char* firstLine, const char* secondLine = "", uint32_t color = COLOR_MUTED) {
  dashboardInitialized = false;
  drawHeader("TRAINING", color);
  M5.Display.fillRoundRect(8, 43, M5.Display.width() - 16, 82, COLOR_CARD);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(COLOR_TEXT, COLOR_CARD);
  M5.Display.setCursor(18, 59);
  M5.Display.println(firstLine);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COLOR_MUTED, COLOR_CARD);
  M5.Display.setCursor(19, 103);
  M5.Display.println(secondLine);
}

void drawDashboardShell() {
  M5.Display.fillScreen(COLOR_BACKGROUND);
  M5.Display.fillRoundRect(8, 7, M5.Display.width() - 16, 27, COLOR_CARD);
  M5.Display.fillRoundRect(8, 43, M5.Display.width() - 16, 46, COLOR_CARD);
  M5.Display.fillRoundRect(8, 97, M5.Display.width() - 16, 35, COLOR_CARD);

  M5.Display.setTextColor(COLOR_TEXT, COLOR_CARD);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(17, 13);
  M5.Display.print("TRAINING");
  M5.Display.fillRoundRect(17, 34, 38, 3, 2, COLOR_AMBER);
  M5.Display.setTextColor(COLOR_MUTED, COLOR_CARD);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(M5.Display.width() - 102, 15);
  M5.Display.print("BATT");

  M5.Display.setTextColor(COLOR_MUTED, COLOR_CARD);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(17, 51);
  M5.Display.print("Z ACCELERATION");
  M5.Display.setCursor(M5.Display.width() - 72, 51);
  M5.Display.print("SET");
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COLOR_AMBER, COLOR_CARD);
  M5.Display.setCursor(17, 99);
  M5.Display.print("X");
  M5.Display.setCursor(91, 99);
  M5.Display.print("Y");
  M5.Display.setCursor(165, 99);
  M5.Display.print("Z");
  dashboardInitialized = true;
}

void renderDashboard() {
  const bool isLive = mqttClient.connected();
  const int batteryLevel = M5.Power.getBatteryLevel();
  if (!dashboardInitialized) {
    drawDashboardShell();
    lastSetIdOnScreen = UINT32_MAX;
  }

  if (setId != lastSetIdOnScreen) {
    M5.Display.fillRect(M5.Display.width() - 72, 60, 56, 24, COLOR_CARD);
    M5.Display.setTextColor(COLOR_AMBER, COLOR_CARD);
    M5.Display.setTextSize(3);
    M5.Display.setCursor(M5.Display.width() - 72, 62);
    M5.Display.printf("%lu", static_cast<unsigned long>(setId));
    lastSetIdOnScreen = setId;
  }

  // カード自体は描き直さず、変化する値の領域だけ消して更新します。
  M5.Display.fillCircle(M5.Display.width() - 26, 20, 5, isLive ? COLOR_GREEN : COLOR_RED);
  M5.Display.fillRect(M5.Display.width() - 71, 12, 32, 12, COLOR_CARD);
  M5.Display.setTextColor(COLOR_MUTED, COLOR_CARD);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(M5.Display.width() - 71, 15);
  M5.Display.printf("%d%%", batteryLevel);
  // セット番号の表示に重ならないよう、加速度の値だけを消して描き直す。
  M5.Display.fillRect(17, 61, M5.Display.width() - 89, 28, COLOR_CARD);
  M5.Display.fillRect(17, 115, M5.Display.width() - 34, 16, COLOR_CARD);

  M5.Display.setTextColor(COLOR_TEXT, COLOR_CARD);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(17, 62);
  // 保持しているのはmgの整数だが、画面はgのほうが読みやすいので戻して表示する。
  if (hasLatestSample) {
    M5.Display.printf("%+.2f g", latestSample.az / 1000.0f);
  } else {
    M5.Display.print("-- g");
  }

  M5.Display.setTextColor(COLOR_TEXT, COLOR_CARD);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(17, 115);
  if (hasLatestSample) {
    M5.Display.printf("%+.2f", latestSample.ax / 1000.0f);
    M5.Display.setCursor(91, 115);
    M5.Display.printf("%+.2f", latestSample.ay / 1000.0f);
    M5.Display.setCursor(165, 115);
    M5.Display.printf("%+.2f", latestSample.az / 1000.0f);
  } else {
    M5.Display.print("--");
  }
}

void connectWiFi() {
  showMessage("Wi-Fi", "Connecting with saved settings", COLOR_AMBER);
  Serial.println("Wi-Fi connecting with saved settings");
  WiFi.mode(WIFI_STA);
  // Wi-Fi情報は端末のNVSに保存済みであることを前提にします。
  WiFi.begin();

  while (WiFi.status() != WL_CONNECTED) {
    Serial.printf("Wi-Fi status: %d\n", WiFi.status());
    delay(500);
  }

  Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
  showMessage("Wi-Fi connected", WiFi.localIP().toString().c_str(), COLOR_GREEN);
}

bool loadEndpoint() {
  File endpointFile = LittleFS.open("/aws-iot/endpoint.txt", FILE_READ);
  if (!endpointFile) {
    Serial.println("endpoint file not found");
    return false;
  }
  awsIoTEndpoint = endpointFile.readString();
  endpointFile.close();
  awsIoTEndpoint.trim();
  return !awsIoTEndpoint.isEmpty();
}

bool loadTlsFiles() {
  File caFile = LittleFS.open("/aws-iot/amazon-root-ca.pem", FILE_READ);
  if (!caFile || caFile.size() == 0 || !tlsClient.loadCACert(caFile, caFile.size())) {
    Serial.println("failed to load Amazon Root CA");
    return false;
  }
  caFile.close();

  File certificateFile = LittleFS.open("/aws-iot/device-certificate.pem", FILE_READ);
  if (!certificateFile || certificateFile.size() == 0 ||
      !tlsClient.loadCertificate(certificateFile, certificateFile.size())) {
    Serial.println("failed to load device certificate");
    return false;
  }
  certificateFile.close();

  File privateKeyFile = LittleFS.open("/aws-iot/private-key.pem", FILE_READ);
  if (!privateKeyFile || privateKeyFile.size() == 0 ||
      !tlsClient.loadPrivateKey(privateKeyFile, privateKeyFile.size())) {
    Serial.println("failed to load device private key");
    return false;
  }
  privateKeyFile.close();
  return true;
}

bool loadAwsIoTConfiguration() {
  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS mount failed");
    return false;
  }
  return loadEndpoint() && loadTlsFiles();
}

void synchronizeTime() {
  // TLS サーバー証明書を検証するため、AWS IoT Core 接続前に時刻を合わせます。
  configTime(9 * 60 * 60, 0, "ntp.nict.jp", "pool.ntp.org");
  while (time(nullptr) < 1700000000) {
    delay(500);
  }
}

String iso8601Now() {
  time_t now = time(nullptr);
  struct tm localTime {};
  localtime_r(&now, &localTime);
  char value[32];
  strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%S+09:00", &localTime);
  return String(value);
}

void connectMqtt() {
  while (!mqttClient.connected()) {
    showMessage("AWS IoT", "Connecting...", COLOR_AMBER);
    Serial.println("AWS IoT connecting");
    if (mqttClient.connect(AWS_IOT_THING_NAME)) {
      Serial.println("AWS IoT connected");
      showMessage("AWS IoT connected", AWS_IOT_THING_NAME, COLOR_GREEN);
      return;
    }
    Serial.printf("MQTT connect failed: %d\n", mqttClient.state());
    char errorMessage[32];
    snprintf(errorMessage, sizeof(errorMessage), "Retrying (code %d)", mqttClient.state());
    showMessage("AWS IoT retry", errorMessage, COLOR_RED);
    delay(3000);
  }
}

bool appendSampleJson(char* payload, size_t capacity, size_t* used, const ImuSample& sample, bool prependComma) {
  const int written = snprintf(
      payload + *used,
      capacity - *used,
      "%s{\"offset_ms\":%lu,\"ax\":%d,\"ay\":%d,\"az\":%d,\"gx\":%d,\"gy\":%d,\"gz\":%d}",
      prependComma ? "," : "",
      static_cast<unsigned long>(sample.offsetMs),
      sample.ax,
      sample.ay,
      sample.az,
      sample.gx,
      sample.gy,
      sample.gz);
  if (written < 0 || static_cast<size_t>(written) >= capacity - *used) {
    return false;
  }
  *used += static_cast<size_t>(written);
  return true;
}

void publishBatch() {
  if (sampleCount == 0) {
    return;
  }

  // 4.8 KiBの配列をスタックに置かないよう静的にする。loop以外から呼ばれることはない。
  static char payload[PAYLOAD_BUFFER_BYTES];
  size_t used = 0;
  const String capturedAt = iso8601Now();
  const int headerSize = snprintf(
      payload,
      sizeof(payload),
      "{\"schema_version\":2,\"device_id\":\"%s\",\"session_id\":\"%s\",\"set_id\":%lu,"
      "\"placement\":\"%s\",\"captured_at\":\"%s\",\"batch_start_ms\":%llu,"
      "\"sequence\":%llu,\"sampling_hz\":%lu,\"samples\":[",
      AWS_IOT_THING_NAME,
      sessionId.c_str(),
      static_cast<unsigned long>(setId),
      PLACEMENT,
      capturedAt.c_str(),
      static_cast<unsigned long long>(batchStartWallMs),
      static_cast<unsigned long long>(sequence),
      static_cast<unsigned long>(SAMPLING_HZ));
  if (headerSize < 0 || static_cast<size_t>(headerSize) >= sizeof(payload)) {
    showMessage("Payload too large", "Header", COLOR_RED);
    sampleCount = 0;
    return;
  }
  used = static_cast<size_t>(headerSize);

  // 実測のサンプル間隔。10 ms付近に収まっているかをシリアルで確認するために出す。
  uint32_t deltaMin = UINT32_MAX;
  uint32_t deltaMax = 0;
  for (size_t index = 1; index < sampleCount; index++) {
    const uint32_t delta = samples[index].offsetMs - samples[index - 1].offsetMs;
    if (delta < deltaMin) {
      deltaMin = delta;
    }
    if (delta > deltaMax) {
      deltaMax = delta;
    }
  }
  if (sampleCount < 2) {
    deltaMin = 0;
  }

  for (size_t index = 0; index < sampleCount; index++) {
    if (!appendSampleJson(payload, sizeof(payload), &used, samples[index], index > 0)) {
      showMessage("Payload too large", "Samples", COLOR_RED);
      sampleCount = 0;
      return;
    }
  }

  const int footerSize = snprintf(payload + used, sizeof(payload) - used, "]}");
  if (footerSize < 0 || static_cast<size_t>(footerSize) >= sizeof(payload) - used) {
    showMessage("Payload too large", "Footer", COLOR_RED);
    sampleCount = 0;
    return;
  }
  used += static_cast<size_t>(footerSize);

  if (used > PAYLOAD_LIMIT_BYTES) {
    showMessage("Payload too large", String(used).c_str(), COLOR_RED);
    sampleCount = 0;
    return;
  }

  if (mqttClient.publish(telemetryTopic.c_str(), reinterpret_cast<const uint8_t*>(payload), used, false)) {
    Serial.printf(
        "Telemetry sent: sequence=%llu set=%lu bytes=%u samples=%u dt_min=%lu dt_max=%lu\n",
        static_cast<unsigned long long>(sequence),
        static_cast<unsigned long>(setId),
        static_cast<unsigned int>(used),
        static_cast<unsigned int>(sampleCount),
        static_cast<unsigned long>(deltaMin),
        static_cast<unsigned long>(deltaMax));
    sequence++;
    lastPayloadBytes = used;
  } else {
    showMessage("Publish failed", "Will retry", COLOR_RED);
  }
  sampleCount = 0;
}

// 呼び出し側が M5.Imu.update() で新しいデータを確認済みであることを前提にする。
void collectSample(uint32_t now) {
  if (sampleCount == 0) {
    batchStartedAtMs = now;
    batchStartWallMs = wallClockMs();
  }

  m5::imu_data_t imu {};
  M5.Imu.getImuData(&imu);
  samples[sampleCount] = {
      // offset_msは予定時刻ではなく実測値。間隔の乱れをそのまま残す。
      .offsetMs = now - batchStartedAtMs,
      .ax = toFixedPoint(imu.accel.x, 1000.0f),
      .ay = toFixedPoint(imu.accel.y, 1000.0f),
      .az = toFixedPoint(imu.accel.z, 1000.0f),
      .gx = toFixedPoint(imu.gyro.x, 10.0f),
      .gy = toFixedPoint(imu.gyro.y, 10.0f),
      .gz = toFixedPoint(imu.gyro.z, 10.0f),
  };
  latestSample = samples[sampleCount];
  hasLatestSample = true;
  sampleCount++;
  if (sampleCount >= SAMPLES_PER_BATCH) {
    publishBatch();
  }
}

}  // namespace

void setup() {
  auto config = M5.config();
  M5.begin(config);
  M5.Display.setRotation(1);
  M5.Display.setTextWrap(false);
  Serial.begin(115200);

  connectWiFi();
  synchronizeTime();

  if (!loadAwsIoTConfiguration()) {
    showMessage("AWS files missing", "Upload LittleFS data", COLOR_RED);
    while (true) {
      delay(1000);
    }
  }

  mqttClient.setServer(awsIoTEndpoint.c_str(), 8883);
  // トピック名とMQTTヘッダーの分を上乗せする。
  mqttClient.setBufferSize(PAYLOAD_BUFFER_BYTES + 256);

  telemetryTopic = String("$aws/rules/training_iot_basic_ingest/training/") + AWS_IOT_THING_NAME + "/telemetry";
  sessionId = iso8601Now();
  nextSampleAtMs = millis();
  renderDashboard();
}

void loop() {
  M5.update();
  // ボタンAでセット番号を進める。0はセット外（ラック上、休憩）を表す。
  if (M5.BtnA.wasPressed()) {
    setId++;
  }
  if (!mqttClient.connected()) {
    connectMqtt();
  }
  mqttClient.loop();

  const uint32_t now = millis();
  // 予定時刻を積み上げて、millis()差分方式のドリフトを避ける。
  if (static_cast<int32_t>(now - nextSampleAtMs) >= 0) {
    // IMUに新しい値が来たときだけ取る。同じ値を重複して送らないため。
    if (M5.Imu.update()) {
      collectSample(now);
      nextSampleAtMs += SAMPLE_INTERVAL_MS;
      // collectSampleは送信でブロックし得るので、判定にはnowではなく現在時刻を読み直す。
      const uint32_t afterCollect = millis();
      if (static_cast<int32_t>(afterCollect - nextSampleAtMs) >= 0) {
        // 予定を追い越した場合は、まとめ取りせず間隔を取り直す。
        nextSampleAtMs = afterCollect + SAMPLE_INTERVAL_MS;
      }
    }
  }
  if (now - lastDashboardAtMs >= DASHBOARD_REFRESH_MS) {
    lastDashboardAtMs = now;
    renderDashboard();
  }
}
