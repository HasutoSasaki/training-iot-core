#!/usr/bin/env bash
set -euo pipefail

# 使うAWSプロファイルは .env の AWS_PROFILE で指定する。
# 個人のアカウントに依存しないよう、リポジトリにはプロファイル名を書かない。
# 認証情報は一時ファイルとコンテナ環境変数だけで扱い、リポジトリには保存しない。
# リポジトリのルートで実行する前提（docker compose が .env を読むのと同じ場所）。
if [ -f .env ]; then
  # shellcheck disable=SC1091
  source .env
fi

if [ -z "${AWS_PROFILE:-}" ]; then
  echo "AWS_PROFILE が設定されていません。.env.example を .env にコピーして、使うプロファイル名を書いてください。" >&2
  exit 1
fi

umask 077
credentials_file="$(mktemp "${TMPDIR:-/tmp}/training-iot-grafana.XXXXXX")"
cleanup() {
  rm -f "$credentials_file"
}
trap cleanup EXIT

aws configure export-credentials \
  --profile "$AWS_PROFILE" \
  --format env > "$credentials_file"

# shellcheck disable=SC1090
source "$credentials_file"
export AWS_REGION="${AWS_REGION:-ap-northeast-1}"

docker compose up -d --force-recreate grafana
