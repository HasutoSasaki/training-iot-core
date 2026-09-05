# 構築と確認

## 作成するリソース

- AWS IoT Core Basic Ingest Rule
- Amazon Data Firehose（GZIP、1 MiBまたは60秒でS3へ保存）
- rawデータ用S3バケット、Athenaクエリ結果用S3バケット
- AWS Glue Data Catalogのデータベース・外部テーブル
- Athena Workgroup（1クエリあたり100 MiBの上限）
- ローカルGrafana用Athenaデータソース定義

S3バケットとRuleエラーログは、スタック削除時にも保持します。保存期間の自動削除は初期版では設定しません。

## デプロイ前の確認

```bash
pnpm install --frozen-lockfile
pnpm typecheck
pnpm synth
```

対象アカウントとリージョンを確認したうえで、初回だけCDKをbootstrapします。

```bash
pnpm exec cdk bootstrap aws://ACCOUNT_ID/ap-northeast-1
pnpm exec cdk deploy
```

`cdk deploy` の出力から、S3バケット名・Athenaデータベース名・Workgroup名を控えます。

## 実機用の Thing と証明書

CDK は `training-iot-device-telemetry` ポリシーを作成します。このポリシーは、Thing に関連付けられた証明書が、その Thing 名と同じ `device_id` の Basic Ingest トピックへ発行することだけを許可します。

ログイン済みの `training-iot-sandbox` プロファイルで、M5StickS3 ごとに一度だけ実行します。`m5sticks3-01` は任意の重複しない名前に置き換えてください。

```bash
aws iot create-thing --thing-name m5sticks3-01 --profile training-iot-sandbox
aws iot create-keys-and-certificate --set-as-active \
  --certificate-pem-outfile firmware/m5sticks3-telemetry/data/aws-iot/device-certificate.pem \
  --public-key-outfile /private/tmp/m5sticks3-01-public-key.pem \
  --private-key-outfile firmware/m5sticks3-telemetry/data/aws-iot/private-key.pem \
  --profile training-iot-sandbox
```

出力の `certificateArn` を使い、Thing とポリシーを関連付けます。

```bash
aws iot attach-thing-principal --thing-name m5sticks3-01 --principal CERTIFICATE_ARN --profile training-iot-sandbox
aws iot attach-policy --policy-name training-iot-device-telemetry --target CERTIFICATE_ARN --profile training-iot-sandbox
aws iot describe-endpoint --endpoint-type iot:Data-ATS --profile training-iot-sandbox
```

endpoint とRoot CAを含む4ファイルを `firmware/m5sticks3-telemetry/data/aws-iot/` に置き、PlatformIOの `pio run -t uploadfs` でLittleFSへ書き込みます。生成したファイルは秘密鍵を含むため、Gitに追加せずローカルだけで管理します。

## ローカルGrafana

`sandbox` でログインし、そこから `176545285577` の
`OrganizationAccountAccessRole` を引き受ける
`training-iot-sandbox` プロファイルを使います。短期認証情報はGrafanaの
起動時だけコンテナへ渡し、アクセスキーをリポジトリやGrafana設定に保存しません。

```bash
aws login --profile sandbox
./tools/start-grafana.sh
```

Grafanaは <http://localhost:3000> で開きます。短期認証情報の有効期限後は、
もう一度 `aws login --profile sandbox`（必要な場合）と
`./tools/start-grafana.sh` を実行してコンテナを再作成します。Athenaデータソースに
必要なのは、Athena、Glue Data Catalog、rawデータ用S3、Athena結果用S3への最小権限です。

## 最初のAthenaクエリ

日時の範囲を必ず指定します。パーティション投影を使うため、`datehour` が保存先の `yyyy/MM/dd/HH` と一致する必要があります。

```sql
SELECT
  captured_at,
  device_id,
  sequence,
  samples
FROM training_iot.telemetry_raw
WHERE datehour BETWEEN '2026/08/29/09' AND '2026/08/29/10'
ORDER BY captured_at
LIMIT 100;
```
