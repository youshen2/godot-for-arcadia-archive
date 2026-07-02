#!/usr/bin/env bash
set -euo pipefail

export SCRIPT_AES256_ENCRYPTION_KEY="85534143e979103227f1dae12835b174c780ab7e2b73127c10763b0b25880c52"

cd /Users/huicat28/Projects/Engine/godot-for-arcadia-archive

JOBS=12
BUILD_TEMPLATES=0
BUILD_EXIT_CODE=0
ERROR_LOG="./error.log"
CUSTOM_BUILD_OPTIONS="./custom.py"
TEMPLATE_OUTPUT_DIR="./bin/export_templates"
TEMPLATE_FAILURE_LABELS=()
TEMPLATE_FAILURE_LOGS=()
WINDOWS_ACTIVE_MINGW_PREFIX=""

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
      echo "  WINDOWS_LLVM_MINGW_PREFIX=/path/to/llvm-mingw ./build.sh -t"
      exit 1
      ;;
  esac
done

MACOS_EDITOR_BIN="./bin/godot.macos.editor.arm64.moye.mono"
MACOS_EDITOR_APP="./bin/godot_macos_editor_moye_mono.app"

format_command() {
  local command=""

  printf -v command "%q " "$@"
  echo "${command% }"
}

append_error_log_to() {
  local error_log="$1"
  local title="$2"

  mkdir -p "$(dirname "$error_log")"

  {
    echo
    echo "========== $title =========="
    echo "时间: $(date '+%Y-%m-%d %H:%M:%S')"
    shift 2
    printf '%s\n' "$@"
  } >> "$error_log"
}

append_error_log() {
  append_error_log_to "$ERROR_LOG" "$@"
}

clear_error_log() {
  local error_log="$1"

  mkdir -p "$(dirname "$error_log")"
  rm -f "$error_log"
}

append_command_error_log_to() {
  local error_log="$1"
  local command="$2"
  local status="$3"
  local output_file="$4"

  mkdir -p "$(dirname "$error_log")"

  {
    echo
    echo "========== 命令执行失败 =========="
    echo "时间: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "命令: $command"
    echo "退出码: $status"
    echo "输出:"
    LC_ALL=C tr -d '\004\010' < "$output_file" | sed '1s/^\^D//'
  } >> "$error_log"
}

run_logged_to_log() {
  local error_log="$1"
  local command
  local restore_errexit=0
  local status
  local temp_log

  shift
  command="$(format_command "$@")"
  temp_log="$(mktemp "${TMPDIR:-/tmp}/godot-build.XXXXXX")"

  clear_error_log "$error_log"

  echo
  echo ">>> $command"

  case "$-" in
    *e*)
      restore_errexit=1
      ;;
  esac

  set +e
  if [[ -t 1 ]] && command -v script >/dev/null 2>&1; then
    script -q -F "$temp_log" "$@"
    status=$?
  else
    "$@" 2>&1 | tee "$temp_log"
    status=${PIPESTATUS[0]}
  fi

  if [[ "$restore_errexit" -eq 1 ]]; then
    set -e
  else
    set +e
  fi

  if [[ "$status" -ne 0 ]]; then
    append_command_error_log_to "$error_log" "$command" "$status" "$temp_log"
    rm -f "$temp_log"
    echo
    echo "❌ 构建失败，详情已写入: $error_log"
    return "$status"
  fi

  rm -f "$temp_log"
}

run_logged() {
  run_logged_to_log "$ERROR_LOG" "$@"
}

run_scons() {
  run_logged scons "$@" -j"$JOBS"
}

run_scons_to_log() {
  local error_log="$1"

  shift
  run_logged_to_log "$error_log" scons "$@" -j"$JOBS"
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
    append_error_log \
      "缺少构建产物" \
      "产物: $name" \
      "路径: $path"
    exit 1
  fi

  echo "✅ $name: $path"
}

configure_custom_build_options() {
  local mode="$1"
  local temp_file

  case "$mode" in
    editor|template)
      ;;
    *)
      echo "❌ 未知 custom.py 构建配置模式: $mode"
      exit 1
      ;;
  esac

  temp_file="${CUSTOM_BUILD_OPTIONS}.tmp.$$"
  cp "$CUSTOM_BUILD_OPTIONS" "$temp_file"

  if ! awk -v mode="$mode" '
function comment_line(line, indent, rest) {
	if (line ~ /^[[:space:]]*($|#)/) {
		return line;
	}

	match(line, /^[[:space:]]*/);
	indent = substr(line, 1, RLENGTH);
	rest = substr(line, RLENGTH + 1);
	return indent "# " rest;
}

function uncomment_line(line, indent, rest) {
	if (line !~ /^[[:space:]]*#/) {
		return line;
	}

	match(line, /^[[:space:]]*/);
	indent = substr(line, 1, RLENGTH);
	rest = substr(line, RLENGTH + 1);
	sub(/^#[[:space:]]?/, "", rest);
	return indent rest;
}

function apply_block(line) {
	if (block == "T") {
		return mode == "template" ? uncomment_line(line) : comment_line(line);
	}
	if (block == "E") {
		return mode == "editor" ? uncomment_line(line) : comment_line(line);
	}
	return line;
}

/^[[:space:]]*# T-Begin[[:space:]]*$/ {
	seen_t_begin = 1;
	block = "T";
	print;
	next;
}

/^[[:space:]]*# T-End[[:space:]]*$/ {
	seen_t_end = 1;
	block = "";
	print;
	next;
}

/^[[:space:]]*# E-Begin[[:space:]]*$/ {
	seen_e_begin = 1;
	block = "E";
	print;
	next;
}

/^[[:space:]]*# E-End[[:space:]]*$/ {
	seen_e_end = 1;
	block = "";
	print;
	next;
}

{
	print apply_block($0);
}

END {
	if (block != "" || !seen_t_begin || !seen_t_end || !seen_e_begin || !seen_e_end) {
		exit 1;
	}
}
' "$CUSTOM_BUILD_OPTIONS" > "$temp_file"; then
    rm -f "$temp_file"
    echo "❌ 切换 custom.py 构建配置失败，请检查 # T-Begin/# T-End 和 # E-Begin/# E-End 标记"
    exit 1
  fi

  mv "$temp_file" "$CUSTOM_BUILD_OPTIONS"
  echo ">>> custom.py 已切换为 $mode 构建配置"
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

  rm -rf "$TEMPLATE_OUTPUT_DIR"

  # Android
  rm -f bin/libgodot.android.template_*.moye.so
  rm -f bin/libgodot.android.template_*.moye.mono.so

  rm -f bin/android_source.zip
  rm -f bin/android_debug.apk
  rm -f bin/android_release.apk
  rm -f bin/android_template.apk
  rm -f bin/android_monoDebug.apk
  rm -f bin/android_monoRelease.apk
  rm -f bin/godot-lib.template_debug.aar
  rm -f bin/godot-lib.template_release.aar

  # Windows
  rm -f bin/godot.windows.template_debug.x86_32.moye.mono.exe
  rm -f bin/godot.windows.template_debug.x86_32.moye.mono.console.exe
  rm -f bin/godot.windows.template_release.x86_32.moye.mono.exe
  rm -f bin/godot.windows.template_release.x86_32.moye.mono.console.exe
  rm -f bin/godot.windows.template_debug.x86_64.moye.mono.exe
  rm -f bin/godot.windows.template_debug.x86_64.moye.mono.console.exe
  rm -f bin/godot.windows.template_release.x86_64.moye.mono.exe
  rm -f bin/godot.windows.template_release.x86_64.moye.mono.console.exe
  rm -f bin/godot.windows.template_debug.arm64.moye.mono.exe
  rm -f bin/godot.windows.template_debug.arm64.moye.mono.console.exe
  rm -f bin/godot.windows.template_release.arm64.moye.mono.exe
  rm -f bin/godot.windows.template_release.arm64.moye.mono.console.exe
  rm -f bin/godot.windows.template_*.llvm.moye.mono.exe
  rm -f bin/godot.windows.template_*.llvm.moye.mono.console.exe

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
  rm -f bin/godot_macos_moye_mono.zip

  # iOS
  rm -f bin/libgodot.ios.template_debug.arm64.moye.a
  rm -f bin/libgodot.ios.template_release.arm64.moye.a
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
  rm -f bin/godot_ios_moye.zip

  echo ">>> export template 清理完成"
}

template_target_dir() {
  local platform="$1"
  local target_name="$2"

  echo "$TEMPLATE_OUTPUT_DIR/$platform/$target_name"
}

platform_template_output_dir() {
  local platform="$1"

  echo "$TEMPLATE_OUTPUT_DIR/$platform"
}

android_template_output_dir() {
  platform_template_output_dir android
}

windows_template_output_dir() {
  platform_template_output_dir windows
}

macos_template_output_dir() {
  platform_template_output_dir macos
}

ios_template_output_dir() {
  platform_template_output_dir ios
}

windows_llvm_mingw_prefix_is_valid() {
  local prefix="$1"

  [[ -x "$prefix/bin/i686-w64-mingw32-clang" ]] &&
    [[ -x "$prefix/bin/x86_64-w64-mingw32-clang" ]] &&
    [[ -x "$prefix/bin/aarch64-w64-mingw32-clang" ]]
}

resolve_windows_llvm_mingw_prefix() {
  local default_prefix="$HOME/Toolchains/llvm-mingw"

  if [[ -n "${WINDOWS_LLVM_MINGW_PREFIX:-}" ]]; then
    echo "$WINDOWS_LLVM_MINGW_PREFIX"
    return 0
  fi

  if [[ -n "${MINGW_PREFIX:-}" ]]; then
    if windows_llvm_mingw_prefix_is_valid "$MINGW_PREFIX"; then
      echo "$MINGW_PREFIX"
      return 0
    fi

    if windows_llvm_mingw_prefix_is_valid "$default_prefix"; then
      echo "⚠️ 当前 MINGW_PREFIX 不是完整 llvm-mingw，改用: $default_prefix" >&2
      echo "$default_prefix"
      return 0
    fi

    echo "$MINGW_PREFIX"
    return 0
  fi

  echo "$default_prefix"
}

configure_windows_llvm_mingw() {
  WINDOWS_ACTIVE_MINGW_PREFIX="$(resolve_windows_llvm_mingw_prefix)"

  if ! windows_llvm_mingw_prefix_is_valid "$WINDOWS_ACTIVE_MINGW_PREFIX"; then
    echo "❌ Windows llvm-mingw 工具链不可用: $WINDOWS_ACTIVE_MINGW_PREFIX"
    echo "需要包含以下编译器:"
    echo "  $WINDOWS_ACTIVE_MINGW_PREFIX/bin/i686-w64-mingw32-clang"
    echo "  $WINDOWS_ACTIVE_MINGW_PREFIX/bin/x86_64-w64-mingw32-clang"
    echo "  $WINDOWS_ACTIVE_MINGW_PREFIX/bin/aarch64-w64-mingw32-clang"
    echo
    echo "可通过 WINDOWS_LLVM_MINGW_PREFIX=/path/to/llvm-mingw ./build.sh -t 指定路径"
    return 1
  fi

  echo ">>> Windows llvm-mingw: $WINDOWS_ACTIVE_MINGW_PREFIX"
  echo ">>> Windows 构建仅临时设置 MINGW_PREFIX/PATH，SCons 选项保持脚本原样"
}

record_windows_toolchain_failure() {
  local error_log
  local label

  error_log="$(windows_template_output_dir)/error.log"
  mkdir -p "$(windows_template_output_dir)"

  append_error_log_to \
    "$error_log" \
    "Windows llvm-mingw 环境缺失" \
    "工具链路径: $WINDOWS_ACTIVE_MINGW_PREFIX" \
    "需要完整 llvm-mingw，并包含 i686/x86_64/aarch64 的 clang 目标编译器。" \
    "可通过 WINDOWS_LLVM_MINGW_PREFIX=/path/to/llvm-mingw ./build.sh -t 指定路径。"

  for label in \
    "Windows template_debug x86_32" \
    "Windows template_release x86_32" \
    "Windows template_debug x86_64" \
    "Windows template_release x86_64" \
    "Windows template_debug arm64" \
    "Windows template_release arm64"; do
    record_template_failure "$label" "$error_log"
  done
}

record_template_failure() {
  local label="$1"
  local error_log="$2"
  local index

  for index in "${!TEMPLATE_FAILURE_LABELS[@]}"; do
    if [[ "${TEMPLATE_FAILURE_LABELS[$index]}" == "$label" ]]; then
      return 0
    fi
  done

  TEMPLATE_FAILURE_LABELS+=("$label")
  TEMPLATE_FAILURE_LOGS+=("$error_log")
}

run_template_scons() {
  local platform="$1"
  local target_name="$2"
  local label="$3"
  local target_dir
  local error_log

  shift 3
  target_dir="$(template_target_dir "$platform" "$target_name")"
  error_log="$target_dir/error.log"

  mkdir -p "$target_dir"

  echo
  echo "========== 构建 $label =========="

  if ! run_scons_to_log "$error_log" "$@"; then
    record_template_failure "$label" "$error_log"
  fi
}

run_android_template_scons() {
  local label="$1"
  local error_log
  local temp_error_log

  shift
  error_log="$(android_template_output_dir)/error.log"
  temp_error_log="$(mktemp "${TMPDIR:-/tmp}/godot-android-build.XXXXXX")"

  mkdir -p "$(android_template_output_dir)"

  echo
  echo "========== 构建 $label =========="

  if ! run_scons_to_log "$temp_error_log" "$@"; then
    {
      echo
      echo "========== $label =========="
      cat "$temp_error_log"
    } >> "$error_log"
    record_template_failure "$label" "$error_log"
  fi

  rm -f "$temp_error_log"
}

run_windows_template_scons() {
  local label="$1"
  local error_log
  local temp_error_log

  shift
  error_log="$(windows_template_output_dir)/error.log"
  temp_error_log="$(mktemp "${TMPDIR:-/tmp}/godot-windows-build.XXXXXX")"

  mkdir -p "$(windows_template_output_dir)"

  echo
  echo "========== 构建 $label =========="

  if ! (
    export MINGW_PREFIX="$WINDOWS_ACTIVE_MINGW_PREFIX"
    export PATH="$WINDOWS_ACTIVE_MINGW_PREFIX/bin:$PATH"

    run_scons_to_log "$temp_error_log" "$@"
  ); then
    {
      echo
      echo "========== $label =========="
      cat "$temp_error_log"
    } >> "$error_log"
    record_template_failure "$label" "$error_log"
  fi

  rm -f "$temp_error_log"
}

check_template_output() {
  local platform="$1"
  local target_name="$2"
  local label="$3"
  local path="$4"
  local name="$5"
  local target_dir
  local error_log

  target_dir="$(template_target_dir "$platform" "$target_name")"
  error_log="$target_dir/error.log"

  if [[ ! -e "$path" ]]; then
    echo "❌ 缺少产物: $name"
    echo "路径: $path"
    append_error_log_to \
      "$error_log" \
      "缺少构建产物" \
      "目标: $label" \
      "产物: $name" \
      "路径: $path"
    record_template_failure "$label" "$error_log"
    return 0
  fi

  echo "✅ $name: $path"
}

check_template_output_candidates_to_log() {
  local error_log="$1"
  local label="$2"
  local name="$3"
  local path

  shift 3

  for path in "$@"; do
    if [[ -e "$path" ]]; then
      echo "✅ $name: $path"
      return 0
    fi
  done

  echo "❌ 缺少产物: $name"
  echo "候选路径: $*"
  append_error_log_to \
    "$error_log" \
    "缺少构建产物" \
    "目标: $label" \
    "产物: $name" \
    "候选路径: $*"
  record_template_failure "$label" "$error_log"
}

check_android_template_output() {
  local label="$1"
  local path="$2"
  local name="$3"
  local target_dir
  local error_log

  target_dir="$(android_template_output_dir)"
  error_log="$target_dir/error.log"

  if [[ ! -e "$path" ]]; then
    echo "❌ 缺少产物: $name"
    echo "路径: $path"
    append_error_log_to \
      "$error_log" \
      "缺少构建产物" \
      "目标: $label" \
      "产物: $name" \
      "路径: $path"
    record_template_failure "$label" "$error_log"
    return 0
  fi

  echo "✅ $name: $path"
}

check_android_template_output_any() {
  local label="$1"
  local name="$2"
  local error_log

  shift 2
  error_log="$(android_template_output_dir)/error.log"

  check_template_output_candidates_to_log "$error_log" "$label" "$name" "$@"
}

check_windows_template_output() {
  local label="$1"
  local path="$2"
  local name="$3"
  local target_dir
  local error_log

  target_dir="$(windows_template_output_dir)"
  error_log="$target_dir/error.log"

  if [[ ! -e "$path" ]]; then
    echo "❌ 缺少产物: $name"
    echo "路径: $path"
    append_error_log_to \
      "$error_log" \
      "缺少构建产物" \
      "目标: $label" \
      "产物: $name" \
      "路径: $path"
    record_template_failure "$label" "$error_log"
    return 0
  fi

  echo "✅ $name: $path"
}

check_windows_template_output_any() {
  local label="$1"
  local name="$2"
  local error_log

  shift 2
  error_log="$(windows_template_output_dir)/error.log"

  check_template_output_candidates_to_log "$error_log" "$label" "$name" "$@"
}

check_platform_template_output_any() {
  local platform="$1"
  local label="$2"
  local name="$3"
  local error_log

  shift 3
  error_log="$(platform_template_output_dir "$platform")/error.log"
  mkdir -p "$(platform_template_output_dir "$platform")"

  check_template_output_candidates_to_log "$error_log" "$label" "$name" "$@"
}

summarize_template_failures() {
  local index

  if [[ "${#TEMPLATE_FAILURE_LABELS[@]}" -eq 0 ]]; then
    echo
    echo "========== export templates 全部构建成功 =========="
    return 0
  fi

  BUILD_EXIT_CODE=1

  echo
  echo "========== export templates 构建完成，但存在失败目标 =========="

  for index in "${!TEMPLATE_FAILURE_LABELS[@]}"; do
    echo "❌ ${TEMPLATE_FAILURE_LABELS[$index]}"
    echo "   日志: ${TEMPLATE_FAILURE_LOGS[$index]}"
  done
}

copy_template_output() {
  local path="$1"
  local target_dir="$2"

  if [[ -e "$path" ]]; then
    mkdir -p "$target_dir"
    mv "$path" "$target_dir"/
    echo "✅ 归档 template: $path -> $target_dir/"
  fi
}

copy_template_output_as() {
  local path="$1"
  local target_dir="$2"
  local target_name="$3"

  if [[ -e "$path" ]]; then
    mkdir -p "$target_dir"
    mv "$path" "$target_dir/$target_name"
    echo "✅ 归档 template: $path -> $target_dir/$target_name"
  fi
}

copy_template_output_first_as() {
  local target_dir="$1"
  local target_name="$2"
  local path

  shift 2

  for path in "$@"; do
    if [[ -e "$path" ]]; then
      mkdir -p "$target_dir"
      mv "$path" "$target_dir/$target_name"
      echo "✅ 归档 template: $path -> $target_dir/$target_name"
      return 0
    fi
  done
}

copy_template_file_as() {
  local path="$1"
  local target_dir="$2"
  local target_name="$3"

  if [[ -e "$path" ]]; then
    mkdir -p "$target_dir"
    cp -p "$path" "$target_dir/$target_name"
    echo "✅ 归档 template: $path -> $target_dir/$target_name"
  fi
}

cleanup_template_intermediates() {
  if [[ "${#TEMPLATE_FAILURE_LABELS[@]}" -ne 0 ]]; then
    echo
    echo ">>> 存在失败目标，保留 bin/ 中间产物用于排错和增量重试"
    return 0
  fi

  echo
  echo "========== 清理 export template 中间产物 =========="

  rm -f bin/libgodot.android.template_*.moye.so
  rm -f bin/libgodot.android.template_*.moye.mono.so
  rm -f bin/android_debug.apk
  rm -f bin/android_release.apk
  rm -f bin/android_monoDebug.apk
  rm -f bin/android_monoRelease.apk
  rm -f bin/android_source.zip
  rm -f bin/godot-lib.template_debug.aar
  rm -f bin/godot-lib.template_release.aar

  rm -f bin/godot.windows.template_*.moye.mono*.exe
  rm -f bin/godot.windows.template_*.llvm.moye.mono*.exe

  rm -f bin/godot.macos.template_*.moye.mono
  rm -rf bin/godot_macos_template_debug_moye_mono.app
  rm -rf bin/godot_macos_template_release_moye_mono.app
  rm -rf bin/macos_template.app
  rm -f bin/godot_macos_moye_mono.zip
  rm -f bin/macos.zip
  rm -f bin/macos_template.zip

  rm -f bin/libgodot.ios.template_*.moye.a
  rm -f bin/libgodot.ios.template_*.moye.mono.a
  rm -rf bin/godot_ios_template_debug_moye_mono
  rm -rf bin/godot_ios_template_release_moye_mono
  rm -rf bin/ios_template
  rm -rf bin/ios_template_debug
  rm -rf bin/ios_template_release
  rm -f bin/godot_ios_moye.zip
  rm -f bin/ios.zip
  rm -f bin/ios_template.zip
  rm -f bin/ios_template_debug.zip
  rm -f bin/ios_template_release.zip

  find "$TEMPLATE_OUTPUT_DIR" -type d -empty -delete
  echo ">>> 中间产物清理完成"
}

template_python() {
  if [[ -x /usr/bin/python3 ]]; then
    echo /usr/bin/python3
    return 0
  fi

  type -P python3 || true
}

godot_template_version() {
  local python_bin="$1"

  "$python_bin" - <<'PY'
import methods

info = methods.get_version_info(".mono", True)
version = "%d.%d" % (info["major"], info["minor"])
if info["patch"] > 0:
    version += ".%d" % info["patch"]
version += ".%s%s" % (info["status"], info["module_config"])
print(version)
PY
}

create_export_templates_package() {
  local version
  local package_name
  local package_path
  local package_abs
  local stage_dir
  local templates_dir
  local error_log
  local python_bin
  local missing=0
  local file
  local required_files

  if [[ "${#TEMPLATE_FAILURE_LABELS[@]}" -ne 0 ]]; then
    echo
    echo ">>> 存在失败目标，跳过生成可导入模板包"
    return 0
  fi

  echo
  echo "========== 生成可导入 export templates 包 =========="

  python_bin="$(template_python)"
  if [[ -z "$python_bin" ]]; then
    error_log="$TEMPLATE_OUTPUT_DIR/package/error.log"
    mkdir -p "$(dirname "$error_log")"
    append_error_log_to "$error_log" "生成模板包失败" "未找到 python3，无法创建 zip 格式模板包。"
    record_template_failure "Export templates package" "$error_log"
    return 0
  fi

  version="$(godot_template_version "$python_bin")"
  package_name="godot-${version}-moye-export_templates.tpz"
  package_path="$TEMPLATE_OUTPUT_DIR/$package_name"
  package_abs="$(cd "$(dirname "$package_path")" && pwd)/$(basename "$package_path")"
  stage_dir="$TEMPLATE_OUTPUT_DIR/.package"
  templates_dir="$stage_dir/templates"
  error_log="$TEMPLATE_OUTPUT_DIR/package/error.log"

  required_files=(
    "$(android_template_output_dir)/android_debug.apk"
    "$(android_template_output_dir)/android_release.apk"
    "$(android_template_output_dir)/android_source.zip"
    "$(windows_template_output_dir)/windows_debug_x86_32.exe"
    "$(windows_template_output_dir)/windows_debug_x86_32_console.exe"
    "$(windows_template_output_dir)/windows_release_x86_32.exe"
    "$(windows_template_output_dir)/windows_release_x86_32_console.exe"
    "$(windows_template_output_dir)/windows_debug_x86_64.exe"
    "$(windows_template_output_dir)/windows_debug_x86_64_console.exe"
    "$(windows_template_output_dir)/windows_release_x86_64.exe"
    "$(windows_template_output_dir)/windows_release_x86_64_console.exe"
    "$(windows_template_output_dir)/windows_debug_arm64.exe"
    "$(windows_template_output_dir)/windows_debug_arm64_console.exe"
    "$(windows_template_output_dir)/windows_release_arm64.exe"
    "$(windows_template_output_dir)/windows_release_arm64_console.exe"
    "$(macos_template_output_dir)/macos.zip"
    "$(ios_template_output_dir)/ios.zip"
    "$TEMPLATE_OUTPUT_DIR/common/icudt_godot.dat"
  )

  for file in "${required_files[@]}"; do
    if [[ ! -e "$file" ]]; then
      echo "❌ 模板包缺少文件: $file"
      append_error_log_to "$error_log" "生成模板包失败" "缺少文件: $file"
      missing=1
    fi
  done

  if [[ "$missing" -ne 0 ]]; then
    record_template_failure "Export templates package" "$error_log"
    return 0
  fi

  rm -rf "$stage_dir"
  rm -f "$package_path"
  mkdir -p "$templates_dir"

  printf '%s\n' "$version" > "$templates_dir/version.txt"

  for file in "${required_files[@]}"; do
    cp -p "$file" "$templates_dir/$(basename "$file")"
  done

  "$python_bin" - "$stage_dir" "$package_abs" <<'PY'
import os
import sys
import zipfile

stage_dir, package_path = sys.argv[1:3]

with zipfile.ZipFile(package_path, "w", zipfile.ZIP_DEFLATED) as archive:
    for root, dirs, files in os.walk(stage_dir):
        dirs.sort()
        for file_name in sorted(files):
            path = os.path.join(root, file_name)
            archive_name = os.path.relpath(path, stage_dir).replace(os.sep, "/")
            archive.write(path, archive_name)
PY

  rm -rf "$stage_dir"
  echo "✅ 可导入模板包: $package_path"
}

collect_template_outputs() {
  echo
  echo "========== 归档 export templates =========="
  echo ">>> 输出目录: $TEMPLATE_OUTPUT_DIR"

  mkdir -p "$TEMPLATE_OUTPUT_DIR"

  # Android
  copy_template_output_first_as "$(android_template_output_dir)" "android_debug.apk" \
    "bin/android_debug.apk" \
    "bin/android_monoDebug.apk"
  copy_template_output_first_as "$(android_template_output_dir)" "android_release.apk" \
    "bin/android_release.apk" \
    "bin/android_monoRelease.apk"
  copy_template_output_as "bin/android_source.zip" "$(android_template_output_dir)" "android_source.zip"

  # Windows
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_debug_x86_32_console.exe" \
    "bin/godot.windows.template_debug.x86_32.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_debug.x86_32.moye.mono.console.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_debug_x86_32.exe" \
    "bin/godot.windows.template_debug.x86_32.llvm.moye.mono.exe" \
    "bin/godot.windows.template_debug.x86_32.moye.mono.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_debug_x86_64_console.exe" \
    "bin/godot.windows.template_debug.x86_64.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_debug.x86_64.moye.mono.console.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_debug_x86_64.exe" \
    "bin/godot.windows.template_debug.x86_64.llvm.moye.mono.exe" \
    "bin/godot.windows.template_debug.x86_64.moye.mono.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_debug_arm64_console.exe" \
    "bin/godot.windows.template_debug.arm64.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_debug.arm64.moye.mono.console.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_debug_arm64.exe" \
    "bin/godot.windows.template_debug.arm64.llvm.moye.mono.exe" \
    "bin/godot.windows.template_debug.arm64.moye.mono.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_release_x86_32_console.exe" \
    "bin/godot.windows.template_release.x86_32.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_release.x86_32.moye.mono.console.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_release_x86_32.exe" \
    "bin/godot.windows.template_release.x86_32.llvm.moye.mono.exe" \
    "bin/godot.windows.template_release.x86_32.moye.mono.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_release_x86_64_console.exe" \
    "bin/godot.windows.template_release.x86_64.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_release.x86_64.moye.mono.console.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_release_x86_64.exe" \
    "bin/godot.windows.template_release.x86_64.llvm.moye.mono.exe" \
    "bin/godot.windows.template_release.x86_64.moye.mono.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_release_arm64_console.exe" \
    "bin/godot.windows.template_release.arm64.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_release.arm64.moye.mono.console.exe"
  copy_template_output_first_as "$(windows_template_output_dir)" "windows_release_arm64.exe" \
    "bin/godot.windows.template_release.arm64.llvm.moye.mono.exe" \
    "bin/godot.windows.template_release.arm64.moye.mono.exe"

  # macOS
  copy_template_output_first_as "$(macos_template_output_dir)" "macos.zip" \
    "bin/macos.zip" \
    "bin/godot_macos_moye_mono.zip" \
    "bin/macos_template.zip"

  # iOS
  copy_template_output_first_as "$(ios_template_output_dir)" "ios.zip" \
    "bin/ios.zip" \
    "bin/godot_ios_moye.zip" \
    "bin/ios_template.zip" \
    "bin/ios_template_release.zip"

  # Common
  copy_template_file_as "thirdparty/icu4c/icudt_godot.dat" "$TEMPLATE_OUTPUT_DIR/common" "icudt_godot.dat"

  echo
  echo ">>> export templates 已归档到: $TEMPLATE_OUTPUT_DIR"
  find "$TEMPLATE_OUTPUT_DIR" -maxdepth 3 -mindepth 1 -print | sort
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
  run_logged "$MACOS_EDITOR_BIN" --headless --generate-mono-glue modules/mono/glue

  echo
  echo ">>> 构建 GodotSharp managed assemblies"
  run_logged ./modules/mono/build_scripts/build_assemblies.py \
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

  clear_error_log "$(android_template_output_dir)/error.log"

  run_android_template_scons "Android template_debug arm32" \
    platform=android \
    target=template_debug \
    arch=arm32 \
    module_mono_enabled=yes

  run_android_template_scons "Android template_debug arm64" \
    platform=android \
    target=template_debug \
    arch=arm64 \
    module_mono_enabled=yes

  run_android_template_scons "Android template_debug x86_32" \
    platform=android \
    target=template_debug \
    arch=x86_32 \
    module_mono_enabled=yes

  run_android_template_scons "Android template_debug x86_64" \
    platform=android \
    target=template_debug \
    arch=x86_64 \
    module_mono_enabled=yes

  run_android_template_scons "Android template_release arm32" \
    platform=android \
    target=template_release \
    arch=arm32 \
    module_mono_enabled=yes

  run_android_template_scons "Android template_release arm64" \
    platform=android \
    target=template_release \
    arch=arm64 \
    module_mono_enabled=yes

  run_android_template_scons "Android template_release x86_32" \
    platform=android \
    target=template_release \
    arch=x86_32 \
    module_mono_enabled=yes

  run_android_template_scons "Android template_release x86_64" \
    platform=android \
    target=template_release \
    arch=x86_64 \
    module_mono_enabled=yes \
    generate_android_binaries=yes

  check_android_template_output_any "Android template_debug APK" "Android debug APK template" \
    "bin/android_debug.apk" \
    "bin/android_monoDebug.apk"
  check_android_template_output_any "Android template_release APK" "Android release APK template" \
    "bin/android_release.apk" \
    "bin/android_monoRelease.apk"
  check_android_template_output "Android source template" "bin/android_source.zip" "Android source template"
}

build_windows_templates() {
  echo
  echo "========== 构建 Windows templates =========="

  clear_error_log "$(windows_template_output_dir)/error.log"

  if ! configure_windows_llvm_mingw; then
    record_windows_toolchain_failure
    return 0
  fi

  run_windows_template_scons "Windows template_debug x86_32" \
    platform=windows \
    target=template_debug \
    arch=x86_32 \
    module_mono_enabled=yes

  run_windows_template_scons "Windows template_release x86_32" \
    platform=windows \
    target=template_release \
    arch=x86_32 \
    module_mono_enabled=yes

  run_windows_template_scons "Windows template_debug x86_64" \
    platform=windows \
    target=template_debug \
    arch=x86_64 \
    module_mono_enabled=yes

  run_windows_template_scons "Windows template_release x86_64" \
    platform=windows \
    target=template_release \
    arch=x86_64 \
    module_mono_enabled=yes

  run_windows_template_scons "Windows template_debug arm64" \
    platform=windows \
    target=template_debug \
    arch=arm64 \
    module_mono_enabled=yes

  run_windows_template_scons "Windows template_release arm64" \
    platform=windows \
    target=template_release \
    arch=arm64 \
    module_mono_enabled=yes

  check_windows_template_output_any "Windows template_debug x86_32 console" "Windows template_debug x86_32 console" \
    "bin/godot.windows.template_debug.x86_32.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_debug.x86_32.moye.mono.console.exe"
  check_windows_template_output_any "Windows template_debug x86_32" "Windows template_debug x86_32" \
    "bin/godot.windows.template_debug.x86_32.llvm.moye.mono.exe" \
    "bin/godot.windows.template_debug.x86_32.moye.mono.exe"
  check_windows_template_output_any "Windows template_debug x86_64 console" "Windows template_debug x86_64 console" \
    "bin/godot.windows.template_debug.x86_64.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_debug.x86_64.moye.mono.console.exe"
  check_windows_template_output_any "Windows template_debug x86_64" "Windows template_debug x86_64" \
    "bin/godot.windows.template_debug.x86_64.llvm.moye.mono.exe" \
    "bin/godot.windows.template_debug.x86_64.moye.mono.exe"
  check_windows_template_output_any "Windows template_debug arm64 console" "Windows template_debug arm64 console" \
    "bin/godot.windows.template_debug.arm64.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_debug.arm64.moye.mono.console.exe"
  check_windows_template_output_any "Windows template_debug arm64" "Windows template_debug arm64" \
    "bin/godot.windows.template_debug.arm64.llvm.moye.mono.exe" \
    "bin/godot.windows.template_debug.arm64.moye.mono.exe"
  check_windows_template_output_any "Windows template_release x86_32 console" "Windows template_release x86_32 console" \
    "bin/godot.windows.template_release.x86_32.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_release.x86_32.moye.mono.console.exe"
  check_windows_template_output_any "Windows template_release x86_32" "Windows template_release x86_32" \
    "bin/godot.windows.template_release.x86_32.llvm.moye.mono.exe" \
    "bin/godot.windows.template_release.x86_32.moye.mono.exe"
  check_windows_template_output_any "Windows template_release x86_64 console" "Windows template_release x86_64 console" \
    "bin/godot.windows.template_release.x86_64.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_release.x86_64.moye.mono.console.exe"
  check_windows_template_output_any "Windows template_release x86_64" "Windows template_release x86_64" \
    "bin/godot.windows.template_release.x86_64.llvm.moye.mono.exe" \
    "bin/godot.windows.template_release.x86_64.moye.mono.exe"
  check_windows_template_output_any "Windows template_release arm64 console" "Windows template_release arm64 console" \
    "bin/godot.windows.template_release.arm64.llvm.moye.mono.console.exe" \
    "bin/godot.windows.template_release.arm64.moye.mono.console.exe"
  check_windows_template_output_any "Windows template_release arm64" "Windows template_release arm64" \
    "bin/godot.windows.template_release.arm64.llvm.moye.mono.exe" \
    "bin/godot.windows.template_release.arm64.moye.mono.exe"
}

build_macos_templates() {
  echo
  echo "========== 构建 macOS templates =========="

  run_template_scons macos template_debug_arm64 "macOS template_debug arm64" \
    platform=macos \
    target=template_debug \
    arch=arm64 \
    module_mono_enabled=yes

  run_template_scons macos template_release_arm64 "macOS template_release arm64" \
    platform=macos \
    target=template_release \
    arch=arm64 \
    module_mono_enabled=yes

  run_template_scons macos template_debug_x86_64 "macOS template_debug x86_64" \
    platform=macos \
    target=template_debug \
    arch=x86_64 \
    module_mono_enabled=yes

  run_template_scons macos template_release_x86_64 "macOS template_release x86_64" \
    platform=macos \
    target=template_release \
    arch=x86_64 \
    module_mono_enabled=yes \
    generate_bundle=yes

  check_template_output macos template_debug_arm64 "macOS template_debug arm64" "bin/godot.macos.template_debug.arm64.moye.mono" "macOS template_debug arm64"
  check_template_output macos template_release_arm64 "macOS template_release arm64" "bin/godot.macos.template_release.arm64.moye.mono" "macOS template_release arm64"
  check_template_output macos template_debug_x86_64 "macOS template_debug x86_64" "bin/godot.macos.template_debug.x86_64.moye.mono" "macOS template_debug x86_64"
  check_template_output macos template_release_x86_64 "macOS template_release x86_64" "bin/godot.macos.template_release.x86_64.moye.mono" "macOS template_release x86_64"
  check_platform_template_output_any macos "macOS template zip" "macOS template zip" \
    "bin/macos.zip" \
    "bin/godot_macos_moye_mono.zip" \
    "bin/macos_template.zip"

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

  run_template_scons ios template_debug_arm64 "iOS template_debug arm64" \
    platform=ios \
    target=template_debug \
    module_mono_enabled=yes \
    generate_bundle=yes

  run_template_scons ios template_release_arm64 "iOS template_release arm64" \
    platform=ios \
    target=template_release \
    module_mono_enabled=yes \
    generate_bundle=yes

  check_template_output ios template_debug_arm64 "iOS template_debug arm64" "bin/libgodot.ios.template_debug.arm64.moye.a" "iOS template_debug arm64 static library"
  check_template_output ios template_release_arm64 "iOS template_release arm64" "bin/libgodot.ios.template_release.arm64.moye.a" "iOS template_release arm64 static library"
  check_platform_template_output_any ios "iOS template zip" "iOS template zip" \
    "bin/ios.zip" \
    "bin/godot_ios_moye.zip" \
    "bin/ios_template.zip" \
    "bin/ios_template_release.zip"

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

  if [[ -f "bin/godot_ios_moye.zip" ]]; then
    echo "✅ iOS template zip: bin/godot_ios_moye.zip"
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
  collect_template_outputs
  create_export_templates_package
  cleanup_template_intermediates
  summarize_template_failures
}

if [[ "$BUILD_TEMPLATES" -eq 1 ]]; then
  configure_custom_build_options template
  build_all_templates
else
  configure_custom_build_options editor
  build_macos_editor
fi

echo
if [[ "$BUILD_EXIT_CODE" -eq 0 ]]; then
  echo "========== 构建完成 =========="
else
  echo "========== 构建完成，但存在失败目标 =========="
fi

if [[ "$BUILD_TEMPLATES" -eq 1 ]]; then
  ls -la "$TEMPLATE_OUTPUT_DIR"
else
  ls -la bin
fi

exit "$BUILD_EXIT_CODE"
