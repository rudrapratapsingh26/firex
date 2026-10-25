# Host build for the pure-C parts of the firmware. Nothing here needs ESP-IDF
# or a board.
#
#   make test     run every Unity suite: fusion, sensors, ledger, alarm, media, core, net, ota, trace
#   make sim      build the harness tools/simulator drives
#   make bundle   export the golden chain through the C ledger into host-build/golden-bundle.json,
#                 for `npm run verify -- firmware/host-build/golden-bundle.json`
#   make all      tests, harness and bundle
#
# Output goes to host-build/, because build/ belongs to idf.py: this directory
# is also the ESP-IDF project (idf.py set-target esp32s3 build).

CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -O2
TESTFLAGS := -DUNITY_OUTPUT_COLOR -DUNITY_SUPPORT_64 -Itest/unity -Itest
BUILD    := host-build

FUSION_INC := -Icomponents/fusion/include
SENSOR_INC := -Icomponents/sensors/include
LEDGER_INC := -Icomponents/ledger/include
ALARM_INC  := -Icomponents/alarm/include
MEDIA_INC  := -Icomponents/media/include
CORE_INC   := -Icomponents/core/include
NET_INC    := -Icomponents/net/include
OTA_INC    := -Icomponents/ota/include

FUSION_SRC := components/fusion/fusion.c
SENSOR_SRC := $(wildcard components/sensors/score/*.c)
LEDGER_SRC := $(wildcard components/ledger/src/*.c)
ALARM_SRC  := $(wildcard components/alarm/src/*.c)
MEDIA_SRC  := $(wildcard components/media/src/*.c)
CORE_SRC   := $(wildcard components/core/src/*.c)
NET_SRC    := $(wildcard components/net/src/*.c)
OTA_SRC    := $(wildcard components/ota/src/*.c)
HEADERS    := $(wildcard components/*/include/*.h) test/ledger_golden.h test/provision_golden.h
UNITY      := test/unity/unity.c

ifeq ($(OS),Windows_NT)
  LIBEXT := .dll
  EXEEXT := .exe
else
  LIBEXT := .so
  EXEEXT :=
endif

LIB    := $(BUILD)/libfusion$(LIBEXT)
SIM    := $(BUILD)/fusion_cli$(EXEEXT)
BUNDLE := $(BUILD)/ledger_bundle$(EXEEXT)
T_FUSION  := $(BUILD)/test_fusion$(EXEEXT)
T_SENSORS := $(BUILD)/test_sensors$(EXEEXT)
T_LEDGER  := $(BUILD)/test_ledger$(EXEEXT)
T_ALARM   := $(BUILD)/test_alarm$(EXEEXT)
T_MEDIA   := $(BUILD)/test_media$(EXEEXT)
T_CORE    := $(BUILD)/test_core$(EXEEXT)
T_NET     := $(BUILD)/test_net$(EXEEXT)
T_OTA     := $(BUILD)/test_ota$(EXEEXT)
TESTS  := $(T_FUSION) $(T_SENSORS) $(T_LEDGER) $(T_ALARM) $(T_MEDIA) $(T_CORE) $(T_NET) $(T_OTA)

.PHONY: all test lib sim bundle clean

all: sim test bundle

$(BUILD):
	@mkdir -p $(BUILD) 2>/dev/null || true

lib: | $(BUILD)
	$(CC) $(CFLAGS) $(FUSION_INC) -shared -fPIC $(FUSION_SRC) -o $(LIB)

$(T_FUSION): $(FUSION_SRC) $(HEADERS) $(UNITY) test/test_fusion.c | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) $(FUSION_INC) $(FUSION_SRC) $(UNITY) test/test_fusion.c -o $@

$(T_SENSORS): $(FUSION_SRC) $(SENSOR_SRC) $(HEADERS) $(UNITY) test/test_sensors.c | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) $(FUSION_INC) $(SENSOR_INC) \
		$(FUSION_SRC) $(SENSOR_SRC) $(UNITY) test/test_sensors.c -o $@ -lm

$(T_LEDGER): $(LEDGER_SRC) $(HEADERS) $(UNITY) test/test_ledger.c | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) $(LEDGER_INC) $(LEDGER_SRC) $(UNITY) test/test_ledger.c -o $@

$(T_ALARM): $(FUSION_SRC) $(ALARM_SRC) $(HEADERS) $(UNITY) test/test_alarm.c | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) $(FUSION_INC) $(ALARM_INC) $(FUSION_SRC) $(ALARM_SRC) $(UNITY) test/test_alarm.c -o $@

$(T_MEDIA): $(LEDGER_SRC) $(MEDIA_SRC) $(HEADERS) $(UNITY) test/test_media.c | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) $(LEDGER_INC) $(MEDIA_INC) $(LEDGER_SRC) $(MEDIA_SRC) $(UNITY) test/test_media.c -o $@

$(T_CORE): $(FUSION_SRC) $(SENSOR_SRC) $(LEDGER_SRC) $(CORE_SRC) $(HEADERS) $(UNITY) test/test_core.c | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) $(FUSION_INC) $(SENSOR_INC) $(LEDGER_INC) $(CORE_INC) \
		$(FUSION_SRC) $(SENSOR_SRC) $(LEDGER_SRC) $(CORE_SRC) $(UNITY) test/test_core.c -o $@ -lm

$(T_NET): $(LEDGER_SRC) $(NET_SRC) $(HEADERS) $(UNITY) test/test_net.c | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) $(LEDGER_INC) $(NET_INC) $(LEDGER_SRC) $(NET_SRC) $(UNITY) test/test_net.c -o $@

$(T_OTA): $(LEDGER_SRC) $(OTA_SRC) $(HEADERS) $(UNITY) test/test_ota.c | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) $(FUSION_INC) $(LEDGER_INC) $(OTA_INC) $(LEDGER_SRC) $(OTA_SRC) $(UNITY) test/test_ota.c -o $@

# The core tests again, built as a production image builds settings.c (M6): cam_frames is
# fixed in firmware.
T_CORE_LOCKED := $(BUILD)/test_core_locked$(EXEEXT)
TESTS         += $(T_CORE_LOCKED)

$(T_CORE_LOCKED): $(FUSION_SRC) $(SENSOR_SRC) $(LEDGER_SRC) $(CORE_SRC) $(HEADERS) $(UNITY) test/test_core.c | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) -DFX_CAM_FRAMES_LOCKED=3 $(FUSION_INC) $(SENSOR_INC) $(LEDGER_INC) $(CORE_INC) \
		$(FUSION_SRC) $(SENSOR_SRC) $(LEDGER_SRC) $(CORE_SRC) $(UNITY) test/test_core.c -o $@ -lm

$(SIM): $(FUSION_SRC) components/fusion/include/fusion.h sim/fusion_cli.c | $(BUILD)
	$(CC) $(CFLAGS) $(FUSION_INC) $(FUSION_SRC) sim/fusion_cli.c -o $@

$(BUNDLE): $(LEDGER_SRC) $(HEADERS) sim/ledger_bundle.c | $(BUILD)
	$(CC) $(CFLAGS) $(LEDGER_INC) -Itest $(LEDGER_SRC) sim/ledger_bundle.c -o $@

# Trace capture (M5): the pre-window, the episode rules and the file format, and the
# golden file the gateway and tools/pilot read (`make trace-golden` rewrites it).
TRACE_INC    := -Icomponents/trace/include
TRACE_SRC    := $(wildcard components/trace/src/*.c)
T_TRACE      := $(BUILD)/test_trace$(EXEEXT)
TRACE_GOLDEN := $(BUILD)/trace_golden$(EXEEXT)
TESTS        += $(T_TRACE)

$(T_TRACE): $(TRACE_SRC) $(HEADERS) $(UNITY) test/test_trace.c test/trace_golden.fxt | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) $(FUSION_INC) $(TRACE_INC) $(TRACE_SRC) $(UNITY) test/test_trace.c -o $@

$(TRACE_GOLDEN): $(TRACE_SRC) $(HEADERS) sim/trace_golden.c | $(BUILD)
	$(CC) $(CFLAGS) $(FUSION_INC) $(TRACE_INC) $(TRACE_SRC) sim/trace_golden.c -o $@

.PHONY: trace-golden
trace-golden: $(TRACE_GOLDEN)
	@./$(TRACE_GOLDEN) test/trace_golden.fxt
	@echo "wrote test/trace_golden.fxt"

sim: $(SIM)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

bundle: $(BUNDLE)
	@./$(BUNDLE) > $(BUILD)/golden-bundle.json
	@echo "wrote $(BUILD)/golden-bundle.json"

clean:
	rm -rf $(BUILD)

# Secure element (M6): the ATECC608B configuration FireX writes, the lock CRC and the
# provisioning steps, and the golden vectors tools/factory checks its Python mirror against
# (`make se-golden` rewrites them). `test` is extended here because its prerequisites were
# read above this section.
SECURE_INC   := -Icomponents/secure/include
SECURE_SRC   := $(wildcard components/secure/src/*.c)
T_SECURE     := $(BUILD)/test_secure$(EXEEXT)
SE_GOLDEN    := $(BUILD)/se_config_golden$(EXEEXT)
TESTS        += $(T_SECURE)

test: $(T_SECURE)

$(T_SECURE): $(SECURE_SRC) components/secure/include/fx_se_config.h $(UNITY) test/test_secure.c test/se_config_golden.h | $(BUILD)
	$(CC) $(CFLAGS) $(TESTFLAGS) $(SECURE_INC) $(SECURE_SRC) $(UNITY) test/test_secure.c -o $@

$(SE_GOLDEN): $(SECURE_SRC) components/secure/include/fx_se_config.h sim/se_config_golden.c | $(BUILD)
	$(CC) $(CFLAGS) $(SECURE_INC) $(SECURE_SRC) sim/se_config_golden.c -o $@

.PHONY: se-golden
se-golden: $(SE_GOLDEN)
	@mkdir -p ../tools/factory/testdata
	@./$(SE_GOLDEN) test/se_config_golden.h ../tools/factory/testdata/se_config_golden.json
	@echo "wrote test/se_config_golden.h and tools/factory/testdata/se_config_golden.json"
