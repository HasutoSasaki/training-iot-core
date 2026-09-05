#!/usr/bin/env bash
set -euo pipefail

# sandbox の aws login を元に、176545285577 の AssumeRole短期認証情報を取得する。
# 認証情報は一時ファイルとコンテナ環境変数だけで扱い、リポジトリには保存しない。
umask 077
credentials_file="$(mktemp "${TMPDIR:-/tmp}/training-iot-grafana.XXXXXX")"
cleanup() {
  rm -f "$credentials_file"
}
trap cleanup EXIT

aws configure export-credentials \
  --profile training-iot-sandbox \
  --format env > "$credentials_file"

# shellcheck disable=SC1090
source "$credentials_file"
export AWS_REGION="${AWS_REGION:-ap-northeast-1}"

docker compose up -d --force-recreate grafana
