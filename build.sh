#!/usr/bin/env bash
set -euo pipefail

cd /Users/huicat28/Projects/Engine/godot-for-arcadia-archive

JOBS=12
BUILD_TEMPLATES=0

while getopts "tj:" opt; do
  case "$opt" in
    t)
      BUILD_TEMPLATES=1
      ;;
    j)
      JOBS="$OPTARG"
      ;;
    *)
      echo "用法:"
      echo "  ./build.sh        清理后编译 macOS editor"
      echo "  ./build.sh -t     清理后编译全部 export templates"
      echo "  ./build.sh -j 8   指定并行数量"
      echo "  ./build.sh -t -j 8"
      exit 1
      ;;
  esac
done

MACOS_EDITOR_BIN="./bin/godot.macos.editor.arm64.moye.mono"
MACOS_EDITOR_APP="./bin/godot_macos_editor_moye_mono.app"

run_scons() {
  echo
  echo ">>> scons $* -j$JOBS"
  scons "$@" -j"$JOBS"
}

fix_quarantine() {
  local path="$1"

  if [[ -e "$path" ]]; then
    echo ">>> 移除 quarantine: $path"
    xattr -dr com.apple.quarantine "$path" 2>/dev/null || true
  fi
}

check_exists() {
  local path="$1"
  local name="$2"

  if [[ ! -e "$path" ]]; then
    echo "❌ 缺少产物: $name"
    echo "路径: $path"
    exit 1
  fi

  echo "✅ $name: $path"
}

clean_editor_targets() {
  echo ">>> 清理 macOS editor 产物"

  rm -rf bin/GodotSharp
  rm -rf "$MACOS_EDITOR_APP"
  rm -f "$MACOS_EDITOR_BIN"

  echo ">>> editor 清理完成"
}

clean_template_targets() {
  echo ">>> 清理 export template 产物"

  # Android
  rm -f bin/libgodot.android.template_release.arm32.moye.mono.so
  rm -f bin/libgodot.android.template_release.arm64.moye.mono.so
  rm -f bin/libgodot.android.template_release.x86_32.moye.mono.so
  rm -f bin/libgodot.android.template_release.x86_64.moye.mono.so

  rm -f bin/android_source.zip
  rm -f bin/android_debug.apk
  rm -f bin/android_release.apk
  rm -f bin/android_template.apk

  # Windows
  rm -f bin/godot.windows.template_debug.x86_32.moye.mono.exe
  rm -f bin/godot.windows.template_release.x86_32.moye.mono.exe
  rm -f bin/godot.windows.template_debug.x86_64.moye.mono.exe
  rm -f bin/godot.windows.template_release.x86_64.moye.mono.exe
  rm -f bin/godot.windows.template_debug.arm64.moye.mono.exe
  rm -f bin/godot.windows.template_release.arm64.moye.mono.exe

  # macOS
  rm -f bin/godot.macos.template_debug.arm64.moye.mono
  rm -f bin/godot.macos.template_release.arm64.moye.mono
  rm -f bin/godot.macos.template_debug.x86_64.moye.mono
  rm -f bin/godot.macos.template_release.x86_64.moye.mono

  rm -rf bin/godot_macos_template_debug_moye_mono.app
  rm -rf bin/godot_macos_template_release_moye_mono.app
  rm -rf bin/macos_template.app
  rm -f bin/macos.zip
  rm -f bin/macos_template.zip

  # iOS
  rm -f bin/libgodot.ios.template_debug.arm64.moye.mono.a
  rm -f bin/libgodot.ios.template_release.arm64.moye.mono.a

  rm -rf bin/godot_ios_template_debug_moye_mono
  rm -rf bin/godot_ios_template_release_moye_mono
  rm -rf bin/ios_template
  rm -rf bin/ios_template_debug
  rm -rf bin/ios_template_release

  rm -f bin/ios.zip
  rm -f bin/ios_template.zip
  rm -f bin/ios_template_debug.zip
  rm -f bin/ios_template_release.zip

  echo ">>> export template 清理完成"
}

build_macos_editor() {
  echo
  echo "========== 构建 macOS editor =========="

  clean_editor_targets

  run_scons \
    platform=macos \
    arch=arm64 \
    target=editor \
    module_mono_enabled=yes

  check_exists "$MACOS_EDITOR_BIN" "macOS editor binary"

  echo
  echo ">>> 生成 mono glue"
  "$MACOS_EDITOR_BIN" --headless --generate-mono-glue modules/mono/glue

  echo
  echo ">>> 构建 GodotSharp managed assemblies"
  ./modules/mono/build_scripts/build_assemblies.py \
    --godot-output-dir=./bin \
    --godot-platform=macos

  run_scons \
    platform=macos \
    arch=arm64 \
    target=editor \
    module_mono_enabled=yes

  run_scons \
    platform=macos \
    arch=arm64 \
    target=editor \
    module_mono_enabled=yes \
    generate_bundle=yes

  check_exists "$MACOS_EDITOR_BIN" "macOS editor binary"
  check_exists "bin/GodotSharp" "GodotSharp"

  if [[ ! -d "$MACOS_EDITOR_APP" ]]; then
    echo "⚠️ 没找到预期的 macOS editor app:"
    echo "路径: $MACOS_EDITOR_APP"
    echo
    echo "当前 .app 列表："
    find bin -maxdepth 1 -type d -name "*.app" -print || true
    echo
    exit 1
  fi

  check_exists "$MACOS_EDITOR_APP" "macOS editor app"

  fix_quarantine "$MACOS_EDITOR_BIN"
  fix_quarantine "$MACOS_EDITOR_APP"
}

build_android_templates() {
  echo
  echo "========== 构建 Android templates =========="

  run_scons \
    platform=android \
    target=template_release \
    arch=arm32 \
    module_mono_enabled=yes

  run_scons \
    platform=android \
    target=template_release \
    arch=arm64 \
    module_mono_enabled=yes

  run_scons \
    platform=android \
    target=template_release \
    arch=x86_32 \
    module_mono_enabled=yes

  run_scons \
    platform=android \
    target=template_release \
    arch=x86_64 \
    module_mono_enabled=yes \
    generate_android_binaries=yes

  check_exists "bin/libgodot.android.template_release.arm32.moye.mono.so" "Android template_release arm32"
  check_exists "bin/libgodot.android.template_release.arm64.moye.mono.so" "Android template_release arm64"
  check_exists "bin/libgodot.android.template_release.x86_32.moye.mono.so" "Android template_release x86_32"
  check_exists "bin/libgodot.android.template_release.x86_64.moye.mono.so" "Android template_release x86_64"
}

build_windows_templates() {
  echo
  echo "========== 构建 Windows templates =========="

  run_scons \
    platform=windows \
    target=template_debug \
    arch=x86_32 \
    module_mono_enabled=yes

  run_scons \
    platform=windows \
    target=template_release \
    arch=x86_32 \
    module_mono_enabled=yes

  run_scons \
    platform=windows \
    target=template_debug \
    arch=x86_64 \
    module_mono_enabled=yes

  run_scons \
    platform=windows \
    target=template_release \
    arch=x86_64 \
    module_mono_enabled=yes

  run_scons \
    platform=windows \
    target=template_debug \
    arch=arm64 \
    module_mono_enabled=yes

  run_scons \
    platform=windows \
    target=template_release \
    arch=arm64 \
    module_mono_enabled=yes

  check_exists "bin/godot.windows.template_debug.x86_32.moye.mono.exe" "Windows template_debug x86_32"
  check_exists "bin/godot.windows.template_release.x86_32.moye.mono.exe" "Windows template_release x86_32"
  check_exists "bin/godot.windows.template_debug.x86_64.moye.mono.exe" "Windows template_debug x86_64"
  check_exists "bin/godot.windows.template_release.x86_64.moye.mono.exe" "Windows template_release x86_64"
  check_exists "bin/godot.windows.template_debug.arm64.moye.mono.exe" "Windows template_debug arm64"
  check_exists "bin/godot.windows.template_release.arm64.moye.mono.exe" "Windows template_release arm64"
}

build_macos_templates() {
  echo
  echo "========== 构建 macOS templates =========="

  run_scons \
    platform=macos \
    target=template_debug \
    arch=arm64 \
    module_mono_enabled=yes

  run_scons \
    platform=macos \
    target=template_release \
    arch=arm64 \
    module_mono_enabled=yes

  run_scons \
    platform=macos \
    target=template_debug \
    arch=x86_64 \
    module_mono_enabled=yes

  run_scons \
    platform=macos \
    target=template_release \
    arch=x86_64 \
    module_mono_enabled=yes \
    generate_bundle=yes

  check_exists "bin/godot.macos.template_debug.arm64.moye.mono" "macOS template_debug arm64"
  check_exists "bin/godot.macos.template_release.arm64.moye.mono" "macOS template_release arm64"
  check_exists "bin/godot.macos.template_debug.x86_64.moye.mono" "macOS template_debug x86_64"
  check_exists "bin/godot.macos.template_release.x86_64.moye.mono" "macOS template_release x86_64"

  fix_quarantine "bin/godot.macos.template_debug.arm64.moye.mono"
  fix_quarantine "bin/godot.macos.template_release.arm64.moye.mono"
  fix_quarantine "bin/godot.macos.template_debug.x86_64.moye.mono"
  fix_quarantine "bin/godot.macos.template_release.x86_64.moye.mono"

  if [[ -d "bin/godot_macos_template_release_moye_mono.app" ]]; then
    fix_quarantine "bin/godot_macos_template_release_moye_mono.app"
  fi
}

build_ios_templates() {
  echo
  echo "========== 构建 iOS templates =========="

  run_scons \
    platform=ios \
    target=template_debug \
    module_mono_enabled=yes \
    generate_bundle=yes

  run_scons \
    platform=ios \
    target=template_release \
    module_mono_enabled=yes \
    generate_bundle=yes

  check_exists "bin/libgodot.ios.template_debug.arm64.moye.mono.a" "iOS template_debug arm64 static library"
  check_exists "bin/libgodot.ios.template_release.arm64.moye.mono.a" "iOS template_release arm64 static library"

  if [[ -d "bin/godot_ios_template_debug_moye_mono" ]]; then
    fix_quarantine "bin/godot_ios_template_debug_moye_mono"
  fi

  if [[ -d "bin/godot_ios_template_release_moye_mono" ]]; then
    fix_quarantine "bin/godot_ios_template_release_moye_mono"
  fi

  if [[ -f "bin/ios.zip" ]]; then
    echo "✅ iOS bundle zip: bin/ios.zip"
  fi

  if [[ -f "bin/ios_template.zip" ]]; then
    echo "✅ iOS template zip: bin/ios_template.zip"
  fi
}

build_all_templates() {
  echo
  echo "========== 构建全部 export templates =========="

  clean_template_targets

  build_android_templates
  build_windows_templates
  build_macos_templates
  build_ios_templates
}

if [[ "$BUILD_TEMPLATES" -eq 1 ]]; then
  build_all_templates
else
  build_macos_editor
fi

echo
echo "========== 构建完成 =========="
ls -la bin