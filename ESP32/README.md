# ESP32 MQTT gateway

The ESP32 is a **network coprocessor**, not a second application processor. It
translates the STM32's UART telemetry into MQTT and relays MQTT commands back.
No sampling, no filtering, no timing-critical work happens here.

That split is deliberate. Wi-Fi association, DHCP, TLS handshakes and MQTT
reconnection all have unbounded latency. Putting them on the same core as a
1 Hz deterministic sampling loop is how you end up with jitter you cannot
explain. Here the STM32's schedule is unaffected by anything the network does.

## Wiring

| STM32 (NUCLEO-F103RB) | ESP32 DevKit | Note |
|---|---|---|
| PA9 — USART1_TX | GPIO16 (RX2) | STM32 transmits |
| PA10 — USART1_RX | GPIO17 (TX2) | STM32 receives |
| GND | GND | **required** — a shared ground reference |

Both boards use 3.3 V logic, so no level shifting is needed. Do not power the
Nucleo from the ESP32's 3V3 pin; give each board its own USB supply and join
only the grounds.

## Setup

1. Arduino IDE → Boards Manager → install **esp32** by Espressif.
2. Library Manager → install **PubSubClient** by Nick O'Leary.
3. Edit the configuration block at the top of `mqtt_gateway.ino`:
   Wi-Fi SSID/password, broker host, and the topics if you want different ones.
4. Select your ESP32 board and port, then upload.

## Topics

| Topic | Direction | Payload |
|---|---|---|
| `construction/site1/environment` | ESP32 → broker | telemetry JSON |
| `construction/site1/status` | ESP32 → broker | gateway events and STM32 command responses |
| `construction/site1/command` | broker → ESP32 → STM32 | a command line, e.g. `SET_PERIOD=500` |

Telemetry payload:

```json
{
  "temperature": 25.30,
  "humidity": 45.80,
  "pressure": 1012.40,
  "gas": 45231,
  "air_quality": 72,
  "status": "NORMAL",
  "uptime_ms": 12345,
  "sequence": 42
}
```

## Trying it without hardware

A local broker is enough to see the whole path work:

```bash
# Terminal 1 — broker
mosquitto -v

# Terminal 2 — watch telemetry
mosquitto_sub -h localhost -t 'construction/site1/#' -v

# Terminal 3 — change the sampling period from the cloud side
mosquitto_pub -h localhost -t 'construction/site1/command' -m 'SET_PERIOD=500'
```

The STM32 answers `OK PERIOD=500`, which the gateway republishes on the status
topic.
