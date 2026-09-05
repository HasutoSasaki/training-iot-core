#include <M5Unified.h>
#include <PubSubClient.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

namespace {

constexpr uint32_t SAMPLING_HZ = 10;
constexpr uint32_t SAMPLE_INTERVAL_MS = 1000 / SAMPLING_HZ;
constexpr size_t SAMPLES_PER_BATCH = 10;
constexpr size_t MAX_PAYLOAD_BYTES = 4600;
constexpr char AWS_IOT_THING_NAME[] = "m5sticks3-01";
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

struct ImuSample {
  uint32_t offsetMs;
  float ax;
  float ay;
  float az;
  float gx;
  float gy;
  float gz;
};

ImuSample samples[SAMPLES_PER_BATCH];
size_t sampleCount = 0;
uint64_t sequence = 0;
uint32_t batchStartedAtMs = 0;
uint32_t lastSampleAtMs = 0;
String sessionId;
String telemetryTopic;
String awsIoTEndpoint;
ImuSample latestSample {};
bool hasLatestSample = false;
uint32_t lastPayloadBytes = 0;
uint32_t lastDashboardAtMs = 0;
bool dashboardInitialized = false;

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
  }

  // カード自体は描き直さず、変化する値の領域だけ消して更新します。
  M5.Display.fillCircle(M5.Display.width() - 26, 20, 5, isLive ? COLOR_GREEN : COLOR_RED);
  M5.Display.fillRect(M5.Display.width() - 71, 12, 32, 12, COLOR_CARD);
  M5.Display.setTextColor(COLOR_MUTED, COLOR_CARD);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(M5.Display.width() - 71, 15);
  M5.Display.printf("%d%%", batteryLevel);
  M5.Display.fillRect(17, 61, M5.Display.width() - 34, 28, COLOR_CARD);
  M5.Display.fillRect(17, 115, M5.Display.width() - 34, 16, COLOR_CARD);

  M5.Display.setTextColor(COLOR_TEXT, COLOR_CARD);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(17, 62);
  if (hasLatestSample) {
    M5.Display.printf("%+.2f g", latestSample.az);
  } else {
    M5.Display.print("-- g");
  }

  M5.Display.setTextColor(COLOR_TEXT, COLOR_CARD);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(17, 115);
  if (hasLatestSample) {
    M5.Display.printf("%+.2f", latestSample.ax);
    M5.Display.setCursor(91, 115);
    M5.Display.printf("%+.2f", latestSample.ay);
    M5.Display.setCursor(165, 115);
    M5.Display.printf("%+.2f", latestSample.az);
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
      "%s{\"offset_ms\":%lu,\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,\"gx\":%.4f,\"gy\":%.4f,\"gz\":%.4f}",
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

  char payload[MAX_PAYLOAD_BYTES] = {};
  size_t used = 0;
  const String capturedAt = iso8601Now();
  const int headerSize = snprintf(
      payload,
      sizeof(payload),
      "{\"schema_version\":1,\"device_id\":\"%s\",\"session_id\":\"%s\",\"captured_at\":\"%s\",\"sequence\":%llu,\"sampling_hz\":%lu,\"samples\":[",
      AWS_IOT_THING_NAME,
      sessionId.c_str(),
      capturedAt.c_str(),
      static_cast<unsigned long long>(sequence),
      static_cast<unsigned long>(SAMPLING_HZ));
  if (headerSize < 0 || static_cast<size_t>(headerSize) >= sizeof(payload)) {
    showMessage("Payload too large", "Header", COLOR_RED);
    return;
  }
  used = static_cast<size_t>(headerSize);

  for (size_t index = 0; index < sampleCount; index++) {
    if (!appendSampleJson(payload, sizeof(payload), &used, samples[index], index > 0)) {
      showMessage("Payload too large", "Samples", COLOR_RED);
      return;
    }
  }

  const int footerSize = snprintf(payload + used, sizeof(payload) - used, "]}");
  if (footerSize < 0 || static_cast<size_t>(footerSize) >= sizeof(payload) - used) {
    showMessage("Payload too large", "Footer", COLOR_RED);
    return;
  }
  used += static_cast<size_t>(footerSize);

  if (used > 4500) {
    showMessage("Payload too large", String(used).c_str(), COLOR_RED);
    return;
  }

  if (mqttClient.publish(telemetryTopic.c_str(), reinterpret_cast<const uint8_t*>(payload), used, false)) {
    Serial.printf("Telemetry sent: sequence=%llu bytes=%u\n", static_cast<unsigned long long>(sequence), static_cast<unsigned int>(used));
    sequence++;
    lastPayloadBytes = used;
  } else {
    showMessage("Publish failed", "Will retry", COLOR_RED);
  }
  sampleCount = 0;
}

void collectSample(uint32_t now) {
  if (sampleCount == 0) {
    batchStartedAtMs = now;
  }

  M5.Imu.update();
  m5::imu_data_t imu {};
  M5.Imu.getImuData(&imu);
  samples[sampleCount] = {
      .offsetMs = now - batchStartedAtMs,
      .ax = imu.accel.x,
      .ay = imu.accel.y,
      .az = imu.accel.z,
      .gx = imu.gyro.x,
      .gy = imu.gyro.y,
      .gz = imu.gyro.z,
  };
  latestSample = samples[sampleCount];
  hasLatestSample = true;
  sampleCount++;
  if (sampleCount == SAMPLES_PER_BATCH) {
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
  mqttClient.setBufferSize(MAX_PAYLOAD_BYTES + 128);

  telemetryTopic = String("$aws/rules/training_iot_basic_ingest/training/") + AWS_IOT_THING_NAME + "/telemetry";
  sessionId = iso8601Now();
  renderDashboard();
}

void loop() {
  M5.update();
  if (!mqttClient.connected()) {
    connectMqtt();
  }
  mqttClient.loop();

  const uint32_t now = millis();
  if (now - lastSampleAtMs >= SAMPLE_INTERVAL_MS) {
    lastSampleAtMs = now;
    collectSample(now);
  }
  if (now - lastDashboardAtMs >= DASHBOARD_REFRESH_MS) {
    lastDashboardAtMs = now;
    renderDashboard();
  }
}
