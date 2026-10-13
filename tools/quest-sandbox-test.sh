#!/usr/bin/env bash
# Runs the ARM64 core on a connected Quest inside the host app's sandbox, without the headset
# having to be worn or unlocked: no display, scripted controller input, a screenshot every N
# presented frames. The app's SandboxShell instrumentation starts the process, so it runs under
# the same security domain and system-call filter as when the app itself launches it (adb shell
# and run-as do not: neither may allocate GPU memory on Quest).
#
#   tools/quest-sandbox-test.sh <seconds> [input script] [screenshot interval] [NAME=value ...]
#
# The core gets the settings the app gives it by default (no multisampling: SHADPS4_MAX_MSAA=1,
# the driver's direct rendering: TU_DEBUG=sysmem, a headset of 90 Hz: SHADPS4_VR_REFRESH_RATE=90,
# which has the game draw 45 frames a second, or 30 where that is all it manages, and the
# picture sharpened: SHADPS4_VR_SHARPEN=0.6); a NAME=value given here replaces them.
# The sandbox is not a VR session: the process may only use processors 0-2 there (a VR app gets
# three others), and the GPU's clock follows the load up to 690 MHz (a VR session holds it at
# 599 at most).
#
# Needs the app installed (quest-host/build.sh), the runtime staged in build/quest/runtime and
# the game in /data/local/tmp/astro/games/CUSA12392. Results land in build/quest/run/.
#
# Settings of the script itself come from the environment:
#   ASTRO_DRIVER   folder of the Vulkan driver inside the app's files folder
#                  (default runtime/drivers/turnip; drivers/<name> holds the other bundled builds)
#   ASTRO_DUMP_AT  seconds after which the core dumps its render targets (user/rt_dump)
#   ASTRO_TOP_AT   seconds after which the busiest threads of the core are listed (run/top.txt)
#   ASTRO_MEM_EVERY  seconds between samples of the core's memory use (run/mem.txt)
#   ASTRO_THREADS_AT  seconds after which the core logs what each guest thread is doing and
#                  samples the native stack of every thread (ASTRO_THREAD_SAMPLES times, 12 by
#                  default); tools/symbolize-stacks.sh turns the addresses into functions
#   ASTRO_AT       "<seconds>:<command>;<seconds>:<command>..." runs shell commands in the
#                  app's files folder at those times (no single quotes in them). With
#                  TU_DEBUG_FILE=$PWD/tu_debug.txt given as a setting of the core, that is how the
#                  driver's rendering mode is switched while the game runs:
#                  ASTRO_AT="300:printf sysmem > tu_debug.txt;340:printf gmem > tu_debug.txt"
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
# The headset also shows up over the network once wireless debugging has been used: name the
# USB connection so that adb does not have to guess.
export ANDROID_SERIAL=${ANDROID_SERIAL:-$(cat "$(dirname "${BASH_SOURCE[0]}")/quest-serial.local" 2>/dev/null)}
# (ASTRO_PACKAGE: the build made by quest-host/build.sh --test-package, if that is the
# one to run.)
package=${ASTRO_PACKAGE:-com.astrobotquest.vrhost}
stage=/data/local/tmp/astro
game=${ASTRO_GAME:-$stage/games/CUSA12392/eboot.bin}
driver=${ASTRO_DRIVER:-runtime/drivers/turnip}
dump_at=${ASTRO_DUMP_AT:-}
dump_command=""
top_at=${ASTRO_TOP_AT:-}
top_command=""
if [[ -n "$top_at" ]]; then
  top_command="(sleep $top_at; top -H -b -n 3 -d 4 -m 45 -p \$(pidof libastro_ld.so) > top.txt 2>&1) &"
fi
threads_at=${ASTRO_THREADS_AT:-}
threads_command=""
if [[ -n "$threads_at" ]]; then
  threads_command="(sleep $threads_at; echo ${ASTRO_THREAD_SAMPLES:-12} > user/dump_threads) &"
fi
mem_every=${ASTRO_MEM_EVERY:-}
mem_command=""
if [[ -n "$mem_every" ]]; then
  mem_command="sh $stage/memwatch.sh $mem_every > mem.txt 2>&1 &"
fi
if [[ -n "$dump_at" ]]; then
  dump_command="(sleep $dump_at; touch user/dump_render_targets) &"
fi
# ASTRO_GPUTRACE_AT=<seconds>: the driver's own trace of what the GPU does (every render pass
# with the time the GPU reached it) from that moment on, ASTRO_GPUTRACE_BYTES of it (12 MB, a
# few seconds). The driver writes the trace from start to end; a pipe lets all but the wanted
# part go to waste. tools/gputrace.py sums the result (run/gputrace.txt) up.
trace_at=${ASTRO_GPUTRACE_AT:-}
trace_command=""
trace_env=""
if [[ -n "$trace_at" ]]; then
  trace_command="rm -f gputrace.fifo gputrace.txt; mkfifo gputrace.fifo
(timeout $trace_at cat > /dev/null; head -c ${ASTRO_GPUTRACE_BYTES:-12000000} > gputrace.txt; cat > /dev/null) < gputrace.fifo &"
  trace_env="MESA_GPU_TRACES=print MESA_GPU_TRACEFILE=\$PWD/gputrace.fifo"
fi
at_commands=""
if [[ -n "${ASTRO_AT:-}" ]]; then
  IFS=';' read -ra at_entries <<< "$ASTRO_AT"
  for entry in "${at_entries[@]}"; do
    at_commands+="(sleep ${entry%%:*}; ${entry#*:}) &
"
  done
fi

seconds=${1:-60}
script=${2:-}
shots=${3:-600}
shift $(( $# < 3 ? $# : 3 ))
extra_env="$*"

cd "$root" || exit 1
mkdir -p build/quest/run
tools/llvm/bin/llvm-strip.exe -o build/quest/shadps4-stripped build/arm64/shadps4 || exit 1
tools/llvm/bin/aarch64-linux-gnu-clang.exe -O2 -fPIC -shared quest-host/runtime/kgsl_compat.c \
  -o build/quest/libkgsl_compat.so -lpthread || exit 1

# Stage through /data/local/tmp, which adb can write and the app can read.
"$ADB" push build/quest/shadps4-stripped $stage/shadps4-arm64-fex | tail -1
"$ADB" push build/quest/libkgsl_compat.so $stage/libkgsl_compat.so | tail -1
input_env=""
if [[ -n "$script" ]]; then
  "$ADB" push "$script" $stage/input.txt | tail -1
  input_env="SHADPS4_INPUT_SCRIPT=\$PWD/input.txt"
fi
"$ADB" push tools/quest-memwatch.sh $stage/memwatch.sh > /dev/null
"$ADB" shell "chmod 644 $stage/shadps4-arm64-fex $stage/libkgsl_compat.so $stage/input.txt $stage/memwatch.sh 2>/dev/null"

# Everything below runs inside the sandbox, in the app's files folder.
command="
mkdir -p runtime/host user home/cache tmp
rm -f runtime/.stamp
[ -d runtime/drivers ] || cp -r $stage/runtime/. runtime/
cp $stage/shadps4-arm64-fex runtime/host/shadps4-arm64-fex
cp $stage/libkgsl_compat.so runtime/host/libkgsl_compat.so
[ -f $stage/input.txt ] && cp $stage/input.txt input.txt
rm -rf user/screenshots user/rt_dump user/dump_render_targets user/*.wav core.log top.txt mem.txt
echo $shots > user/auto_shot_every
: > tu_debug.txt
$dump_command
$at_commands
$trace_command
$top_command
$threads_command
$mem_command
env HOME=\$PWD/home XDG_DATA_HOME=\$PWD/home/data XDG_CACHE_HOME=\$PWD/home/cache \
  XDG_CONFIG_HOME=\$PWD/home/config TMPDIR=\$PWD/tmp MESA_SHADER_CACHE_DIR=\$PWD/home/cache/mesa \
  GLIBC_TUNABLES=glibc.pthread.rseq=0 \
  LD_LIBRARY_PATH=\$PWD/runtime/host:\$PWD/$driver \
  VK_ICD_FILENAMES=\$PWD/$driver/freedreno_icd.aarch64.json \
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SHADPS4_HEADLESS=1 SHADPS4_MAX_MSAA=1 \
  TU_DEBUG=sysmem KGSL_COMPAT_PRIORITY=12 SHADPS4_VR_REFRESH_RATE=90 SHADPS4_VR_SHARPEN=0.6 \
  SHADPS4_THREAD_NICE=Game:Main=-14,shadPS4:GpuComm=-14,sndx_out_thread=-16,sndx_audio3d=-16,shadPS4:AudioOu=-16,shadPS4:VrHostL=-18,shadPS4:Present=-18 \
  $input_env $trace_env $extra_env \
  timeout $seconds \$ASTRO_LIB_DIR/libastro_ld.so \
  --library-path \$PWD/runtime/host:\$PWD/$driver \
  --preload \$PWD/runtime/host/libkgsl_compat.so \
  runtime/host/shadps4-arm64-fex -g $game > core.log 2>&1
echo exit=\$? >> core.log
tail -c 2000 core.log
"
# The run, and a look every five seconds at how busy the GPU is and how fast it and the processors
# are clocked (the shell may read that, the app may not; one line per sample in run/gpu.txt:
# seconds, busy%, GPU MHz, GPU temperature, MHz of each processor), go on on the headset by
# themselves: another program on this PC with an adb of its own can restart the adb server in
# the middle of a run, which ends whatever is still attached to it. This side only looks in until
# the run is over.
cat > build/quest/run/run-instrument.sh <<EOF
am instrument -w -e cmd '$command' $package/com.astrobotquest.vrhost.SandboxShell > $stage/instrument.txt 2>&1
touch $stage/run.done
EOF
cat > build/quest/run/gpu-sampler.sh <<'EOF'
start=$(date +%s)
while [ ! -f /data/local/tmp/astro/run.done ]; do
  echo "$(( $(date +%s) - start )) $(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage | tr -d " %") $(( $(cat /sys/class/kgsl/kgsl-3d0/gpuclk) / 1000000 )) $(( $(cat /sys/class/kgsl/kgsl-3d0/temp) / 1000 )) $(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq | while read f; do printf "%s " $(( f / 1000 )); done)"
  sleep 5
done
EOF
(cd build/quest/run && "$ADB" push run-instrument.sh $stage/run-instrument.sh > /dev/null &&
  "$ADB" push gpu-sampler.sh $stage/gpu-sampler.sh > /dev/null)
"$ADB" shell "rm -f $stage/run.done $stage/instrument.txt $stage/gpu.txt"
"$ADB" shell "nohup sh $stage/gpu-sampler.sh > $stage/gpu.txt 2>&1 &"
"$ADB" shell "nohup sh $stage/run-instrument.sh > /dev/null 2>&1 &"
waited=0
until "$ADB" shell "[ -f $stage/run.done ]" > /dev/null 2>&1; do
  sleep 5
  waited=$((waited + 5))
  if (( waited > seconds + 300 )); then
    echo "the run did not end in time"
    break
  fi
done
"$ADB" exec-out "cat $stage/instrument.txt" > build/quest/run/instrument.txt 2>/dev/null
"$ADB" exec-out "cat $stage/gpu.txt" > build/quest/run/gpu.txt 2>/dev/null

rm -rf build/quest/run/screenshots build/quest/run/log build/quest/run/rt_dump
# A core stuck in a fault loop writes gigabytes; the beginning and the end tell the story.
# (Stack samples are long and sit in the middle: with them, up to 60 MB are fetched.)
if [[ -n "$threads_at" ]]; then
  "$ADB" exec-out "run-as $package sh -c 'head -c 60000000 files/core.log'" > build/quest/run/core.log
else
  "$ADB" exec-out "run-as $package sh -c 'head -c 6000000 files/core.log; echo; echo ...; tail -c 400000 files/core.log'"   > build/quest/run/core.log
fi
# The GPU profile's tables (SHADPS4_GPU_PROFILE), and the frame rate and the choices of pace and
# size all along, can sit in the part of a long log left out above.
"$ADB" exec-out "run-as $package sh -c 'grep -a vk_gpu_profile files/core.log'"   > build/quest/run/profile.txt 2>/dev/null
"$ADB" exec-out "run-as $package sh -c 'grep -a -e known_title -e \"frame stats\" -e \"frame path\" files/core.log'"   > build/quest/run/pacing.txt 2>/dev/null
rm -f build/quest/run/*.wav
"$ADB" exec-out "run-as $package sh -c 'cd files/user && tar -cf - screenshots log rt_dump *.wav 2>/dev/null'" \
  | tar -xf - -C build/quest/run 2>/dev/null
"$ADB" exec-out "run-as $package cat files/top.txt 2>/dev/null" > build/quest/run/top.txt
"$ADB" exec-out "run-as $package cat files/mem.txt 2>/dev/null" > build/quest/run/mem.txt
if [[ -n "$trace_at" ]]; then
  "$ADB" exec-out "run-as $package cat files/gputrace.txt 2>/dev/null" > build/quest/run/gputrace.txt
fi
ls build/quest/run/screenshots 2>/dev/null | wc -l
tail -1 build/quest/run/core.log
