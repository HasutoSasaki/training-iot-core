# M5StickS3 テレメトリ送信

このプロジェクトは BMI270 の加速度・ジャイロを 10 Hz で読み、1秒ごとに10件をまとめて Basic Ingest へ送信します。

## 接続情報

`data/aws-iot/` に次の4ファイルを置き、LittleFSへ書き込みます。

- `endpoint.txt`
- `amazon-root-ca.pem`
- `device-certificate.pem`
- `private-key.pem`

このディレクトリはGitの管理対象外です。証明書・秘密鍵をコミットしないでください。Wi-Fi情報は端末に保存済みであることを前提に `WiFi.begin()` を使います。

## 書き込み

M5StickS3 を USB 接続し、PlatformIO で `m5sticks3` 環境をビルド・書き込みます。

```sh
cd firmware/m5sticks3-telemetry
pio run -t upload
pio run -t uploadfs
pio device monitor
```

画面に `Telemetry sent` が表示されれば、1秒ごとに1バッチを送信しています。Firehose のバッファリングがあるため、Grafana に現れるまで最大約1分かかります。
