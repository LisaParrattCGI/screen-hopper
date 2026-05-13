#!/bin/sh
set -eu

cd "$(dirname "$0")"

mkdir -p .build/module-cache .build/clang-cache

if [ -z "${SDKROOT:-}" ]; then
    for sdk in \
        /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk \
        /Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk \
        "$(xcrun --show-sdk-path --sdk macosx)"
    do
        if [ -d "$sdk" ]; then
            SDKROOT="$sdk"
            export SDKROOT
            break
        fi
    done
fi

CLANG_MODULE_CACHE_PATH="$PWD/.build/clang-cache"
export CLANG_MODULE_CACHE_PATH

swiftc \
    -module-cache-path "$PWD/.build/module-cache" \
    HIDProtocol.swift \
    ScreenHopperLive.swift \
    -framework AppKit \
    -framework CoreGraphics \
    -framework IOKit \
    -framework ServiceManagement \
    -o ScreenHopperLive

app_dir="$PWD/.build/ScreenHopperLive.app"
contents_dir="$app_dir/Contents"

mkdir -p "$contents_dir/MacOS" "$contents_dir/Resources"
cp Info.plist "$contents_dir/Info.plist"
cp ScreenHopperLive "$contents_dir/MacOS/ScreenHopperLive"
chmod 755 "$contents_dir/MacOS/ScreenHopperLive"
