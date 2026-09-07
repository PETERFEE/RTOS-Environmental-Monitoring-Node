/*
 * mqtt_gateway.ino -- ESP32 network coprocessor for rtos-iot-sensor-node
 *
 * The STM32 does the real-time work: sampling on a deterministic schedule,
 * RTOS scheduling, bus arbitration. This sketch does none of that. It is a
 * protocol translator, and keeping it that way is the point of the two-chip
 * architecture: Wi-Fi association, DHCP, TLS and MQTT reconnection all have
 * unbounded, unpredictable latency, and none of it is allowed anywhere near
 * the sampling loop.
 *
 * Wiring (see docs/wiring.md)
 *   STM32 PA9  (USART1_TX) --> ESP32 GPIO16 (RX2)
 *   STM32 PA10 (USART1_RX) <-- ESP32 GPIO17 (TX2)
 *   STM32 GND              --- ESP32 GND      <-- required, common ground
 *
 * Both boards are 3.3 V logic, so no level shifting is needed.
 *
 * Uplink   : "T=25.30,H=45.80,P=1012.40,G=45231,A=72,S=NORMAL,TS=12345,N=42"
 *            -> JSON on  construction/site1/environment
 * Downlink : MQTT payload on construction/site1/command -> written to the STM32
 *            verbatim with a newline, e.g. "SET_PERIOD=500"
 *
 * Dependencies (Library Manager): PubSubClient by Nick O'Leary.
 * Board: any ESP32 dev kit; select it in Tools -> Board.
 */

#include <WiFi.h>
#include <PubSubClient.h>

// ---------------------------------------------------------------------------
//  Configuration -- edit these
// ---------------------------------------------------------------------------
static const char *WIFI_SSID     = "your-ssid";
static const char *WIFI_PASSWORD = "your-password";

static const char *MQTT_HOST     = "192.168.1.10";   // broker IP or hostname
static const uint16_t MQTT_PORT  = 1883;
static const char *MQTT_CLIENT   = "stm32-env-node-1";
static const char *MQTT_USER     = nullptr;          // set if your broker needs auth
static const char *MQTT_PASS     = nullptr;

static const char *TOPIC_DATA    = "construction/site1/environment";
static const char *TOPIC_STATUS  = "construction/site1/status";
static const char *TOPIC_COMMAND = "construction/site1/command";

// UART2 to the STM32
static const int  UART_RX_PIN = 16;
static const int  UART_TX_PIN = 17;
static const long UART_BAUD   = 115200;

static const size_t LINE_MAX = 192;

// ---------------------------------------------------------------------------
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

static char   lineBuf[LINE_MAX];
static size_t lineLen = 0;

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

/** Extract the value for `key=` out of a comma-separated key=value line. */
static bool fieldValue(const char *line, const char *key, char *out, size_t outSize)
{
  const size_t keyLen = strlen(key);
  const char  *p      = line;

  while (p != nullptr && *p != '\0') {
    // A match must start at the beginning of the line or just after a comma.
    if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
      const char *v   = p + keyLen + 1;
      const char *end = strchr(v, ',');
      size_t      n   = (end != nullptr) ? (size_t)(end - v) : strlen(v);

      if (n >= outSize) n = outSize - 1;
      memcpy(out, v, n);
      out[n] = '\0';
      return true;
    }

    p = strchr(p, ',');
    if (p != nullptr) p++;
  }
  return false;
}

/** Turn one telemetry line into JSON and publish it. */
static void publishTelemetry(const char *line)
{
  char t[16], h[16], pr[16], g[16], a[8], s[20], ts[16], n[16];

  const bool haveT = fieldValue(line, "T", t, sizeof(t));
  const bool haveH = fieldValue(line, "H", h, sizeof(h));

  if (!haveT || !haveH) {
    // Not a telemetry line (could be a command response). Pass it through so
    // nothing is silently swallowed.
    mqtt.publish(TOPIC_STATUS, line);
    return;
  }

  if (!fieldValue(line, "P",  pr, sizeof(pr))) strcpy(pr, "0");
  if (!fieldValue(line, "G",  g,  sizeof(g)))  strcpy(g,  "0");
  if (!fieldValue(line, "A",  a,  sizeof(a)))  strcpy(a,  "0");
  if (!fieldValue(line, "S",  s,  sizeof(s)))  strcpy(s,  "UNKNOWN");
  if (!fieldValue(line, "TS", ts, sizeof(ts))) strcpy(ts, "0");
  if (!fieldValue(line, "N",  n,  sizeof(n)))  strcpy(n,  "0");

  char json[288];
  snprintf(json, sizeof(json),
           "{\"temperature\":%s,\"humidity\":%s,\"pressure\":%s,"
           "\"gas\":%s,\"air_quality\":%s,\"status\":\"%s\","
           "\"uptime_ms\":%s,\"sequence\":%s}",
           t, h, pr, g, a, s, ts, n);

  if (!mqtt.publish(TOPIC_DATA, json)) {
    Serial.println("[gw] publish failed");
  }
}

/** MQTT -> STM32. Payloads are forwarded verbatim plus a newline. */
static void onMqttMessage(char *topic, byte *payload, unsigned int length)
{
  if (length == 0 || length > 64) return;

  char cmd[66];
  memcpy(cmd, payload, length);
  cmd[length] = '\0';

  Serial.printf("[gw] command: %s\n", cmd);
  Serial2.print(cmd);
  Serial2.print('\n');
}

static void ensureWifi()
{
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("[gw] wifi connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[gw] wifi ok, ip=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[gw] wifi FAILED, will retry");
  }
}

static void ensureMqtt()
{
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;

  // Backoff so a dead broker does not turn into a tight reconnect loop.
  static unsigned long nextAttempt = 0;
  if (millis() < nextAttempt) return;

  Serial.print("[gw] mqtt connecting... ");
  const bool ok = (MQTT_USER != nullptr)
                    ? mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)
                    : mqtt.connect(MQTT_CLIENT);

  if (ok) {
    Serial.println("connected");
    mqtt.subscribe(TOPIC_COMMAND);
    mqtt.publish(TOPIC_STATUS, "gateway online");
  } else {
    Serial.printf("failed rc=%d\n", mqtt.state());
    nextAttempt = millis() + 5000;
  }
}

// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);                                     // USB console
  Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);   // STM32 link

  Serial.println();
  Serial.println("[gw] rtos-iot-sensor-node ESP32 gateway");

  ensureWifi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
}

void loop()
{
  ensureWifi();
  ensureMqtt();
  mqtt.loop();

  // Assemble whole lines from the STM32. Partial lines are held until the
  // terminating newline arrives, so a split UART read never produces a
  // truncated MQTT message.
  while (Serial2.available() > 0) {
    const char c = (char)Serial2.read();

    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        Serial.printf("[stm32] %s\n", lineBuf);
        if (mqtt.connected()) publishTelemetry(lineBuf);
        lineLen = 0;
      }
      continue;
    }

    if (lineLen < LINE_MAX - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineLen = 0;      // overlong line: drop it rather than publish garbage
    }
  }
}
