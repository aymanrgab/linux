#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/pipa-clang}"
DIST_DIR="${DIST_DIR:-$ROOT_DIR/dist}"
BOOTIMG_TEMPLATE="${BOOTIMG_TEMPLATE:-$ROOT_DIR/../armtix-pipa-boot.img}"
CONFIG_FILE="${CONFIG_FILE:-}"
RAMDISK_PATH="${RAMDISK_PATH:-}"
RAMDISK_DIR="${RAMDISK_DIR:-}"
JOBS="${JOBS:-$(nproc)}"
REMOTE="${REMOTE:-}"
SSH_PORT="${SSH_PORT:-}"
REMOTE_DIR="${REMOTE_DIR:-}"
MODULES_ROOT="${MODULES_ROOT:-/}"
BOOTIMG_DEST="${BOOTIMG_DEST:-}"
INSTALL_MODULES=1
FASTBOOT_SERIAL="${FASTBOOT_SERIAL:-}"
FLASH_SLOT="${FLASH_SLOT:-both}"
SET_ACTIVE_SLOT="${SET_ACTIVE_SLOT:-keep}"
LTO_MODE="${LTO_MODE:-none}"
LOCALVERSION_OVERRIDE="${LOCALVERSION_OVERRIDE:-}"
KEEP_LOCALVERSION_AUTO=0
ACTION="all"

DTB_NAME="sm8250-xiaomi-pipa.dtb"
DEFCONFIG="$ROOT_DIR/arch/arm64/configs/defconfig"
SM8250_CONFIG="$ROOT_DIR/arch/arm64/configs/sm8250.config"
DEFAULT_EXTERNAL_CONFIG="$ROOT_DIR/../config-qcom-sm8250-pipa.aarch64"
IMAGE_PATH=""
IMAGE_GZ_PATH=""
DTB_PATH=""
MAKE_ARGS=()

usage() {
  cat <<EOF
Usage: $(basename "$0") [action] [options]

Actions:
  all             Configure, build, stage modules, and repack boot.img
  all-deploy      Build, pack artifacts, and deploy to remote device
  config          Generate .config from full config or defconfig + sm8250 fragment
  build           Configure and build Image/Image.gz/dtbs/modules
  modules         Stage modules and create modules tarball
  bootimg         Repack boot.img from built Image.gz + pipa DTB
  deploy          Upload/install prebuilt artifacts to remote device
  flash-boot      Flash packed boot.img over fastboot
  inspect-bootimg Study template boot.img and dump repack metadata
  clean           Remove build and dist output

Options:
  --out-dir PATH          Build output directory
  --dist-dir PATH         Artifact directory
  --bootimg-template PATH Template boot.img to inspect/repack
  --config-file PATH      Use full kernel config file as base
  --ramdisk PATH          Override ramdisk file for boot.img packing
  --ramdisk-dir PATH      Build plain newc cpio ramdisk from directory
  --jobs N                Parallel build jobs (default: nproc)
  --remote USER@HOST      Remote tablet host for deploy action
  --ssh-port N            SSH port for deploy action
  --remote-dir PATH       Remote staging directory
  --modules-root PATH     Remote extraction root for modules tarball
  --bootimg-dest PATH     Optional remote install path for boot.img
  --no-install-modules    Upload artifacts but skip remote modules install
  --fastboot-serial ID    Fastboot serial to target
  --slot MODE             both, current, inactive, a, or b (default: both)
  --set-active MODE       keep, current, inactive, a, or b (default: keep)
  --lto MODE              none, thin, or full
  --localversion STR      Override CONFIG_LOCALVERSION
  --keep-localversion-auto
                          Keep CONFIG_LOCALVERSION_AUTO enabled
  -h, --help              Show this help

Examples:
  ./build.sh
  ./build.sh build --config-file ../config-qcom-sm8250-pipa.aarch64
  ./build.sh build --jobs 16
  ./build.sh bootimg --bootimg-template ../armtix-pipa-boot.img
  ./build.sh deploy --remote alarm@pipa --bootimg-dest /boot/boot.img
  ./build.sh all-deploy --remote root@192.168.1.50
  ./build.sh flash-boot --slot both
  ./build.sh flash-boot --slot inactive --set-active inactive
  ./build.sh all --lto full --localversion "-pipa"
EOF
}

log() {
  printf '[build] %s\n' "$*"
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

run_make() {
  make "${MAKE_ARGS[@]}" "$@"
}

kernelrelease() {
  run_make -s kernelrelease
}

artifact_boot_dir() {
  printf '%s\n' "$DIST_DIR/boot"
}

artifact_bootimg_path() {
  local krel="$1"
  printf '%s\n' "$(artifact_boot_dir)/pipa-$krel.boot.img"
}

artifact_modules_tarball() {
  local krel="$1"
  printf '%s\n' "$DIST_DIR/modules-$krel.tar.zst"
}

parse_args() {
  while (($#)); do
    case "$1" in
      all|all-deploy|config|build|modules|bootimg|deploy|flash-boot|inspect-bootimg|clean)
        ACTION="$1"
        ;;
      --out-dir)
        shift
        OUT_DIR="${1:?missing value for --out-dir}"
        ;;
      --dist-dir)
        shift
        DIST_DIR="${1:?missing value for --dist-dir}"
        ;;
      --bootimg-template)
        shift
        BOOTIMG_TEMPLATE="${1:?missing value for --bootimg-template}"
        ;;
      --config-file)
        shift
        CONFIG_FILE="${1:?missing value for --config-file}"
        ;;
      --ramdisk)
        shift
        RAMDISK_PATH="${1:?missing value for --ramdisk}"
        ;;
      --ramdisk-dir)
        shift
        RAMDISK_DIR="${1:?missing value for --ramdisk-dir}"
        ;;
      --remote)
        shift
        REMOTE="${1:?missing value for --remote}"
        ;;
      --ssh-port)
        shift
        SSH_PORT="${1:?missing value for --ssh-port}"
        ;;
      --remote-dir)
        shift
        REMOTE_DIR="${1:?missing value for --remote-dir}"
        ;;
      --modules-root)
        shift
        MODULES_ROOT="${1:?missing value for --modules-root}"
        ;;
      --bootimg-dest)
        shift
        BOOTIMG_DEST="${1:?missing value for --bootimg-dest}"
        ;;
      --no-install-modules)
        INSTALL_MODULES=0
        ;;
      --fastboot-serial)
        shift
        FASTBOOT_SERIAL="${1:?missing value for --fastboot-serial}"
        ;;
      --slot)
        shift
        FLASH_SLOT="${1:?missing value for --slot}"
        ;;
      --set-active)
        shift
        SET_ACTIVE_SLOT="${1:?missing value for --set-active}"
        ;;
      --jobs)
        shift
        JOBS="${1:?missing value for --jobs}"
        ;;
      --lto)
        shift
        LTO_MODE="${1:?missing value for --lto}"
        ;;
      --localversion)
        shift
        LOCALVERSION_OVERRIDE="${1:?missing value for --localversion}"
        ;;
      --keep-localversion-auto)
        KEEP_LOCALVERSION_AUTO=1
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "unknown argument: $1"
        ;;
    esac
    shift
  done
}

refresh_derived_paths() {
  IMAGE_PATH="$OUT_DIR/arch/arm64/boot/Image"
  IMAGE_GZ_PATH="$OUT_DIR/arch/arm64/boot/Image.gz"
  DTB_PATH="$OUT_DIR/arch/arm64/boot/dts/qcom/$DTB_NAME"
  MAKE_ARGS=(
    -C "$ROOT_DIR"
    ARCH=arm64
    O="$OUT_DIR"
    LLVM=1
    LLVM_IAS=1
  )
}

validate_args() {
  case "$LTO_MODE" in
    none|thin|full) ;;
    *)
      die "invalid --lto mode: $LTO_MODE"
      ;;
  esac

  if [[ -n "$RAMDISK_PATH" && -n "$RAMDISK_DIR" ]]; then
    die "use either --ramdisk or --ramdisk-dir, not both"
  fi

  if [[ -n "$CONFIG_FILE" && ! -f "$CONFIG_FILE" ]]; then
    die "config file not found: $CONFIG_FILE"
  fi

  if [[ "$INSTALL_MODULES" -ne 0 && "$MODULES_ROOT" != /* ]]; then
    die "--modules-root must be an absolute path"
  fi

  if [[ -n "$BOOTIMG_DEST" && "$BOOTIMG_DEST" != /* ]]; then
    die "--bootimg-dest must be an absolute path"
  fi

  case "$FLASH_SLOT" in
    both|current|inactive|a|b) ;;
    *)
      die "invalid --slot mode: $FLASH_SLOT"
      ;;
  esac

  case "$SET_ACTIVE_SLOT" in
    keep|current|inactive|a|b) ;;
    *)
      die "invalid --set-active mode: $SET_ACTIVE_SLOT"
      ;;
  esac
}

ensure_build_tools() {
  need_cmd make
  need_cmd clang
  need_cmd ld.lld
  need_cmd llvm-ar
  need_cmd llvm-objcopy
  need_cmd llvm-strip
}

ensure_bootimg_tools() {
  need_cmd unpack_bootimg
  need_cmd mkbootimg
  need_cmd cpio
  need_cmd file
}

ensure_artifact_tools() {
  need_cmd tar
  need_cmd zstd
}

ensure_remote_tools() {
  need_cmd ssh
  need_cmd scp
}

ensure_fastboot_tools() {
  need_cmd fastboot
}

configure_kernel() {
  ensure_build_tools
  mkdir -p "$OUT_DIR"

  if [[ -n "$CONFIG_FILE" ]]; then
    log "use full config base: $CONFIG_FILE"
    cp "$CONFIG_FILE" "$OUT_DIR/.config"
  elif [[ -f "$DEFAULT_EXTERNAL_CONFIG" ]]; then
    log "use auto-detected full config base: $DEFAULT_EXTERNAL_CONFIG"
    cp "$DEFAULT_EXTERNAL_CONFIG" "$OUT_DIR/.config"
  else
    log "merge defconfig + sm8250 fragment"
    "$ROOT_DIR/scripts/kconfig/merge_config.sh" -Q -m -O "$OUT_DIR" \
      "$DEFCONFIG" \
      "$SM8250_CONFIG"
  fi

  if [[ ! -f "$OUT_DIR/.config" ]]; then
    die "failed to create $OUT_DIR/.config"
  fi

  if [[ "$KEEP_LOCALVERSION_AUTO" -eq 0 ]]; then
    "$ROOT_DIR/scripts/config" --file "$OUT_DIR/.config" -d LOCALVERSION_AUTO
  fi

  if [[ -n "$LOCALVERSION_OVERRIDE" ]]; then
    "$ROOT_DIR/scripts/config" --file "$OUT_DIR/.config" \
      --set-str LOCALVERSION "$LOCALVERSION_OVERRIDE"
  fi

  case "$LTO_MODE" in
    none)
      "$ROOT_DIR/scripts/config" --file "$OUT_DIR/.config" \
        -e LTO_NONE \
        -d LTO_CLANG_THIN \
        -d LTO_CLANG_FULL
      ;;
    thin)
      "$ROOT_DIR/scripts/config" --file "$OUT_DIR/.config" \
        -d LTO_NONE \
        -e LTO_CLANG_THIN \
        -d LTO_CLANG_FULL
      ;;
    full)
      "$ROOT_DIR/scripts/config" --file "$OUT_DIR/.config" \
        -d LTO_NONE \
        -d LTO_CLANG_THIN \
        -e LTO_CLANG_FULL
      ;;
  esac

  log "resolve final kernel config"
  run_make olddefconfig
}

ensure_config() {
  [[ -f "$OUT_DIR/.config" ]] || configure_kernel
}

build_kernel() {
  ensure_config

  log "build kernel with clang"
  run_make -j"$JOBS" Image Image.gz dtbs modules
}

require_build_outputs() {
  [[ -f "$IMAGE_PATH" ]] || die "missing $IMAGE_PATH, run '$0 build' first"
  [[ -f "$IMAGE_GZ_PATH" ]] || die "missing $IMAGE_GZ_PATH, run '$0 build' first"
  [[ -f "$DTB_PATH" ]] || die "missing $DTB_PATH, run '$0 build' first"
}

stage_boot_files() {
  local krel boot_dir

  require_build_outputs
  krel="$(kernelrelease)"
  boot_dir="$(artifact_boot_dir)"

  mkdir -p "$boot_dir"

  log "stage boot artifacts"
  install -Dm644 "$IMAGE_PATH" "$boot_dir/Image"
  install -Dm644 "$IMAGE_GZ_PATH" "$boot_dir/Image.gz"
  install -Dm644 "$DTB_PATH" "$boot_dir/$DTB_NAME"
  install -Dm644 "$OUT_DIR/.config" "$boot_dir/config-$krel"
  printf '%s\n' "$krel" > "$boot_dir/kernelrelease.txt"
}

stage_modules() {
  local krel modules_root install_root modules_dir tarball

  ensure_artifact_tools
  require_build_outputs
  krel="$(kernelrelease)"
  modules_root="$DIST_DIR/modules/$krel"
  install_root="$modules_root/usr"
  modules_dir="$install_root/lib/modules/$krel"
  tarball="$(artifact_modules_tarball "$krel")"

  log "stage modules"
  rm -rf "$modules_root"
  mkdir -p "$install_root"
  run_make INSTALL_MOD_PATH="$install_root" INSTALL_MOD_STRIP=1 DEPMOD=/bin/true \
    modules_install

  rm -f "$modules_dir/build" "$modules_dir/source"

  log "pack modules archive"
  rm -f "$tarball"
  tar -I zstd -cf "$tarball" -C "$modules_root" usr/lib/modules
}

prepare_template_bootimg() {
  local work_dir="$1"

  mkdir -p "$work_dir/raw"
  unpack_bootimg --boot_img "$BOOTIMG_TEMPLATE" --out "$work_dir/raw" \
    > "$work_dir/unpack.txt"
  unpack_bootimg --boot_img "$BOOTIMG_TEMPLATE" --out "$work_dir/raw" \
    --format=mkbootimg -0 > "$work_dir/mkbootimg.args0"
}

build_ramdisk_from_dir() {
  local source_dir="$1"
  local output_path="$2"

  [[ -d "$source_dir" ]] || die "ramdisk directory not found: $source_dir"

  (
    cd "$source_dir"
    find . -mindepth 1 -print0 | sort -z | cpio --null -o --format=newc --quiet
  ) > "$output_path"
}

inspect_bootimg() {
  local inspect_dir kernel_path ramdisk_path fdt_offset="" has_modules="no" has_init="no"

  ensure_bootimg_tools
  [[ -f "$BOOTIMG_TEMPLATE" ]] || die "template boot.img not found: $BOOTIMG_TEMPLATE"

  inspect_dir="$DIST_DIR/template-bootimg"
  rm -rf "$inspect_dir"
  mkdir -p "$inspect_dir"

  log "inspect template bootimg"
  prepare_template_bootimg "$inspect_dir"

  kernel_path="$inspect_dir/raw/kernel"
  ramdisk_path="$inspect_dir/raw/ramdisk"

  unpack_bootimg --boot_img "$BOOTIMG_TEMPLATE" --out "$inspect_dir/raw" --format=mkbootimg \
    > "$inspect_dir/mkbootimg.args.txt"
  cpio -t < "$ramdisk_path" > "$inspect_dir/ramdisk.list" 2>/dev/null

  if grep -Eq '^usr/lib/modules/|^lib/modules/' "$inspect_dir/ramdisk.list"; then
    has_modules="yes"
  fi

  if grep -Eq '(^|/)init$' "$inspect_dir/ramdisk.list"; then
    has_init="yes"
  fi

  fdt_offset="$(grep -abo $'\320\r\376\355' "$kernel_path" | tail -1 | cut -d: -f1 || true)"
  if [[ -n "$fdt_offset" ]]; then
    dd if="$kernel_path" of="$inspect_dir/kernel.dtb" bs=1 skip="$fdt_offset" status=none
  fi

  {
    printf 'template_bootimg=%s\n' "$BOOTIMG_TEMPLATE"
    printf 'kernel_file=%s\n' "$(file -b "$kernel_path")"
    printf 'ramdisk_file=%s\n' "$(file -b "$ramdisk_path")"
    printf 'ramdisk_has_modules=%s\n' "$has_modules"
    printf 'ramdisk_has_init=%s\n' "$has_init"
    if [[ -n "$fdt_offset" ]]; then
      printf 'kernel_appended_dtb_offset=%s\n' "$fdt_offset"
      printf 'kernel_appended_dtb=%s\n' "$(file -b "$inspect_dir/kernel.dtb")"
    fi
  } > "$inspect_dir/summary.txt"

  log "template info written to $inspect_dir"
}

pack_bootimg() {
  local tmp_dir kernel_payload boot_dir krel ramdisk bootimg_out
  local -a mkbootimg_args=()

  ensure_bootimg_tools
  require_build_outputs
  [[ -f "$BOOTIMG_TEMPLATE" ]] || die "template boot.img not found: $BOOTIMG_TEMPLATE"

  krel="$(kernelrelease)"
  boot_dir="$(artifact_boot_dir)"
  kernel_payload="$boot_dir/Image.gz-dtb"
  bootimg_out="$(artifact_bootimg_path "$krel")"
  tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/pipa-bootimg.XXXXXX")"

  mkdir -p "$boot_dir"

  log "prepare template bootimg metadata"
  prepare_template_bootimg "$tmp_dir"
  cp "$tmp_dir/unpack.txt" "$boot_dir/bootimg-template-info.txt"
  unpack_bootimg --boot_img "$BOOTIMG_TEMPLATE" --out "$tmp_dir/raw" --format=mkbootimg \
    > "$boot_dir/bootimg-template.mkbootimg.args"
  cpio -t < "$tmp_dir/raw/ramdisk" > "$boot_dir/bootimg-template.ramdisk.list" 2>/dev/null

  if [[ -n "$RAMDISK_PATH" ]]; then
    [[ -f "$RAMDISK_PATH" ]] || die "ramdisk file not found: $RAMDISK_PATH"
    ramdisk="$RAMDISK_PATH"
  elif [[ -n "$RAMDISK_DIR" ]]; then
    ramdisk="$tmp_dir/custom.ramdisk"
    build_ramdisk_from_dir "$RAMDISK_DIR" "$ramdisk"
  else
    ramdisk="$tmp_dir/raw/ramdisk"
  fi

  log "create kernel payload Image.gz + DTB"
  cat "$IMAGE_GZ_PATH" "$DTB_PATH" > "$kernel_payload"

  while IFS= read -r -d '' arg; do
    mkbootimg_args+=("$arg")
  done < "$tmp_dir/mkbootimg.args0"

  for i in "${!mkbootimg_args[@]}"; do
    case "${mkbootimg_args[$i]}" in
      --kernel)
        mkbootimg_args[$((i + 1))]="$kernel_payload"
        ;;
      --ramdisk)
        mkbootimg_args[$((i + 1))]="$ramdisk"
        ;;
    esac
  done

  log "pack bootimg"
  mkbootimg "${mkbootimg_args[@]}" --output "$bootimg_out"
  sha256sum "$bootimg_out" > "$bootimg_out.sha256"
  rm -rf "$tmp_dir"

  log "bootimg ready: $bootimg_out"
}

clean_outputs() {
  log "remove $OUT_DIR and $DIST_DIR"
  rm -rf "$OUT_DIR" "$DIST_DIR"
}

fastboot_getvar() {
  local var="$1"
  shift

  "$@" getvar "$var" 2>&1 | sed -n "s/^${var}: //p" | tail -n 1
}

resolve_slot_mode() {
  local mode="$1"
  local current_slot="$2"

  case "$mode" in
    a|b)
      printf '%s\n' "$mode"
      ;;
    current)
      [[ -n "$current_slot" ]] || die "fastboot current-slot unavailable, cannot use --slot/--set-active current"
      printf '%s\n' "$current_slot"
      ;;
    inactive)
      [[ -n "$current_slot" ]] || die "fastboot current-slot unavailable, cannot use --slot/--set-active inactive"
      if [[ "$current_slot" == "a" ]]; then
        printf 'b\n'
      else
        printf 'a\n'
      fi
      ;;
    *)
      die "internal error: unsupported slot mode $mode"
      ;;
  esac
}

flash_bootimg() {
  local krel bootimg_out current_slot has_slot_boot slot_name active_slot
  local -a fb_cmd=()
  local -a target_slots=()

  ensure_fastboot_tools
  krel="$(kernelrelease)"
  bootimg_out="$(artifact_bootimg_path "$krel")"
  [[ -f "$bootimg_out" ]] || die "missing $bootimg_out, run '$0 bootimg' first"

  fb_cmd=(fastboot)
  if [[ -n "$FASTBOOT_SERIAL" ]]; then
    fb_cmd+=(-s "$FASTBOOT_SERIAL")
  fi

  log "wait for fastboot device"
  until "${fb_cmd[@]}" devices | grep -q '[[:graph:]]'; do
    sleep 1
  done

  current_slot="$(fastboot_getvar current-slot "${fb_cmd[@]}" || true)"
  has_slot_boot="$(fastboot_getvar has-slot:boot "${fb_cmd[@]}" || true)"
  if [[ -n "$current_slot" ]]; then
    log "current fastboot slot: $current_slot"
  else
    log "current fastboot slot unavailable"
  fi

  if [[ "$has_slot_boot" != "yes" ]]; then
    if [[ "$FLASH_SLOT" != "both" && "$FLASH_SLOT" != "current" ]]; then
      die "device boot partition is not slotted, cannot use --slot $FLASH_SLOT"
    fi
    if [[ "$SET_ACTIVE_SLOT" != "keep" && "$SET_ACTIVE_SLOT" != "current" ]]; then
      die "device boot partition is not slotted, cannot use --set-active $SET_ACTIVE_SLOT"
    fi
    log "boot partition not slotted, flash plain boot"
    "${fb_cmd[@]}" flash boot "$bootimg_out"
    log "fastboot flash done"
    return
  fi

  case "$FLASH_SLOT" in
    both)
      target_slots=(a b)
      ;;
    *)
      target_slots=("$(resolve_slot_mode "$FLASH_SLOT" "$current_slot")")
      ;;
  esac

  for slot_name in "${target_slots[@]}"; do
    log "flash boot_${slot_name} <- $(basename "$bootimg_out")"
    "${fb_cmd[@]}" flash "boot_${slot_name}" "$bootimg_out"
  done

  if [[ "$SET_ACTIVE_SLOT" != "keep" ]]; then
    active_slot="$(resolve_slot_mode "$SET_ACTIVE_SLOT" "$current_slot")"
    log "set active slot -> $active_slot"
    "${fb_cmd[@]}" set_active "$active_slot"
  fi

  log "fastboot flash done"
}

deploy_remote() {
  local krel bootimg_out modules_tar config_out remote_stage remote_bootimg_dest
  local bootimg_name modules_name config_name
  local -a ssh_cmd scp_cmd

  ensure_remote_tools
  [[ -n "$REMOTE" ]] || die "deploy requires --remote USER@HOST"

  krel="$(kernelrelease)"
  bootimg_out="$(artifact_bootimg_path "$krel")"
  modules_tar="$(artifact_modules_tarball "$krel")"
  config_out="$(artifact_boot_dir)/config-$krel"
  remote_stage="${REMOTE_DIR:-/tmp/pipa-kernel/$krel}"
  remote_bootimg_dest="${BOOTIMG_DEST:-__EMPTY__}"
  bootimg_name="$(basename "$bootimg_out")"
  modules_name="$(basename "$modules_tar")"
  config_name="$(basename "$config_out")"

  [[ -f "$bootimg_out" ]] || die "missing $bootimg_out, run '$0 bootimg' first"
  [[ -f "$modules_tar" ]] || die "missing $modules_tar, run '$0 modules' first"
  [[ -f "$config_out" ]] || die "missing $config_out"

  ssh_cmd=(ssh)
  scp_cmd=(scp)
  if [[ -n "$SSH_PORT" ]]; then
    ssh_cmd+=(-p "$SSH_PORT")
    scp_cmd+=(-P "$SSH_PORT")
  fi

  log "create remote staging dir $remote_stage"
  "${ssh_cmd[@]}" "$REMOTE" "mkdir -p '$remote_stage'"

  log "upload bootimg, config, and modules tarball"
  "${scp_cmd[@]}" \
    "$bootimg_out" \
    "$modules_tar" \
    "$config_out" \
    "$REMOTE:$remote_stage/"

  log "run remote install"
  "${ssh_cmd[@]}" "$REMOTE" /bin/bash -s -- \
    "$remote_stage" \
    "$modules_name" \
    "$krel" \
    "$MODULES_ROOT" \
    "$bootimg_name" \
    "$remote_bootimg_dest" \
    "$INSTALL_MODULES" <<'EOF'
set -euo pipefail

remote_stage="$1"
modules_name="$2"
krel="$3"
modules_root="$4"
bootimg_name="$5"
bootimg_dest="$6"
install_modules="$7"

if [[ "$bootimg_dest" == "__EMPTY__" ]]; then
  bootimg_dest=""
fi

sudo_cmd=()
if [[ "$(id -u)" -ne 0 ]]; then
  if command -v sudo >/dev/null 2>&1; then
    sudo_cmd=(sudo)
  else
    echo "error: remote install needs root or sudo" >&2
    exit 1
  fi
fi

if [[ "$install_modules" -eq 1 ]]; then
  "${sudo_cmd[@]}" tar --zstd -xf "$remote_stage/$modules_name" -C "$modules_root"
  if command -v depmod >/dev/null 2>&1; then
    "${sudo_cmd[@]}" depmod -a "$krel"
  fi
fi

if [[ -n "$bootimg_dest" ]]; then
  "${sudo_cmd[@]}" install -Dm644 "$remote_stage/$bootimg_name" "$bootimg_dest"
fi
EOF

  log "deploy done"
  log "remote bootimg: $remote_stage/$bootimg_name"
  log "remote config:  $remote_stage/$config_name"
  if [[ "$INSTALL_MODULES" -eq 1 ]]; then
    log "remote modules installed under $MODULES_ROOT/usr/lib/modules/$krel"
  else
    log "remote modules tarball staged at $remote_stage/$modules_name"
  fi
  if [[ -n "$BOOTIMG_DEST" ]]; then
    log "remote bootimg installed to $BOOTIMG_DEST"
  fi
}

main() {
  parse_args "$@"
  validate_args
  refresh_derived_paths

  case "$ACTION" in
    clean)
      clean_outputs
      ;;
    inspect-bootimg)
      inspect_bootimg
      ;;
    config)
      configure_kernel
      ;;
    build)
      build_kernel
      stage_boot_files
      ;;
    modules)
      ensure_config
      stage_modules
      ;;
    bootimg)
      ensure_config
      stage_boot_files
      pack_bootimg
      ;;
    flash-boot)
      ensure_config
      stage_boot_files
      pack_bootimg
      flash_bootimg
      ;;
    deploy)
      require_build_outputs
      stage_boot_files
      stage_modules
      pack_bootimg
      deploy_remote
      ;;
    all)
      build_kernel
      stage_boot_files
      stage_modules
      inspect_bootimg
      pack_bootimg
      ;;
    all-deploy)
      build_kernel
      stage_boot_files
      stage_modules
      inspect_bootimg
      pack_bootimg
      deploy_remote
      ;;
    *)
      die "unsupported action: $ACTION"
      ;;
  esac
}

main "$@"
