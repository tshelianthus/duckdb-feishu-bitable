#!/usr/bin/env bash

# Remove only records created by the protected COPY writeback test suite.
# The target must be the dedicated write-test Base; this script refuses a
# missing prefix so an accidental broad delete cannot be performed.

set -euo pipefail
umask 077

readonly API_BASE="https://open.feishu.cn"
readonly PAGE_SIZE=500
readonly DELETE_SIZE=500

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/feishu-write-cleanup.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

app_token="${FEISHU_WRITE_TEST_APP_TOKEN-}"
table_id="${FEISHU_WRITE_TEST_TABLE_ID-}"
app_id="${FEISHU_WRITE_TEST_APP_ID-}"
app_secret="${FEISHU_WRITE_TEST_APP_SECRET-}"
prefix="${FEISHU_WRITE_TEST_PREFIX-}"
access_token=""

if [[ -z "$app_token" || -z "$table_id" || -z "$app_id" || -z "$app_secret" || -z "$prefix" ]]; then
	echo "FEISHU_WRITE_TEST_APP_TOKEN, FEISHU_WRITE_TEST_TABLE_ID, FEISHU_WRITE_TEST_APP_ID, FEISHU_WRITE_TEST_APP_SECRET, and FEISHU_WRITE_TEST_PREFIX are required" >&2
	exit 2
fi
if [[ ! "$app_token" =~ ^[A-Za-z0-9._~-]+$ || ! "$table_id" =~ ^[A-Za-z0-9._~-]+$ ]]; then
	echo "the write-test app_token and table_id must be path-safe identifiers" >&2
	exit 2
fi
if [[ ! "$prefix" =~ ^[A-Za-z0-9._:-]+$ ]]; then
	echo "FEISHU_WRITE_TEST_PREFIX contains unsupported characters" >&2
	exit 2
fi

# The write-test Base is addressed with the application identity: exchange the
# app credentials for a short-lived tenant_access_token. Never echo either the
# credentials or the token.
fetch_tenant_access_token() {
	local body_file="$tmp_dir/tenant_token_body.json"
	local response_file="$tmp_dir/tenant_token.json"
	local http_status
	local code
	local token

	jq -n --arg id "$app_id" --arg secret "$app_secret" \
		'{app_id: $id, app_secret: $secret}' >"$body_file"

	if ! http_status="$(curl -sS --connect-timeout 10 --max-time 30 \
		-H 'Content-Type: application/json' \
		--data-binary "@$body_file" \
		-o "$response_file" -w '%{http_code}' \
		"${API_BASE}/open-apis/auth/v3/tenant_access_token/internal")"; then
		echo "TOKEN_HTTP_STATUS=transport_error FEISHU_CODE=NOT_AVAILABLE MSG=transport_error" >&2
		return 1
	fi
	code="$(jq -r 'if type == "object" and .code != null then (.code | tostring) else "N/A" end' "$response_file" 2>/dev/null || echo INVALID_JSON)"
	if [[ "$code" != "0" ]]; then
		local message
		message="$(jq -r 'if type == "object" and .msg != null then (.msg | tostring) else "" end' "$response_file" 2>/dev/null || echo response_not_json)"
		printf 'TOKEN_HTTP_STATUS=%s FEISHU_CODE=%s MSG=%s\n' "$http_status" "$code" "$(redact_message "$message")" >&2
		return 1
	fi
	token="$(jq -r '(.tenant_access_token // "") | tostring' "$response_file" 2>/dev/null || echo "")"
	if [[ -z "$token" ]]; then
		echo "token_exchange_error: response has no tenant_access_token" >&2
		return 1
	fi
	access_token="$token"
}

redact_message() {
	local value="$1"
	value="${value//"$app_token"/<REDACTED>}"
	value="${value//"$table_id"/<REDACTED>}"
	value="${value//"$app_id"/<REDACTED>}"
	value="${value//"$app_secret"/<REDACTED>}"
	if [[ -n "$access_token" ]]; then
		value="${value//"$access_token"/<REDACTED>}"
	fi
	value="$(printf '%s' "$value" | sed -E \
		-e 's/(Bearer[[:space:]]+)[^[:space:]]+/\1<REDACTED>/Ig' \
		-e 's/[[:cntrl:]]/ /g')"
	printf '%s' "$value"
}

request() {
	local method="$1"
	local url="$2"
	local body_file="$3"
	local response_file="$tmp_dir/response.json"
	local config_file="$tmp_dir/curl.conf"
	local http_status

	{
		printf 'header = "Authorization: Bearer %s"\n' "$access_token"
		printf '%s\n' 'header = "Content-Type: application/json"'
		printf 'request = "%s"\n' "$method"
		printf 'url = "%s"\n' "$url"
		printf 'output = "%s"\n' "$response_file"
		if [[ -n "$body_file" ]]; then
			printf 'data-binary = "@%s"\n' "$body_file"
		fi
	} >"$config_file"

	if ! http_status="$(curl -sS --connect-timeout 10 --max-time 60 --config "$config_file" -w '%{http_code}')"; then
		echo "HTTP_STATUS=transport_error FEISHU_CODE=NOT_AVAILABLE MSG=transport_error" >&2
		return 1
	fi
	if [[ ! "$http_status" =~ ^[0-9]{3}$ ]]; then
		http_status="transport_error"
	fi

	local code
	local message
	code="$(jq -r 'if type == "object" and .code != null then (.code | tostring) else "N/A" end' "$response_file" 2>/dev/null || echo INVALID_JSON)"
	message="$(jq -r 'if type == "object" and .msg != null then (.msg | tostring) else "" end' "$response_file" 2>/dev/null || echo response_not_json)"
	printf 'HTTP_STATUS=%s FEISHU_CODE=%s MSG=%s\n' "$http_status" "$code" "$(redact_message "$message")"
	[[ "$http_status" =~ ^2[0-9]{2}$ && "$code" == "0" ]]
}

if ! fetch_tenant_access_token; then
	exit 1
fi

ids_file="$tmp_dir/record_ids.txt"
: >"$ids_file"
page_token=""
previous_page_token=""

while :; do
	url="${API_BASE}/open-apis/bitable/v1/apps/${app_token}/tables/${table_id}/records?page_size=${PAGE_SIZE}"
	if [[ -n "$page_token" ]]; then
		encoded_page_token="$(jq -rn --arg token "$page_token" '$token | @uri')"
		url="${url}&page_token=${encoded_page_token}"
	fi
	if ! request GET "$url" ""; then
		exit 1
	fi
	response_file="$tmp_dir/response.json"
	jq -r --arg prefix "$prefix" \
		'.data.items[]? | select((.fields.name? // "") | type == "string" and startswith($prefix)) | .record_id // empty' \
		"$response_file" >>"$ids_file"

	has_more="$(jq -r 'if .data.has_more == true then "true" elif .data.has_more == false then "false" else "invalid" end' "$response_file" 2>/dev/null || echo invalid)"
	if [[ "$has_more" == "false" ]]; then
		break
	fi
	if [[ "$has_more" != "true" ]]; then
		echo "pagination_error: response has no valid has_more value" >&2
		exit 1
	fi
	page_token="$(jq -r '(.data.page_token // "") | tostring' "$response_file" 2>/dev/null || echo "")"
	if [[ -z "$page_token" || "$page_token" == "$previous_page_token" ]]; then
		echo "pagination_error: has_more response has an empty or repeated page_token" >&2
		exit 1
	fi
	previous_page_token="$page_token"
done

sort -u "$ids_file" -o "$ids_file"
total="$(wc -l <"$ids_file" | tr -d '[:space:]')"
echo "MATCHED_RECORDS=$total PREFIX=$prefix"

offset=0
while [[ "$offset" -lt "$total" ]]; do
	chunk_file="$tmp_dir/delete_${offset}.json"
	chunk_ids="$tmp_dir/delete_${offset}.ids"
	sed -n "$((offset + 1)),$((offset + DELETE_SIZE))p" "$ids_file" >"$chunk_ids"
	jq -R -s '{records: (split("\n") | map(select(length > 0)))}' "$chunk_ids" >"$chunk_file"
	echo -n "DELETE_OFFSET=$offset "
	request POST "${API_BASE}/open-apis/bitable/v1/apps/${app_token}/tables/${table_id}/records/batch_delete" "$chunk_file"
	offset=$((offset + DELETE_SIZE))
done
