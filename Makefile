# Host tests and firmware build wrappers.
#   make test        build and run the host tests
#   make goldens     regenerate radio/test/golden from radio/ui/prototype.html (runs the prototype in QuickJS)
#   make pngs        render the goldens to radio/build/png at 4x for review
#   make setup       install the pinned SDK, toolchain and Python venv under radio/third_party/
#   make firmware    build the firmware image (radio/build/havoc-radio.bin)
#   make install-image  radio/build/havoc-radio-install.bin, for stock radios (installer/SPEC.md)
#   make installer   the Havoc Radio Installer for an ESP32-C3 SuperMini,
#                    installer/build/havoc-radio-installer.bin, and installer/build/installer.bin
#                    for its self-update (needs ESP-IDF v5.4.2)

ROOT := $(abspath .)
# The Havoc Radio firmware for the ESP8266 radio (radio/SPEC.md).
RADIO := radio
HOST := radio/build/host
GEN := $(HOST)/gen
VENV_PY := radio/third_party/venv/bin/python
PY := $(if $(wildcard $(VENV_PY)),$(VENV_PY),python3)
XTENSA := radio/third_party/xtensa-lx106-elf/bin/xtensa-lx106-elf-
IDF5 ?= $(HOME)/esp/esp-idf
INSTALL_IMAGE := radio/build/havoc-radio-install.bin
HAVOC_VERSION := $(shell sed -n 's/.*kVersion = "\(.*\)";.*/\1/p' $(RADIO)/main/main.cpp)
NODE ?= node
# A 4 MB flash backup of a K-Radio on the stock firmware, if there is one: more stage and region tests run with it.
STOCK_IMAGE ?=

# The Havoc Radio Installer (installer/SPEC.md): host builds of its plain C modules.
INST := installer/build/host
INST_HOST := $(INST)/installer-host
INST_STAGE := $(INST)/install-stage.bin
INST_PARTITIONS := installer/partitions.csv
INST_STAGE_SIZE := $(shell $(PY) installer/tools/make_stage.py --size)
INST_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinstaller/main -Iinstaller/test \
	-DSL_STAGE_MAX=$(INST_STAGE_SIZE)
INST_SRC := installer/main/server.c installer/main/dns_reply.c installer/main/update_session.c \
	installer/main/stage_layout.c installer/host/host_main.c
INST_TESTS := $(INST)/test_stage_layout $(INST)/test_update_session $(INST)/test_records
# Stage files and their images, read back by the C and Python tests.
INST_STAGE_PAIRS := $(INST_STAGE) $(INSTALL_IMAGE) \
	$(if $(wildcard $(STOCK_IMAGE)),$(INST)/stock-stage.bin $(STOCK_IMAGE))
INST_TEST_IMAGES := $(INSTALL_IMAGE) $(wildcard $(STOCK_IMAGE))

CXX ?= c++
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Werror -ffp-contract=off -DSRC_ROOT=\"$(ROOT)\" \
	-I$(RADIO)/components/gfx/include -I$(RADIO)/components/ui/include -I$(RADIO)/components/radio/include \
	-I$(RADIO)/components/input/include -I$(RADIO)/components/si4732/include -I$(RADIO)/components/rds/include \
	-I$(RADIO)/components/clock/include -I$(RADIO)/components/lamp/include -I$(RADIO)/components/guide/include \
	-I$(RADIO)/components/web/include \
	-I$(GEN) -I$(RADIO)/test

LIB_SRC := $(RADIO)/components/gfx/gfx.cpp $(RADIO)/components/ui/ui.cpp $(RADIO)/components/radio/radio_model.cpp \
	$(RADIO)/components/input/input_logic.cpp $(RADIO)/components/rds/rds.cpp \
	$(RADIO)/components/clock/clock_core.cpp $(RADIO)/components/lamp/lamp_core.cpp \
	$(RADIO)/components/guide/guide_core.cpp $(RADIO)/components/guide/guide_slots.cpp \
	$(RADIO)/components/guide/guide_fetch.cpp $(RADIO)/components/web/streams.cpp
TEST_SRC := $(wildcard $(RADIO)/test/*.cpp)
GEN_SRC := $(GEN)/fonts_data.cpp $(GEN)/i18n_strings.cpp
OBJ := $(patsubst %.cpp,$(HOST)/obj/%.o,$(LIB_SRC) $(TEST_SRC)) $(GEN_SRC:.cpp=.o)

.PHONY: test installer-test goldens check-goldens pngs setup firmware install-image installer clean

test: $(HOST)/tests install-image
	$(HOST)/tests
	$(NODE) $(RADIO)/test/test_page.js $(ROOT)
	$(PY) $(RADIO)/test/test_install_image.py
	$(MAKE) installer-test

# installer/SPEC.md, Tests 1 to 4.
installer-test: $(INST_HOST) $(INST_TESTS) $(filter %.bin,$(INST_STAGE_PAIRS))
	$(INST)/test_stage_layout $(INST_STAGE_PAIRS)
	$(INST)/test_update_session
	$(INST)/test_records
	$(PY) installer/test/test_make_stage.py $(INST_STAGE_PAIRS)
	$(PY) installer/test/test_dns.py $(INST_HOST) $(INST_STAGE)
	$(PY) installer/test/test_protocol.py $(INST_HOST) $(INST_STAGE) $(INSTALL_IMAGE)
	$(PY) installer/test/test_radio_flash.py
	$(PY) installer/test/test_installer_update.py
	$(PY) installer/test/test_page.py $(NODE) $(INST_TEST_IMAGES)

$(INST_STAGE): $(INSTALL_IMAGE) installer/tools/make_stage.py installer/tools/radio-flash $(INST_PARTITIONS)
	@mkdir -p $(dir $@)
	$(PY) installer/tools/make_stage.py --image $< --label "Havoc Radio $(HAVOC_VERSION)" --out $@

$(INST)/stock-stage.bin: $(STOCK_IMAGE) installer/tools/make_stage.py installer/tools/radio-flash $(INST_PARTITIONS)
	@mkdir -p $(dir $@)
	$(PY) installer/tools/make_stage.py --image $< --label "$(notdir $<)" --out $@

$(INST)/test_%: installer/test/test_%.c installer/main/%.c installer/main/%.h installer/test/check.h $(INST_PARTITIONS)
	@mkdir -p $(dir $@)
	$(CC) $(INST_CFLAGS) -fsanitize=address,undefined $(filter %.c,$^) -o $@

$(GEN)/fonts_data.cpp: $(RADIO)/ui/prototype.html $(RADIO)/tools/gen_fonts.py
	@mkdir -p $(GEN)
	$(PY) $(RADIO)/tools/gen_fonts.py $< $@

$(GEN)/i18n_strings.cpp $(GEN)/i18n_strings.h: $(RADIO)/ui/prototype.html $(RADIO)/tools/gen_strings.py
	@mkdir -p $(GEN)
	$(PY) $(RADIO)/tools/gen_strings.py $< $(GEN)

$(HOST)/obj/%.o: %.cpp $(GEN)/i18n_strings.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(GEN)/%.o: $(GEN)/%.cpp $(GEN)/i18n_strings.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(HOST)/tests: $(OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

goldens:
	$(VENV_PY) $(RADIO)/tools/gen_goldens.py $(RADIO)/ui/prototype.html $(RADIO)/test/scenarios.txt $(RADIO)/test/golden

check-goldens:
	$(VENV_PY) $(RADIO)/tools/gen_goldens.py --check $(RADIO)/ui/prototype.html $(RADIO)/test/scenarios.txt \
		$(RADIO)/test/golden

pngs:
	$(PY) $(RADIO)/tools/pbm2png.py $(RADIO)/test/golden/*.pbm radio/build/png

setup:
	python3 $(RADIO)/tools/setup_sdk.py

firmware:
	./$(RADIO)/tools/idf.sh build

install-image: firmware
	$(PY) $(RADIO)/tools/make_install_image.py --build-dir radio/build --toolchain $(XTENSA) \
		--stub $(RADIO)/tools/install_stub.s \
		--out $(INSTALL_IMAGE)

$(INST_HOST): $(INST_SRC) $(wildcard installer/main/*.h) $(INST_PARTITIONS)
	@mkdir -p $(dir $@)
	$(CC) $(INST_CFLAGS) $(INST_SRC) -o $@

# installer/SPEC.md, "Build", and Tests 5.
installer: $(INST_STAGE)
	bash -c '. "$(IDF5)/export.sh" >/dev/null && cd installer && idf.py build && cd build && \
		esptool.py --chip esp32c3 merge_bin -o havoc-radio-installer.bin @flash_args \
		$(shell $(PY) installer/tools/make_stage.py --offset) $(ROOT)/$(INST_STAGE)'
	$(PY) installer/test/check_merged.py installer/build $(INST_STAGE) $(INSTALL_IMAGE) \
		docs/images/installer-wiring.svg

clean:
	rm -rf $(HOST)

-include $(shell find $(HOST)/obj -name '*.d' 2>/dev/null)
