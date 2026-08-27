#!/bin/bash
# WayLandIE APK build — macOS port of android-app/tools/build-apk.ps1
set -euo pipefail

SDKROOT="${SDKROOT:-/Volumes/ssd/android-sdk}"
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDKROOT="${NDKROOT:-$SDKROOT/ndk-r29}"

BUILDTOOLS="$SDKROOT/build-tools/36.1.0"
ANDROIDJAR="$SDKROOT/platforms/android-36/android.jar"
AAPT2="$BUILDTOOLS/aapt2"
D8="$BUILDTOOLS/d8"
APKSIGNER="$BUILDTOOLS/apksigner"
ZIPALIGN="$BUILDTOOLS/zipalign"
NDKBIN="$NDKROOT/toolchains/llvm/prebuilt/darwin-x86_64/bin"
NDKCLANG="$NDKBIN/aarch64-linux-android33-clang"
NDKSTRIP="$NDKBIN/llvm-strip"

for p in "$AAPT2" "$D8" "$APKSIGNER" "$ZIPALIGN" "$ANDROIDJAR" "$NDKCLANG" "$NDKSTRIP"; do
    [ -e "$p" ] || { echo "missing: $p"; exit 1; }
done

OBJDIR="$PROJECT_ROOT/obj"
GENDIR="$OBJDIR/gen"
CLASSDIR="$OBJDIR/classes"
DEXDIR="$OBJDIR/dex"
FLATDIR="$OBJDIR/flat"
NATIVEBUILDDIR="$OBJDIR/native"
OUTDIR="$PROJECT_ROOT/out"
KEYSTOREDIR="$PROJECT_ROOT/keystore"
UNSIGNED="$OBJDIR/waylandie-display-unsigned.apk"
DEXED="$OBJDIR/waylandie-display-dexed.apk"
ALIGNED="$OBJDIR/waylandie-display-aligned.apk"
SIGNED="$OUTDIR/waylandie-display-mvp.apk"
KEYSTORE="$KEYSTOREDIR/debug.keystore"

rm -rf "$OBJDIR" "$OUTDIR"
mkdir -p "$GENDIR" "$CLASSDIR" "$DEXDIR" "$FLATDIR" "$OUTDIR" "$KEYSTOREDIR"

# --- native lib ---
NATIVEAPKROOT="$NATIVEBUILDDIR/apk"
NATIVELIBDIR="$NATIVEAPKROOT/lib/arm64-v8a"
NATIVEOUT="$NATIVELIBDIR/libwaylandie_display_native.so"
mkdir -p "$NATIVELIBDIR"
"$NDKCLANG" -shared -fPIC -O2 -Wall -Wextra \
    -o "$NATIVEOUT" "$PROJECT_ROOT/native/waylandie_display_native.c" -landroid
"$NDKSTRIP" --strip-unneeded "$NATIVEOUT"
echo "built native: $NATIVEOUT"

# Bundle AdrenoTools runtime libraries if present (see third_party/adrenotools)
ADRENOTOOLS_PREBUILT="$PROJECT_ROOT/deps/adrenotools/built-arm64"
if [ -d "$ADRENOTOOLS_PREBUILT" ]; then
    for lib in "$ADRENOTOOLS_PREBUILT"/*.so; do
        cp "$lib" "$NATIVELIBDIR/"
    done
    echo "Bundled AdrenoTools libraries from $ADRENOTOOLS_PREBUILT"
fi

# --- resources ---
"$AAPT2" compile --dir "$PROJECT_ROOT/res" -o "$FLATDIR"
FLATFILES=$(find "$FLATDIR" -name '*.flat')
"$AAPT2" link \
    -o "$UNSIGNED" \
    -I "$ANDROIDJAR" \
    --manifest "$PROJECT_ROOT/AndroidManifest.xml" \
    --java "$GENDIR" \
    --min-sdk-version 33 \
    --target-sdk-version 36 \
    --version-code 1 \
    --version-name "0.1.0" \
    $FLATFILES

# --- java ---
SOURCEFILES=$(find "$PROJECT_ROOT/src" "$GENDIR" -name '*.java')
javac -encoding UTF-8 -source 17 -target 17 \
    -classpath "$ANDROIDJAR" \
    -d "$CLASSDIR" \
    $SOURCEFILES

CLASSFILES=$(find "$CLASSDIR" -name '*.class')
"$D8" --min-api 33 --output "$DEXDIR" $CLASSFILES

# --- assemble ---
cp "$UNSIGNED" "$DEXED"
(cd "$DEXDIR" && jar uf "$DEXED" .)
(cd "$NATIVEAPKROOT" && jar uf "$DEXED" .)
"$ZIPALIGN" -f -p 4 "$DEXED" "$ALIGNED"

# --- sign ---
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkeypair \
        -keystore "$KEYSTORE" \
        -storepass android -keypass android \
        -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US"
fi
"$APKSIGNER" sign \
    --ks "$KEYSTORE" \
    --ks-pass pass:android --key-pass pass:android \
    --out "$SIGNED" "$ALIGNED"
"$APKSIGNER" verify --verbose "$SIGNED" | head -4

echo "BUILT: $SIGNED"
