#!/usr/bin/env bash
# Build the Gateway firmware outside STM32CubeIDE.
#
# CubeIDE generates its own makefiles at build time, which is fine on a desktop
# but useless in CI. This reproduces the same compile and link with a plain
# arm-none-eabi toolchain so every push proves the firmware still builds.
#
#   tools/build_firmware.sh [loopback|dual] [output-dir]
set -euo pipefail

MODE="${1:-loopback}"
OUT="${2:-build/firmware-$MODE}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="$ROOT/firmware/gateway"

case "$MODE" in
  loopback) MODE_VALUE=0 ;;
  dual)     MODE_VALUE=1 ;;
  *) echo "usage: $0 [loopback|dual] [output-dir]" >&2; exit 2 ;;
esac

CROSS="${CROSS_COMPILE:-arm-none-eabi-}"
CC="${CROSS}gcc"
SIZE="${CROSS}size"

COMMON="$ROOT/firmware/common"

MCU="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
DEFS="-DUSE_HAL_DRIVER -DSTM32F446xx -DCAN_BRINGUP_MODE=$MODE_VALUE"
INCS="
  -I$PROJ/Core/Inc
  -I$PROJ/Drivers/STM32F4xx_HAL_Driver/Inc
  -I$PROJ/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
  -I$PROJ/Drivers/CMSIS/Device/ST/STM32F4xx/Include
  -I$PROJ/Drivers/CMSIS/Include
  -I$COMMON/app
  -I$COMMON/protocol/generated
"
CFLAGS="$MCU $DEFS $INCS -Wall -Wextra -O1 -ffunction-sections -fdata-sections"

mkdir -p "$OUT"

# The common protocol and application layers are compiled for the target here
# even though the Gateway main loop does not call them yet. They are host-tested
# on every push; this proves they also cross-compile cleanly for Cortex-M4
# before anyone wires them in. --gc-sections drops what is unreferenced, so the
# image does not grow.
for src in "$PROJ"/Core/Src/*.c \
           "$PROJ"/Drivers/STM32F4xx_HAL_Driver/Src/*.c \
           "$COMMON"/app/*.c \
           "$COMMON"/protocol/generated/*.c; do
  $CC $CFLAGS -c "$src" -o "$OUT/$(basename "${src%.c}").o"
done

$CC $MCU -c "$PROJ/Core/Startup/startup_stm32f446retx.s" -o "$OUT/startup.o"

$CC $MCU \
  -T"$PROJ/STM32F446RETX_FLASH.ld" \
  --specs=nosys.specs --specs=nano.specs \
  -Wl,--gc-sections -Wl,-Map="$OUT/gateway.map" \
  "$OUT"/*.o -o "$OUT/gateway.elf"

echo "built $MODE:"
$SIZE "$OUT/gateway.elf"
