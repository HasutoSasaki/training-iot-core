# 構築と確認

## 作成するリソース

- AWS IoT Core Basic Ingest Rule
- Amazon Data Firehose（GZIP、1 MiBまたは60秒でS3へ保存）
- rawデータ用S3バケット、Athenaクエリ結果用S3バケット
- AWS Glue Data Catalogのデータベース・外部テーブル
- Athena Workgroup（1クエリあたり100 MiBの上限）
- ローカルGrafana用Athenaデータソース定義

S3バケットとRuleエラーログは、スタック削除時にも保持します。保存期間の自動削除は初期版では設定しません。

## ローカル設定

使用するAWSプロファイル名はリポジトリに含めず、`.env` で指定します。初回だけコピーして、
`AWS_PROFILE` に自分のプロファイル名を書いてください。`.env` はGitの管理対象外です。

```bash
cp .env.example .env
```

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

ログイン済みのプロファイルで、M5StickS3 ごとに一度だけ実行します。`m5sticks3-01` は任意の重複しない名前に置き換えてください。プロファイル名は `.env` の `AWS_PROFILE` から読み込みます。

```bash
source .env

aws iot create-thing --thing-name m5sticks3-01 --profile "$AWS_PROFILE"
aws iot create-keys-and-certificate --set-as-active \
  --certificate-pem-outfile firmware/m5sticks3-telemetry/data/aws-iot/device-certificate.pem \
  --public-key-outfile /private/tmp/m5sticks3-01-public-key.pem \
  --private-key-outfile firmware/m5sticks3-telemetry/data/aws-iot/private-key.pem \
  --profile "$AWS_PROFILE"
```

出力の `certificateArn` を使い、Thing とポリシーを関連付けます。

```bash
aws iot attach-thing-principal --thing-name m5sticks3-01 --principal CERTIFICATE_ARN --profile "$AWS_PROFILE"
aws iot attach-policy --policy-name training-iot-device-telemetry --target CERTIFICATE_ARN --profile "$AWS_PROFILE"
aws iot describe-endpoint --endpoint-type iot:Data-ATS --profile "$AWS_PROFILE"
```

endpoint とRoot CAを含む4ファイルを `firmware/m5sticks3-telemetry/data/aws-iot/` に置き、PlatformIOの `pio run -t uploadfs` でLittleFSへ書き込みます。生成したファイルは秘密鍵を含むため、Gitに追加せずローカルだけで管理します。

## ローカルGrafana

`.env` の `AWS_PROFILE` に指定したプロファイルを使います。短期認証情報はGrafanaの
起動時だけコンテナへ渡し、アクセスキーをリポジトリやGrafana設定に保存しません。

```bash
source .env
aws sso login --profile "$AWS_PROFILE"   # 利用している認証方式に合わせる
./tools/start-grafana.sh
```

Grafanaは <http://localhost:3000> で開きます。短期認証情報の有効期限後は、
もう一度ログインしてから `./tools/start-grafana.sh` を実行してコンテナを再作成します。
Athenaデータソースに必要なのは、Athena、Glue Data Catalog、rawデータ用S3、
Athena結果用S3への最小権限です。

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
