#!/bin/sh
#
# DOOM (doomgeneric) DRM build for STM32F429 -- mirrors cellphone-build.sh.
#
# The doomgeneric source lives in-place at doomgeneric/doomgeneric/ and already
# has 0001-doom-stm32f429-drm-nommu.patch applied (no-MMU Z_Init, rotated
# i_video, and the doomgeneric_drm.c DRM+tslib backend). That patch -- kept
# next to this script -- is the portable record; see DOOM-PATCH-README.md to
# reproduce it on a fresh clone. Links against the cellphone tslib build, so
# run the cellphone build first.

BASE_DIR=$(pwd)/doomgeneric
cd $BASE_DIR

BUILDROOT=$BASE_DIR/../buildroot-2026.02
SYSROOT=$BUILDROOT/output/host/arm-buildroot-uclinuxfdpiceabi/sysroot
CC=$BUILDROOT/output/host/bin/arm-linux-gcc
STRIP=$BUILDROOT/output/host/bin/arm-linux-strip
CELL=$BASE_DIR/../cellphone/build          # tslib include/lib + output bin
SRC=$BASE_DIR/doomgeneric                  # patched source tree
OUT=$CELL/bin/doomdrm

CFLAGS="-O2 -mcpu=cortex-m4 -mthumb -mfdpic --sysroot=$SYSROOT \
  -I$SYSROOT/usr/include/libdrm -I$CELL/include \
  -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
  -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -Wno-unused-result"

# Minimal DOOM source set (no sound/net/dehacked; backend = doomgeneric_drm).
UNITS="am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop \
  d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info \
  i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio \
  m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc \
  m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map \
  p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec \
  p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane \
  r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound \
  tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone \
  w_file_stdc i_input i_video doomgeneric dummy doomgeneric_drm"

compile_one() {                          # $1 = output path, $2 = extra cflags
    $CC $CFLAGS $2 $SRCS -o "$1" -L$CELL/lib -lm -ldrm -lts
    $STRIP "$1"
}

compile_doom() {
    mkdir -p "$CELL/bin"
    SRCS=""
    for u in $UNITS; do SRCS="$SRCS $SRC/$u.c"; done
    compile_one "$OUT" ""                                  # double-buffered (page flip)
    compile_one "$CELL/bin/doomdrm-single" "-DDOOM_FORCE_SINGLE"   # single buffer (for A/B)
    echo "doom: built $OUT + doomdrm-single (IWAD copied from $SRC/doom1.wad at rootfs-ext2)"
}

build_doom() {
    if [ -f "$OUT" ]; then
        echo "doomdrm was built"
        return 0
    fi
    compile_doom
}

rebuild_doom() {
    echo "doom: rebuild from current doomgeneric/ source"
    rm -f "$OUT"
    compile_doom
}

case "$1" in
    rebuild) rebuild_doom ;;
    *)       build_doom ;;
esac
