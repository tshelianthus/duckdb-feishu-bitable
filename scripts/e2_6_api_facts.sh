#!/usr/bin/env bash

# E-2.6 Feishu Bitable API fact probe.
#
# This script deliberately does not source .env files. Every credential and
# target identifier must already be present in the process environment.

set -euo pipefail

readonly DEFAULT_API_BASE="https://open.feishu.cn"

tmp_dir=""
primary_token=""
acquired_token=""
readonly_token=""
noncollab_app_token=""
run_failures=0
cleanup_failures=0

primary_created=()
readonly_created=()
noncollab_app_created=()
noncollab_user_created=()

api_http_status=""
api_body=""
api_code=""
api_msg=""
api_records_count=""
api_page_token=""
api_has_more=""

usage() {
	cat >&2 <<'EOF'
Usage: scripts/e2_6_api_facts.sh

Required target variables:
  FEISHU_E26_APP_TOKEN       Dedicated write-test Base app_token
  FEISHU_E26_TABLE_ID        Dedicated write-test table_id
  FEISHU_E26_WRITABLE_FIELD  Existing writable field name, usually text
  FEISHU_E26_READONLY_FIELD  Existing formula/auto-number/system field name

Required primary write identity (choose exactly one mode):
  FEISHU_E26_WRITE_APP_ID + FEISHU_E26_WRITE_APP_SECRET
  FEISHU_E26_WRITE_USER_ACCESS_TOKEN

Optional fact scenarios:
  FEISHU_E26_READONLY_APP_ID + FEISHU_E26_READONLY_APP_SECRET
      App identity with only the read-only Bitable scope.
  FEISHU_E26_NONCOLLAB_APP_ID + FEISHU_E26_NONCOLLAB_APP_SECRET
      App identity that is not a collaborator of the dedicated Base.
  FEISHU_E26_NONCOLLAB_USER_ACCESS_TOKEN
      User identity that is not a collaborator of the dedicated Base.

Optional routing variable:
  FEISHU_E26_API_BASE_URL       HTTPS API origin; defaults to open.feishu.cn

Optional scenario variables are never treated as a pass when absent: their
cases are emitted as SKIPPED. The script creates only marker records in the
dedicated Base and attempts batch_delete cleanup on normal and abnormal exit.
EOF
}

usage_error() {
	printf 'error: %s\n' "$1" >&2
	usage
	exit 2
}

# These functions are invoked through EXIT/INT/TERM traps, which ShellCheck
# cannot infer from their declarations.
# shellcheck disable=SC2329
cleanup_ids() {
	local label="$1"
	local token="$2"
	shift 2
	local total=$#
	local offset=0
	local limit=500
	local chunk_size
	local ids_file
	local payload_file
	local -a chunk_ids

	if [[ "$total" -eq 0 ]]; then
		return 0
	fi
	if [[ -z "$token" ]]; then
		emit_skipped "${label}_delete" "cleanup_token_unavailable"
		cleanup_failures=$((cleanup_failures + 1))
		return 0
	fi

	while [[ "$offset" -lt "$total" ]]; do
		chunk_size=$((total - offset))
		if [[ "$chunk_size" -gt "$limit" ]]; then
			chunk_size=$limit
		fi
		chunk_ids=("${@:offset+1:chunk_size}")
		ids_file="$tmp_dir/${label}_${offset}_ids.txt"
		payload_file="$tmp_dir/${label}_${offset}_payload.json"
		printf '%s\n' "${chunk_ids[@]}" >"$ids_file"
		jq -R -s '{records: (split("\n") | map(select(length > 0)))}' \
			"$ids_file" >"$payload_file"
		api_request "POST" "${api_base}/open-apis/bitable/v1/apps/${app_token}/tables/${table_id}/records/batch_delete" \
			"$token" "$payload_file"
		emit_evidence "${label}_delete_${offset}"
		if [[ "$api_code" != "0" ]]; then
			cleanup_failures=$((cleanup_failures + 1))
		fi
		offset=$((offset + chunk_size))
	done
}

# shellcheck disable=SC2329
cleanup() {
	local exit_status=$?
	trap - EXIT INT TERM
	set +e

	# Under `set -u`, empty arrays must not expand to an unbound reference.
	cleanup_ids "primary" "$primary_token" ${primary_created[@]+"${primary_created[@]}"}
	cleanup_ids "readonly_app" "$readonly_token" ${readonly_created[@]+"${readonly_created[@]}"}
	cleanup_ids "noncollab_app" "$noncollab_app_token" ${noncollab_app_created[@]+"${noncollab_app_created[@]}"}
	cleanup_ids "noncollab_user" "$noncollab_user_access_token" ${noncollab_user_created[@]+"${noncollab_user_created[@]}"}

	if [[ -n "$tmp_dir" && -d "$tmp_dir" ]]; then
		rm -rf "$tmp_dir"
	fi
	if [[ "$exit_status" -eq 0 && ("$run_failures" -gt 0 || "$cleanup_failures" -gt 0) ]]; then
		exit_status=1
	fi
	exit "$exit_status"
}

trap cleanup EXIT INT TERM

if ! command -v curl >/dev/null 2>&1; then
	usage_error "curl is required"
fi
if ! command -v jq >/dev/null 2>&1; then
	usage_error "jq is required"
fi

app_token="${FEISHU_E26_APP_TOKEN-}"
table_id="${FEISHU_E26_TABLE_ID-}"
writable_field="${FEISHU_E26_WRITABLE_FIELD-}"
readonly_field="${FEISHU_E26_READONLY_FIELD-}"
write_app_id="${FEISHU_E26_WRITE_APP_ID-}"
write_app_secret="${FEISHU_E26_WRITE_APP_SECRET-}"
write_user_access_token="${FEISHU_E26_WRITE_USER_ACCESS_TOKEN-}"
readonly_app_id="${FEISHU_E26_READONLY_APP_ID-}"
readonly_app_secret="${FEISHU_E26_READONLY_APP_SECRET-}"
noncollab_app_id="${FEISHU_E26_NONCOLLAB_APP_ID-}"
noncollab_app_secret="${FEISHU_E26_NONCOLLAB_APP_SECRET-}"
noncollab_user_access_token="${FEISHU_E26_NONCOLLAB_USER_ACCESS_TOKEN-}"
api_base="${FEISHU_E26_API_BASE_URL:-$DEFAULT_API_BASE}"
api_base="${api_base%/}"

for required_value in \
	"$app_token" "$table_id" "$writable_field" "$readonly_field"; do
	if [[ -z "$required_value" ]]; then
		usage_error "one or more required variables are missing"
	fi
done

if [[ -z "$write_user_access_token" && -z "$write_app_id" && -z "$write_app_secret" ]]; then
	usage_error "a primary write identity is missing"
fi
if [[ -n "$write_user_access_token" && ( -n "$write_app_id" || -n "$write_app_secret" ) ]]; then
	usage_error "primary write identity modes are mutually exclusive"
fi
if [[ -n "$write_app_id" && -z "$write_app_secret" ]] || [[ -z "$write_app_id" && -n "$write_app_secret" ]]; then
	usage_error "both primary write app variables are required together"
fi
if [[ -n "$readonly_app_id" && -z "$readonly_app_secret" ]] || [[ -z "$readonly_app_id" && -n "$readonly_app_secret" ]]; then
	usage_error "both read-only app variables are required together"
fi
if [[ -n "$noncollab_app_id" && -z "$noncollab_app_secret" ]] || [[ -z "$noncollab_app_id" && -n "$noncollab_app_secret" ]]; then
	usage_error "both non-collaborator app variables are required together"
fi
if [[ "$api_base" != https://* ]]; then
	usage_error "FEISHU_E26_API_BASE_URL must use HTTPS"
fi
if [[ ! "$app_token" =~ ^[A-Za-z0-9._~-]+$ || ! "$table_id" =~ ^[A-Za-z0-9._~-]+$ ]]; then
	usage_error "the app_token and table_id must be path-safe identifiers"
fi

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/feishu-e26.XXXXXX")"
run_id="$(date -u '+%Y%m%dT%H%M%SZ')-$$"

redact_message() {
	local value="$1"
	local candidate
	for candidate in \
		"$write_app_id" "$write_app_secret" "$write_user_access_token" \
		"$readonly_app_id" "$readonly_app_secret" \
		"$noncollab_app_id" "$noncollab_app_secret" "$noncollab_user_access_token" \
		"$app_token" "$table_id"; do
		if [[ -n "$candidate" ]]; then
			value="${value//"$candidate"/<REDACTED>}"
		fi
	done
	value="$(printf '%s' "$value" | sed -E \
		-e 's/(Bearer[[:space:]]+)[^[:space:]]+/\1<REDACTED>/Ig' \
		-e 's/((app_?secret|access_?token|authorization)[[:space:]]*[:=][[:space:]]*)[^,;[:space:]]+/\1<REDACTED>/Ig' \
		-e 's/[[:cntrl:]]/ /g')"
	printf '%s' "$value"
}

parse_response() {
	api_code="N/A"
	api_msg=""
	api_records_count="N/A"
	api_page_token="N/A"
	api_has_more="N/A"

	if [[ "$api_http_status" == "transport_error" ]]; then
		api_code="NOT_AVAILABLE"
		api_msg="transport_error"
		return 0
	fi
	if [[ ! -s "$api_body" ]]; then
		api_code="EMPTY_RESPONSE"
		api_msg="empty_response"
		return 0
	fi

	if ! api_code="$(jq -r 'if type == "object" and .code != null then (.code | tostring) else "N/A" end' "$api_body" 2>/dev/null)"; then
		api_code="INVALID_JSON"
	fi
	if ! api_msg="$(jq -r 'if type == "object" and .msg != null then (.msg | tostring) else "" end' "$api_body" 2>/dev/null)"; then
		api_msg="response_not_json"
	fi
	if ! api_records_count="$(jq -r 'if (.data.records? | type) == "array" then (.data.records | length | tostring) else "N/A" end' "$api_body" 2>/dev/null)"; then
		api_records_count="N/A"
	fi
	if ! api_page_token="$(jq -r '((.data.page_token // .data.cursor) // "N/A") | tostring' "$api_body" 2>/dev/null)"; then
		api_page_token="N/A"
	fi
	if ! api_has_more="$(jq -r 'if .data.has_more == true then "true" elif .data.has_more == false then "false" else "N/A" end' "$api_body" 2>/dev/null)"; then
		api_has_more="N/A"
	fi
	api_msg="$(redact_message "$api_msg")"
}

emit_evidence() {
	local label="$1"
	parse_response
	printf 'CASE=%s HTTP_STATUS=%s FEISHU_CODE=%s MSG=%s DATA_RECORDS_COUNT=%s DATA_PAGE_TOKEN=%s DATA_HAS_MORE=%s\n' \
		"$label" "$api_http_status" "$api_code" "$api_msg" "$api_records_count" "$api_page_token" "$api_has_more"
}

emit_skipped() {
	local label="$1"
	local reason="$2"
	printf 'CASE=%s HTTP_STATUS=SKIPPED FEISHU_CODE=NOT_RUN MSG=%s DATA_RECORDS_COUNT=N/A DATA_PAGE_TOKEN=N/A DATA_HAS_MORE=N/A\n' \
		"$label" "$reason"
}

api_request() {
	local method="$1"
	local url="$2"
	local token="$3"
	local payload_file="$4"
	local curl_status
	local curl_stderr="$tmp_dir/curl.stderr"
	local response_file
	local curl_config
	local -a curl_args

	response_file="$(mktemp "$tmp_dir/response.XXXXXX")"
	curl_config="$(mktemp "$tmp_dir/curl_config.XXXXXX")"
	{
		printf '%s\n' 'header = "Content-Type: application/json"'
		if [[ -n "$token" ]]; then
			printf 'header = "Authorization: Bearer %s"\n' "$token"
		fi
	} >"$curl_config"
	curl_args=(curl -sS --connect-timeout 10 --max-time 60 -X "$method")
	curl_args+=( --config "$curl_config" --data-binary "@$payload_file" -o "$response_file" -w '%{http_code}' "$url" )

	if ! curl_status="$("${curl_args[@]}" 2>"$curl_stderr")"; then
		api_http_status="transport_error"
		api_body="$response_file"
		return 0
	fi
	if [[ ! "$curl_status" =~ ^[0-9]{3}$ ]]; then
		api_http_status="transport_error"
	else
		api_http_status="$curl_status"
	fi
	api_body="$response_file"
}

acquire_app_token() {
	local label="$1"
	local app_id_value="$2"
	local app_secret_value="$3"
	local auth_payload="$tmp_dir/${label}_auth.json"
	local app_id_file="$tmp_dir/${label}_app_id"
	local app_secret_file="$tmp_dir/${label}_app_secret"

	printf '%s' "$app_id_value" >"$app_id_file"
	printf '%s' "$app_secret_value" >"$app_secret_file"
	jq -n --rawfile app_id "$app_id_file" --rawfile app_secret "$app_secret_file" \
		'{app_id: $app_id, app_secret: $app_secret}' >"$auth_payload"
	api_request "POST" "${api_base}/open-apis/auth/v3/tenant_access_token/internal" "" "$auth_payload"
	emit_evidence "${label}_auth"
	if [[ "$api_code" != "0" ]]; then
		return 1
	fi
	if ! acquired_token="$(jq -r '.tenant_access_token // empty' "$api_body" 2>/dev/null)"; then
		return 1
	fi
	[[ -n "$acquired_token" ]]
}

make_create_payload() {
	local count="$1"
	local marker="$2"
	local payload_file="$3"
	jq -n --arg field "$writable_field" --arg marker "$marker" --argjson count "$count" \
		'{records: [range(0; $count) | {fields: {($field): ($marker + "-" + tostring)}}]}' >"$payload_file"
}

make_update_payload() {
	local count="$1"
	local marker="$2"
	local field="$3"
	local payload_file="$4"
	local ids_file="$tmp_dir/update_${count}_ids.txt"
	local -a selected_ids

	selected_ids=("${primary_created[@]:0:count}")
	printf '%s\n' "${selected_ids[@]}" >"$ids_file"
	jq -R -s --arg field "$field" --arg marker "$marker" \
		'{records: (split("\n") | map(select(length > 0)) | to_entries |
		 map({record_id: .value, fields: {($field): ($marker + "-" + (.key | tostring))}}))}' \
		"$ids_file" >"$payload_file"
}

collect_created_ids() {
	local bucket="$1"
	local expected_count="$2"
	local ids_output
	local record_id
	local observed_count=0

	if [[ "$api_code" != "0" || "$api_records_count" == "N/A" ]]; then
		return 0
	fi
	if ! ids_output="$(jq -r '.data.records[]?.record_id // empty' "$api_body" 2>/dev/null)"; then
		run_failures=$((run_failures + 1))
		return 0
	fi
	while IFS= read -r record_id; do
		if [[ -z "$record_id" ]]; then
			continue
		fi
		observed_count=$((observed_count + 1))
		case "$bucket" in
		primary) primary_created+=("$record_id") ;;
		readonly) readonly_created+=("$record_id") ;;
		noncollab_app) noncollab_app_created+=("$record_id") ;;
		noncollab_user) noncollab_user_created+=("$record_id") ;;
		esac
	done <<<"$ids_output"
	if [[ "$observed_count" -ne "$expected_count" ]]; then
		run_failures=$((run_failures + 1))
	fi
}

run_create_case() {
	local label="$1"
	local token="$2"
	local count="$3"
	local bucket="$4"
	local payload_file="$tmp_dir/${label}.json"

	make_create_payload "$count" "e26-${run_id}-${label}" "$payload_file"
	api_request "POST" "${api_base}/open-apis/bitable/v1/apps/${app_token}/tables/${table_id}/records/batch_create" \
		"$token" "$payload_file"
	emit_evidence "$label"
	collect_created_ids "$bucket" "$count"
}

run_update_case() {
	local label="$1"
	local count="$2"
	local field="$3"
	local token="$4"
	local payload_file="$tmp_dir/${label}.json"

	if [[ "${#primary_created[@]}" -lt "$count" ]]; then
		emit_skipped "$label" "insufficient_provisioned_records"
		return 0
	fi
	make_update_payload "$count" "e26-${run_id}-${label}" "$field" "$payload_file"
	api_request "POST" "${api_base}/open-apis/bitable/v1/apps/${app_token}/tables/${table_id}/records/batch_update" \
		"$token" "$payload_file"
	emit_evidence "$label"
}

if [[ -n "$write_user_access_token" ]]; then
	primary_token="$write_user_access_token"
	emit_skipped "write_identity_auth" "user_access_token_supplied_no_token_exchange"
elif ! acquire_app_token "write" "$write_app_id" "$write_app_secret"; then
	printf 'CASE=setup HTTP_STATUS=NOT_RUN FEISHU_CODE=NOT_RUN MSG=primary_write_identity_unavailable DATA_RECORDS_COUNT=N/A DATA_PAGE_TOKEN=N/A DATA_HAS_MORE=N/A\n'
	exit 1
else
	primary_token="$acquired_token"
fi

# Provision exactly 1001 records in <=500-record requests for update probes.
run_create_case "provision_500_a" "$primary_token" 500 primary
run_create_case "provision_500_b" "$primary_token" 500 primary
run_create_case "provision_1" "$primary_token" 1 primary

# Boundary facts for batch_create. Any successful response is retained for cleanup.
run_create_case "batch_create_500" "$primary_token" 500 primary
run_create_case "batch_create_501" "$primary_token" 501 primary

# Boundary facts for batch_update, using only records created by this run.
run_update_case "batch_update_500" 500 "$writable_field" "$primary_token"
run_update_case "batch_update_501" 501 "$writable_field" "$primary_token"
run_update_case "batch_update_1000" 1000 "$writable_field" "$primary_token"
run_update_case "batch_update_1001" 1001 "$writable_field" "$primary_token"

# Read-only field rejection. This updates one marker row and is cleaned up later.
run_update_case "readonly_field_write" 1 "$readonly_field" "$primary_token"

# Optional app identity with only a read-only scope.
if [[ -n "$readonly_app_id" ]]; then
	if acquire_app_token "readonly_app" "$readonly_app_id" "$readonly_app_secret"; then
		readonly_token="$acquired_token"
		run_create_case "readonly_credential_write" "$readonly_token" 1 readonly
	else
		emit_skipped "readonly_credential_write" "readonly_app_token_unavailable"
	fi
else
	emit_skipped "readonly_credential_write" "optional_readonly_app_identity_not_provided"
fi

# Optional non-collaborator app identity.
if [[ -n "$noncollab_app_id" ]]; then
	if acquire_app_token "noncollab_app" "$noncollab_app_id" "$noncollab_app_secret"; then
		noncollab_app_token="$acquired_token"
		run_create_case "noncollab_app_write" "$noncollab_app_token" 1 noncollab_app
	else
		emit_skipped "noncollab_app_write" "noncollab_app_token_unavailable"
	fi
else
	emit_skipped "noncollab_app_write" "optional_noncollab_app_identity_not_provided"
fi

# Optional non-collaborator user identity.
if [[ -n "$noncollab_user_access_token" ]]; then
	run_create_case "noncollab_user_write" "$noncollab_user_access_token" 1 noncollab_user
else
	emit_skipped "noncollab_user_write" "optional_noncollab_user_identity_not_provided"
fi

exit 0
