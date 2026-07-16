#!/bin/bash
set -eu
root="$(mktemp -d)"
export PS_REMOTE_PLAY_HOME="$root/home"
export PS_REMOTE_PLAY_STATE_FILE="$root/state"
export PS_REMOTE_PLAY_RENDER_BACKEND=opengl
. scripts/ps_remote_play_ctl.sh
mkdir -p "$XDG_CONFIG_HOME/Chiaki"
printf '[settings]\ncurrent_profile=Deck\n\n[registered_hosts]\n1\\server_nickname=Living Room\n1\\rp_regist_key=@ByteArray(12345678\\0)\n' > "$XDG_CONFIG_HOME/Chiaki/Chiaki.conf"
printf '[registered_hosts]\n1\\server_nickname=Living Room\n1\\rp_regist_key=@ByteArray(12345678\\0)\n' > "$XDG_CONFIG_HOME/Chiaki/Chiaki-Deck.conf"
psrp_apply_preset quality
for file in "$XDG_CONFIG_HOME/Chiaki/Chiaki.conf" "$XDG_CONFIG_HOME/Chiaki/Chiaki-Deck.conf"; do
    grep -qx 'resolution_local_ps5=720p' "$file"
    grep -qx 'resolution_remote_ps5=720p' "$file"
    grep -qx 'render_backend=opengl' "$file"
done
printf 'host=192.168.1.2\nnickname=$(touch should-not-exist)\npreset=smooth\npasscode=\n' > "$PSRP_STATE_FILE"
psrp_load_state
test "$PSRP_NICKNAME" = '$(touch should-not-exist)'
test ! -e should-not-exist
printf 'psrp profile/security test: PASS (%s)\n' "$root"
