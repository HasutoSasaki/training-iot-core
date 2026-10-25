# テレメトリ仕様

現行は `schema_version` 2 です。v1 で保存したデータも残るため、分析側は `schema_version` を見て単位を切り替えます。

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

v2 は 100 Hz で取得し、0.5秒ごとに50サンプルを1メッセージで送ります。1秒あたり2メッセージです。

## JSON 形式

```json
{
  "schema_version": 2,
  "device_id": "m5sticks3-01",
  "session_id": "2026-09-13T10:00:00+09:00",
  "set_id": 3,
  "placement": "barbell",
  "captured_at": "2026-09-13T10:05:12+09:00",
  "batch_start_ms": 1757725512340,
  "sequence": 412,
  "sampling_hz": 100,
  "samples": [
    { "offset_ms": 0, "ax": -12, "ay": 988, "az": 123, "gx": -1235, "gy": 123, "gz": -12 },
    { "offset_ms": 10, "ax": -10, "ay": 991, "az": 120, "gx": -1198, "gy": 130, "gz": -9 }
  ]
}
```

## フィールド

| フィールド | 型 | 意味 |
| --- | --- | --- |
| `schema_version` | int | ペイロードの版。現行は 2 |
| `device_id` | string | Thing 名と同じ。Basic Ingest では購読できないため必ず含める |
| `session_id` | string | 起動時に1回決まる。同じ電源投入中は変わらない |
| `set_id` | int | セット番号。ボタンAを押すごとに 1 増える。起動時は 0 |
| `placement` | string | 装着位置。`barbell`、将来 `wrist_left` などを追加 |
| `captured_at` | string | 人が読むための秒精度の時刻。分析では `batch_start_ms` を使う |
| `batch_start_ms` | bigint | バッチ先頭サンプルの Unix epoch ミリ秒 |
| `sequence` | bigint | 単調増加。欠落・重複の確認に使う |
| `sampling_hz` | int | 公称値。実際の間隔は `offset_ms` の差で確認する |
| `samples[].offset_ms` | int | バッチ先頭からの経過ミリ秒。予定値ではなく実測値 |
| `samples[].ax` `ay` `az` | int | 加速度。mg（1000 が 1 g） |
| `samples[].gx` `gy` `gz` | int | 角速度。0.1 dps（10 が 1 dps） |

各サンプルの絶対時刻は `batch_start_ms + offset_ms` で求めます。`captured_at` は秒精度のため、バッチをまたいだ連結には使いません。

`set_id` は端末に保存しないため、電源を入れ直すと 0 に戻ります。日をまたいで増え続けることはありません。どの起動の何セット目かは `session_id` と `set_id` の組で区別します。ボタンを一度押したあとは休憩中も番号が変わらないので、セット本番の部分の切り出しは分析側で動きの有無から行います。

`ingested_at` はIoT Ruleが付与する受信時刻（Unix epoch ミリ秒）です。デバイス側の時刻ではないため、運動時刻の分析には使いません。

## v1 との違い

| 項目 | v1 | v2 |
| --- | --- | --- |
| サンプリング | 10 Hz | 100 Hz |
| 送信単位 | 1秒ごとに10サンプル | 0.5秒ごとに50サンプル |
| 加速度 `ax` `ay` `az` | g の小数（例 `0.988`） | mg の整数（例 `988`） |
| 角速度 `gx` `gy` `gz` | dps の小数（例 `-123.5`） | 0.1 dps の整数（例 `-1235`） |
| `set_id` | なし | あり |
| `placement` | なし | あり |
| `batch_start_ms` | なし | あり |

整数にしたのは、100 Hz・50サンプルを 4.5 KiB のペイロードに収めるためです。キー名を変えていないので、Glue テーブルの `samples` 構造体（`double` 型）をそのまま使えます。JSON の整数は `double` 列として読めます。

## v1 のデータの扱い

v1 で保存済みのデータは変換せずそのまま残します。Glue テーブルには v2 で追加した列があるため、v1 の行では `set_id`、`placement`、`batch_start_ms` が NULL になります。

分析時は `schema_version` で単位を切り替えます。

```sql
SELECT
  captured_at,
  device_id,
  set_id,
  CASE WHEN schema_version >= 2 THEN s.ax / 1000.0 ELSE s.ax END AS ax_g
FROM training_iot.telemetry_raw
CROSS JOIN UNNEST(samples) AS t(s)
WHERE datehour BETWEEN '2026/09/13/10' AND '2026/09/13/11'
LIMIT 100;
```
