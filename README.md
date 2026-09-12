# Training IoT Core

M5Stack S3 系デバイスを使って、トレーニング中の動作データを収集・保存・分析するための実験用リポジトリです。

初期段階では、ベンチプレスに限定せず、加速度・ジャイロなどのセンサーデータを AWS IoT Core 経由で扱える共通基盤を整えます。

## 構成

| ディレクトリ | 目的 |
| --- | --- |
| `firmware/` | M5Stack S3 系デバイス向けファームウェア |
| `infra/` | AWS IoT Core などのクラウド基盤 |
| `analysis/` | 収集データの探索・分析 |
| `docs/` | 接続手順・データ仕様・設計メモ |

## ドキュメント

- [構成とデータの流れ](docs/architecture.md): 実機、AWS、ローカルGrafanaの全体像
- [構築と確認](docs/operations.md): デプロイ、認証、Grafana起動の手順
- [テレメトリ仕様](docs/telemetry-schema.md): MQTTトピックとJSON形式
- [ベンチプレス分析の提案](docs/bench-press-analysis.md): デバイス取付位置と使用データ、必要な変更

## 最初の到達点

1. デバイスが IMU の時系列データを取得する。
2. デバイスが MQTT/TLS で AWS IoT Core に送信する。
3. IoT Rule が時系列データを保存し、分析できる状態にする。

> [!NOTE]
> AWS アカウント、IoT Thing、証明書、Wi-Fi 情報はこのリポジトリに保存しません。ローカルの `.env` または安全なシークレット管理を使います。

## 開発方針

- ファームウェアは PlatformIO を基盤にします。
- AWS インフラは CDK（TypeScript）で管理します。
- 集計済みのIMUデータは Basic Ingest で Rule に直接送信し、Firehose、S3、Athenaを経由してローカルGrafanaで可視化します。
- データ形式・運動種目・分析ロジックは、実機で取得したデータを見ながら追加します。

初期構成では、運動中の姿勢判定をクラウドに依存させません。端末が即時判定を行い、クラウドは保存と後からの可視化を担います。

## 次にすること

- Grafanaの時間範囲とデバイス選択を変数化する。
- 実機で取得したデータから運動種目ごとの分析方法を検討する。
