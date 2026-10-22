#!/usr/bin/env bash
# Build the Gateway firmware with a plain arm-none-eabi toolchain, for CI.
#
#   tools/build_firmware.sh [loopback|dual] [bringup|app] [output-dir]
set -euo pipefail

TOPOLOGY="${1:-loopback}"
RUN="${2:-bringup}"
OUT="${3:-build/firmware-$TOPOLOGY-$RUN}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="$ROOT/firmware/gateway"

case "$TOPOLOGY" in
  loopback) TOPOLOGY_VALUE=0 ;;
  dual)     TOPOLOGY_VALUE=1 ;;
  *) echo "usage: $0 [loopback|dual] [bringup|app] [output-dir]" >&2; exit 2 ;;
esac

case "$RUN" in
  bringup) RUN_VALUE=0 ;;
  app)     RUN_VALUE=1 ;;
  *) echo "usage: $0 [loopback|dual] [bringup|app] [output-dir]" >&2; exit 2 ;;
esac

CROSS="${CROSS_COMPILE:-arm-none-eabi-}"
CC="${CROSS}gcc"
SIZE="${CROSS}size"

COMMON="$ROOT/firmware/common"

MCU="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard"
DEFS="-DUSE_HAL_DRIVER -DSTM32F446xx -DCAN_BRINGUP_MODE=$TOPOLOGY_VALUE -DBFMC_RUN_MODE=$RUN_VALUE"

# e.g. CAN_TX_OPEN_DRAIN=1 tools/build_firmware.sh dual app
if [ -n "${CAN_TX_OPEN_DRAIN:-}" ]; then
  DEFS="$DEFS -DCAN_TX_OPEN_DRAIN=$CAN_TX_OPEN_DRAIN"
fi
INCS="
  -I$PROJ/Core/Inc
  -I$PROJ/Drivers/STM32F4xx_HAL_Driver/Inc
  -I$PROJ/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy
  -I$PROJ/Drivers/CMSIS/Device/ST/STM32F4xx/Include
  -I$PROJ/Drivers/CMSIS/Include
  -I$COMMON/app
  -I$COMMON/uds
  -I$COMMON/boot
  -I$COMMON/protocol/generated
"
CFLAGS="$MCU $DEFS $INCS -Wall -Wextra -O1 -ffunction-sections -fdata-sections"

mkdir -p "$OUT"

# The common layers are cross-compiled even where unused, to prove they build
# for Cortex-M4; --gc-sections keeps them out of the image.
for src in "$PROJ"/Core/Src/*.c \
           "$PROJ"/Drivers/STM32F4xx_HAL_Driver/Src/*.c \
           "$COMMON"/app/*.c \
           "$COMMON"/uds/*.c \
           "$COMMON"/boot/*.c \
           "$COMMON"/protocol/generated/*.c; do
  $CC $CFLAGS -c "$src" -o "$OUT/$(basename "${src%.c}").o"
done

$CC $MCU -c "$PROJ/Core/Startup/startup_stm32f446retx.s" -o "$OUT/startup.o"

$CC $MCU \
  -T"$PROJ/STM32F446RETX_FLASH.ld" \
  --specs=nosys.specs --specs=nano.specs \
  -Wl,--gc-sections -Wl,-Map="$OUT/gateway.map" \
  "$OUT"/*.o -o "$OUT/gateway.elf"

echo "built $TOPOLOGY/$RUN:"
$SIZE "$OUT/gateway.elf"
