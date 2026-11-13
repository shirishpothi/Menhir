#!/bin/sh
# Publish the debug-signed APK after building, committing and pushing.
set -eu
cd "$(dirname "$0")/.."
apk=android/app/build/outputs/apk/debug/menhir-debug.apk
name=$(sed -n 's/.*versionName = "\(.*\)"/\1/p' android/app/build.gradle.kts)
code=$(sed -n 's/.*versionCode = \([0-9]*\)/\1/p' android/app/build.gradle.kts)
if [ -n "$(git status --porcelain --untracked-files=normal)" ] || [ "$(git branch --show-current)" != main ] || [ "$(git rev-parse HEAD)" != "$(git rev-parse origin/main)" ]; then
    echo "Commit and push first, so the release matches main." >&2
    exit 1
fi
[ -n "$name" ] && [ -n "$code" ] && [ -f "$apk" ]
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cp "$apk" "$dir/menhir-$code.apk"
gh release create "v$name" "$dir/menhir-$code.apk" --repo shirishpothi/Menhir --title "$name" --notes "Menhir $name" --target "$(git rev-parse HEAD)" --latest
topic=$(sed -n 's/.*PUSH_TOPIC = "\(.*\)"/\1/p' android/app/src/main/java/app/menhir/update/Updater.kt)
if curl -fsS -m 15 -o /dev/null -d "$code" "https://ntfy.sh/$topic"; then
    echo "Announced $name to the tablet."
else
    echo "Announcement failed; the tablet will check within 5 minutes." >&2
fi
