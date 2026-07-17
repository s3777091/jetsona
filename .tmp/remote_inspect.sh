#!/bin/bash
set -u
cd /home/ekkohuynh/jetsona || exit 1

echo "--- STATUS ---"
git status --porcelain=v1

echo "--- ROOT-OWNED GIT FILES ---"
find .git ! -user ekkohuynh -printf '%u:%g %m %p\n' 2>/dev/null | head -80

echo "--- ENV KEYS ---"
for envfile in .env /opt/jetson-fw/.env; do
    echo "[$envfile]"
    if [ -r "$envfile" ]; then
        sed -nE 's/^[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)=(.*)$/\1/p' "$envfile"
    else
        echo UNREADABLE_OR_MISSING
    fi
done

echo "--- REPO ENV SET STATE ---"
if [ -f .env ]; then
    while IFS= read -r key; do
        value="$(sed -nE "s/^[[:space:]]*${key}=(.*)$/\1/p" .env | tail -1)"
        if [ -n "$value" ]; then
            echo "$key=set"
        else
            echo "$key=blank"
        fi
    done < <(sed -nE 's/^[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)=.*/\1/p' .env)
fi

echo "--- SUDO ---"
if sudo -n true 2>/dev/null; then
    echo PASSWORDLESS
else
    echo PASSWORD_REQUIRED
fi
