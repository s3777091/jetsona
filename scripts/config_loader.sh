#!/bin/bash
# Shared, dependency-free loader for the project's flat config.yaml and .env.
# Source this file, then call jetson_load_config and jetson_load_secrets.

jetson_trim() {
    local value="$1"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "$value"
}

jetson_unquote() {
    local value
    value="$(jetson_trim "$1")"
    if [ "${#value}" -ge 2 ]; then
        case "$value" in
            \"*\") value="${value:1:${#value}-2}" ;;
            \'*\') value="${value:1:${#value}-2}" ;;
        esac
    fi
    printf '%s' "$value"
}

jetson_is_secret_key() {
    case "$1" in
        *_API_KEY|*_ACCESS_KEY|*_SECRET_KEY|*_PRIVATE_KEY|*_TOKEN|*_PASSWORD|TS_AUTHKEY)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

jetson_export_if_unset() {
    local key="$1" value="$2"
    # Explicit process environment wins over config.yaml and .env.
    if [ "${!key+x}" != x ]; then
        printf -v "$key" '%s' "$value"
        export "$key"
    fi
}

jetson_load_config() {
    local file="$1" raw line key value
    [ -f "$file" ] || {
        echo "config_loader: $file not found; using built-in defaults" >&2
        return 0
    }

    while IFS= read -r raw || [ -n "$raw" ]; do
        line="$(jetson_trim "${raw%$'\r'}")"
        case "$line" in
            ''|'#'*|'---') continue ;;
        esac
        if [[ ! "$line" =~ ^([A-Z][A-Z0-9_]*)[[:space:]]*:[[:space:]]*(.*)$ ]]; then
            echo "config_loader: unsupported config.yaml line: $line" >&2
            return 1
        fi
        key="${BASH_REMATCH[1]}"
        value="${BASH_REMATCH[2]}"
        if jetson_is_secret_key "$key"; then
            echo "config_loader: secret $key must be stored in .env, not config.yaml" >&2
            return 1
        fi
        # Unquoted YAML comments start after whitespace + '#'. Quoted values
        # are kept intact, allowing URLs or labels containing '#'.
        case "$value" in
            \"*\"|\'*\') ;;
            *) value="${value%%[[:space:]]#*}" ;;
        esac
        value="$(jetson_unquote "$value")"
        jetson_export_if_unset "$key" "$value"
    done < "$file"
}

jetson_load_secrets() {
    local file="$1" raw line key value ignored=0
    [ -f "$file" ] || return 0

    while IFS= read -r raw || [ -n "$raw" ]; do
        line="$(jetson_trim "${raw%$'\r'}")"
        case "$line" in
            ''|'#'*) continue ;;
        esac
        if [[ "$line" =~ ^(export[[:space:]]+)?([A-Z][A-Z0-9_]*)[[:space:]]*=(.*)$ ]]; then
            key="${BASH_REMATCH[2]}"
            value="$(jetson_unquote "${BASH_REMATCH[3]}")"
            if jetson_is_secret_key "$key"; then
                jetson_export_if_unset "$key" "$value"
            else
                ignored=$((ignored + 1))
            fi
        else
            echo "config_loader: unsupported .env line (expected KEY=VALUE)" >&2
            return 1
        fi
    done < "$file"

    if [ "$ignored" -gt 0 ]; then
        echo "config_loader: ignored $ignored non-secret .env setting(s); move them to config.yaml" >&2
    fi
}
