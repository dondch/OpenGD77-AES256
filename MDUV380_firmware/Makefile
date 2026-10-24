# Auto-generated Makefile to build OpenGD77 MD-UV390 PLUS 10W firmware
# (mirrors STM32CubeIDE config MDUV380_10W_PLUS_FW). arm-none-eabi-gcc.
TARGET   = openuv380-10w
BUILD    = build
CROSS    = arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size

CPU   = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
GITVERSION := $(shell git rev-parse --short HEAD 2>/dev/null || echo UNKNOWN)
DEFS  = -DGITVERSION=$(GITVERSION) -DUSE_HAL_DRIVER -DSTM32F405xx -DPLATFORM_MDUV380 -DPLATFORM_VARIANT_UV380_PLUS_10W -DNDEBUG
INCS  = -I. -ICore/Inc \
        -IDrivers/CMSIS/Device/ST/STM32F4xx/Include -IDrivers/CMSIS/Include \
        -IDrivers/STM32F4xx_HAL_Driver/Inc -IDrivers/STM32F4xx_HAL_Driver/Inc/Legacy \
        -IMiddlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc \
        -IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc \
        -IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 \
        -IMiddlewares/Third_Party/FreeRTOS/Source/include \
        -IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F \
        -IUSB_DEVICE/App -IUSB_DEVICE/Target -Iapplication/include

# ---------------------------------------------------------------------------
# Build flags
#
# Plain `make` is the release build: DMRA AES-256 encrypted voice is always compiled in,
# every optional flag is off. A flag is on only when set to 1 on the command line, e.g.
# `make ENABLE_FAST_SCAN=1`, or through a named profile, `make PROFILE=full`. The RAM
# cost of each flag and the measured headroom of each profile are in BUILD.md.
#
# Feature flags (user-facing, each one a switch so a build without it is unchanged):
#
#   ENABLE_DMR_DATA        Encrypted DMR SMS (Messages menu, store, confirmed delivery) and
#                          the raw data-frame TX harness (tools/dmr_data_tx.py). The most
#                          expensive flag: it nearly fills CCM RAM.
#   ENABLE_FAST_SCAN       Faster ANALOG scanning: adds sub-30 ms steps (20/15/10/6 ms) to
#                          the "Scan dwell time" setting, selectable on the radio and via
#                          CHIRP. Digital scanning is unchanged (it takes the larger of this
#                          and its own DMR timeslot minimum).
#   ENABLE_DUAL_SCREEN     Dual screen (two rows, A and B, each a VFO or a zone channel, like
#                          the Anytone radios), the row watch, and the Dual Watch options menu
#                          (home side, speed, stay, channel as side B). Ported from
#                          erenbozaci/md-uv390-firmware (Eren TA3BZC).
#   ENABLE_TA2TWC_SCREENS  The TA2TWC screens (josephddt/OpenGD77-TA2TWC): redesigned Channel
#                          home screen with the S-meter, dBm, signal quality, battery voltage,
#                          temperature and a signal history, caller and TX screens, telemetry
#                          lines on the VFO screen, an analog S-meter on the RSSI screen, boot
#                          and shutdown animations, a dark default theme and compact 9-line
#                          menus. With ENABLE_DUAL_SCREEN as well, the dual screen takes the
#                          home screen over while its option is on.
#
# Dev flags (bench and diagnostics; never in a release image):
#
#   ENABLE_SPECTRUM        Swept-RSSI receiver driven over USB (tools/sweep.py, settle.py) and
#                          the VFO sweep waterfall. NOT an SDR: the AT1846S has no IQ output,
#                          only an RSSI register.
#   ENABLE_SCAN_PROFILER   DWT scan-step profiler (CPS 0xA9, tools/scanprof.py). Costs 544
#                          bytes of RAM for the slot table, so it is separate from
#                          ENABLE_SPECTRUM, which it REQUIRES (the implementation lives in
#                          spectrum.c).
#   ENABLE_KEY_INJECTION   USB display mirror + remote key injection (fbmirror.py / rkey.py),
#                          lets a PC drive the UI, read the framebuffer and enter DFU.
#   ENABLE_GPS_NMEA_LOG    GPS NMEA logging to SPI flash (defines LOG_GPS_DATA). Its 4 KB
#                          NMEARecordingBuffer is the single largest optional SRAM user, and
#                          the buffer's .data placement also adds 4 KB of zeros to the image.
#   DMR_AES_DIAG_RX        AES RX event ring + one-superframe raw-burst capture, read with
#                          tools/rxdiag.py / tools/fragcap.py. State lives in CCM.
#   DMR_AES_DIAG_PATTERN   TX diagnostic: inject known AMBE parameters instead of voice.
#   DMR_AES_DIAG_ENCPAT    TX diagnostic: constant known plaintext, then encrypted.
#
# Build profiles (PROFILE=<name>); `make profiles` lists them:
#
#   release    (default) nothing on
#   full       every feature flag
#   dev-sweep  ENABLE_SPECTRUM ENABLE_FAST_SCAN ENABLE_KEY_INJECTION  -- sweep / waterfall bench
#   dev-prof   ENABLE_SPECTRUM ENABLE_SCAN_PROFILER ENABLE_KEY_INJECTION  -- scanprof.py
#   dev-aes    DMR_AES_DIAG_RX ENABLE_KEY_INJECTION  -- AES RX diagnostics
#   dev-gps    ENABLE_GPS_NMEA_LOG
#
# There is no "everything" profile. Since the notification save buffer shrank to an 80-row
# band and the AMBE codec state moved to CCM (2026-10-24), every flag combination links
# (all flags: 14,756 B SRAM spare, "full" profile: 15,556 B SRAM / 6,920 B CCM spare), but the
# dev flags are still meant to be enabled one at a time on top of a feature profile.
# ---------------------------------------------------------------------------
FEATURE_FLAGS = ENABLE_DMR_DATA ENABLE_FAST_SCAN ENABLE_DUAL_SCREEN ENABLE_TA2TWC_SCREENS
DEV_FLAGS     = ENABLE_SPECTRUM ENABLE_SCAN_PROFILER ENABLE_KEY_INJECTION ENABLE_GPS_NMEA_LOG \
                DMR_AES_DIAG_RX DMR_AES_DIAG_PATTERN DMR_AES_DIAG_ENCPAT
ALL_FLAGS     = $(FEATURE_FLAGS) $(DEV_FLAGS)
PROFILES      = release full dev-sweep dev-prof dev-aes dev-gps

PROFILE ?= release
ifeq ($(PROFILE),release)
else ifeq ($(PROFILE),full)
ENABLE_DMR_DATA = 1
ENABLE_FAST_SCAN = 1
ENABLE_DUAL_SCREEN = 1
ENABLE_TA2TWC_SCREENS = 1
else ifeq ($(PROFILE),dev-sweep)
ENABLE_SPECTRUM = 1
ENABLE_FAST_SCAN = 1
ENABLE_KEY_INJECTION = 1
else ifeq ($(PROFILE),dev-prof)
ENABLE_SPECTRUM = 1
ENABLE_SCAN_PROFILER = 1
ENABLE_KEY_INJECTION = 1
else ifeq ($(PROFILE),dev-aes)
DMR_AES_DIAG_RX = 1
ENABLE_KEY_INJECTION = 1
else ifeq ($(PROFILE),dev-gps)
ENABLE_GPS_NMEA_LOG = 1
else
$(error Unknown PROFILE '$(PROFILE)'. Known profiles: $(PROFILES))
endif

# Dependencies between flags, checked here so a wrong combination fails in one line instead
# of deep inside a compile.
ifeq ($(ENABLE_SCAN_PROFILER),1)
ifneq ($(ENABLE_SPECTRUM),1)
$(error ENABLE_SCAN_PROFILER=1 needs ENABLE_SPECTRUM=1: the profiler is implemented in spectrum.c)
endif
endif
ifeq ($(DMR_AES_DIAG_PATTERN)$(DMR_AES_DIAG_ENCPAT),11)
$(error DMR_AES_DIAG_PATTERN and DMR_AES_DIAG_ENCPAT are mutually exclusive TX diagnostics)
endif

ENABLED_FLAGS = $(strip $(foreach f,$(ALL_FLAGS),$(if $(filter 1,$($(f))),$(f))))
DEFS += $(addprefix -D,$(ENABLED_FLAGS))

# RAM headroom floors, checked after every link (see the size report; 134697852 = 0x0807537C,
# the fixed AMBE codec slot). SRAM below the floor
# means the main stack and the libc heap are being squeezed: the linker only reserves
# 0x400 + 0x200 bytes for them. `make RAM_FLOOR=0` to look at an over-full build anyway.
RAM_FLOOR ?= 1024
CCM_FLOOR ?= 0

# REPRODUCIBLE=1 pins the compiled-in build date and time to the commit time, so two builds of
# one commit are byte-identical (the firmware-info screen then shows the commit time).
# Pass the same GITVERSION=<hash> as well when comparing trees at different commits.
ifeq ($(REPRODUCIBLE),1)
export SOURCE_DATE_EPOCH ?= $(shell git log -1 --format=%ct 2>/dev/null || echo 0)
endif

CFLAGS  = $(CPU) $(DEFS) $(INCS) -Os -g3 -ffunction-sections -fdata-sections -Wall -MMD -MP
LDSCRIPT = STM32F405VGTX_FLASH.ld
LDFLAGS = $(CPU) -T$(LDSCRIPT) --specs=nano.specs -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map -lc -lm -lnosys

C_SRCS   = $(sort $(shell find Core Drivers Middlewares USB_DEVICE application -name *.c ! -name languages_builder.c ! -name *_template.c))
ASM_SRCS = Core/Startup/startup_stm32f405vgtx.s
ASM_SRCS_S = $(sort $(shell find application -name *.S))
OBJS = $(addprefix $(BUILD)/,$(C_SRCS:.c=.o)) $(addprefix $(BUILD)/,$(ASM_SRCS:.s=.o)) $(addprefix $(BUILD)/,$(ASM_SRCS_S:.S=.o))
DEPS = $(OBJS:.o=.d)

.DEFAULT_GOAL := all
LINKERDATA_BIN = application/source/linkerdata/codec_bin_section_1.bin
$(LINKERDATA_BIN):
	@chmod +x tools/codec_cleaner.Linux
	cd application/source/linkerdata && ../../../tools/codec_cleaner.Linux -C

$(BUILD)/application/source/dmr_codec/codec_bin.o: $(LINKERDATA_BIN)

all: $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -I. -Iapplication -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@$(SIZE) -A $@ | awk -v flags="$(if $(ENABLED_FLAGS),$(ENABLED_FLAGS),none)" \
	    -v ramfloor=$(RAM_FLOOR) -v ccmfloor=$(CCM_FLOOR) ' \
	  /^\.(isr_vector|text|rodata|data|bss|ccmram|_user_heap_stack|codec_bin_section_1) /{sz[$$1]=$$2; ad[$$1]=$$3} \
	  END { code = sz[".isr_vector"] + sz[".text"] + sz[".rodata"]; \
	        gap  = 134697852 - ad[".codec_bin_section_1"]; \
	        ram  = sz[".data"] + sz[".bss"] + sz["._user_heap_stack"]; \
	        ccm  = sz[".ccmram"]; \
	        printf "size report  flags: %s\n", flags; \
	        printf "  code  %7d bytes, %7d bytes free before the codec slot\n", code, gap; \
	        printf "  SRAM  %7d of 131072 bytes, %6d spare (floor %d)\n", ram, 131072 - ram, ramfloor; \
	        printf "  CCM   %7d of  65536 bytes, %6d spare (floor %d)\n", ccm, 65536 - ccm, ccmfloor; \
	        bad = 0; \
	        if (131072 - ram < ramfloor) { printf "ERROR: SRAM headroom below RAM_FLOOR\n"; bad = 1 } \
	        if (65536 - ccm < ccmfloor)  { printf "ERROR: CCM headroom below CCM_FLOOR\n";  bad = 1 } \
	        if (gap < 0)                 { printf "ERROR: code overlaps the AMBE codec slot\n"; bad = 1 } \
	        exit bad }' || { rm -f $@; exit 1; }

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

profiles:
	@echo $(PROFILES)

flags:
	@echo $(if $(ENABLED_FLAGS),$(ENABLED_FLAGS),none)

clean:
	rm -rf $(BUILD)

.PHONY: all clean profiles flags

-include $(DEPS)
