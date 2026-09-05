# テレメトリ仕様（初期版）

## 送信先

デバイスは、次の Basic Ingest トピックへ MQTT/TLS で送信します。

```text
$aws/rules/training_iot_basic_ingest/training/{deviceId}/telemetry
```

Basic Ingest のトピックは購読できません。`device_id` はペイロードに必ず含めます。

## 送信単位

- IMU 1サンプルごとではなく、複数サンプルを `samples` にまとめて送信する。
- JSON化後のペイロードは 4.5 KiB 以下を目標にする。5 KiBを超えるとIoT Core、Rule、Firehoseの課金単位が増える。
- `sequence` を単調増加させ、欠落・重複を分析時に確認できるようにする。

## JSON 形式

```json
{
  "schema_version": 1,
  "device_id": "m5sticks3-01",
  "session_id": "2026-08-29T10:00:00+09:00",
  "captured_at": "2026-08-29T10:00:05+09:00",
  "sequence": 1,
  "sampling_hz": 50,
  "samples": [
    {
      "offset_ms": 0,
      "ax": 0.12,
      "ay": -0.03,
      "az": 0.98,
      "gx": 1.2,
      "gy": -0.4,
      "gz": 0.1
    }
  ]
}
```

`ingested_at` はIoT Ruleが付与する受信時刻（Unix epoch milliseconds）です。運動時刻の分析には `captured_at` と `offset_ms` を使います。
