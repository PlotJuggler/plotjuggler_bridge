#!/bin/bash
# Assemble AppDir/ for pj_bridge_ros2 from a colcon install + conda prefix.
# usage: make_appdir.sh <conda_prefix> [appdir]
set -euo pipefail

CONDA_PREFIX="$(readlink -f "$1")"
APPDIR="${2:-AppDir}"

mkdir -p "$APPDIR"/usr/{bin,lib,share}
cp install/pj_bridge/lib/pj_bridge/pj_bridge_ros2 "$APPDIR/usr/bin/"

# ldd only sees link-time dependencies. ROS loads these with dlopen():
#  - both RMW providers, so the user's RMW_IMPLEMENTATION (which must match
#    their other nodes) is honoured instead of forced here
#  - the typesupport libraries of every message type (generic subscriptions,
#    and rcl itself for its own interfaces), which dispatch again by dlopen
shopt -s nullglob
queue=(install/pj_bridge/lib/pj_bridge/pj_bridge_ros2
       "$CONDA_PREFIX"/lib/librmw_fastrtps_cpp.so
       "$CONDA_PREFIX"/lib/librmw_cyclonedds_cpp.so
       "$CONDA_PREFIX"/lib/lib*__rosidl_typesupport_*.so)

# Copy them plus their link-time dependencies from the conda env, to a fixed
# point. ldd runs on the original files: a copy's $ORIGIN rpath no longer
# resolves. glibc/ld-linux live outside the prefix and come from the host.
while ((${#queue[@]})); do
  next=()
  for src in "${queue[@]}"; do
    for dep in "$src" $(ldd "$src" 2>/dev/null | awk -v p="$CONDA_PREFIX" 'index($3, p) == 1 {print $3}'); do
      dest="$APPDIR/usr/lib/$(basename "$dep")"
      [[ "$dep" == */pj_bridge_ros2 || -e "$dest" ]] && continue
      cp -L "$dep" "$dest"
      next+=("$dep")
    done
  done
  queue=("${next[@]}")
done

# ament index + message definitions (schema discovery at runtime)
cp -r "$CONDA_PREFIX/share/ament_index" "$APPDIR/usr/share/"
for pkg_dir in "$CONDA_PREFIX"/share/*/msg "$CONDA_PREFIX"/share/*/srv; do
  pkg_name="$(basename "$(dirname "$pkg_dir")")"
  mkdir -p "$APPDIR/usr/share/$pkg_name"
  cp -r "$pkg_dir" "$APPDIR/usr/share/$pkg_name/"
done

cat > "$APPDIR/pj_bridge.desktop" <<'DESKTOP'
[Desktop Entry]
Name=pj_bridge
Exec=pj_bridge_ros2
Icon=pj_bridge
Type=Application
Categories=Science;
DESKTOP
cp pj_bridge.png "$APPDIR/pj_bridge.png"

cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/bash
SELF="$(readlink -f "$0")"
HERE="${SELF%/*}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"
# Appended, not prepended: a sourced workspace's message definitions win. The
# bundled ones only cover common types; custom messages need ROS 2 sourced.
export AMENT_PREFIX_PATH="${AMENT_PREFIX_PATH:+$AMENT_PREFIX_PATH:}${HERE}/usr"
exec "${HERE}/usr/bin/pj_bridge_ros2" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"
