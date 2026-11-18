# Host build: an Atmos program of this repository, compiled by
# ia-atmos/lang with the drivers simulated on the host (drv/host/), to
# test with no board. The board build is ESP-IDF, in esp32/.
#
#   make host       builds app/init.atm into build-host/init
#   make test       runs tst/*.atm and app/init.atm
#
# The compiler is the submodule ext/lang (or LANG_DIR=<checkout>), which
# must be built first: `git submodule update --init ext/lang` and
# `make -C ext/lang`.

LANG_DIR ?= ext/lang
ATMOS    := $(abspath $(LANG_DIR))/atmos

ENV   := env/esp32.atm
DRV   := drv/drv.h drv/err.c drv/host/host.c
ENVS  := $(wildcard env/*.atm)

HOST_CFLAGS  := -O1 -I$(CURDIR)/drv
HOST_LDFLAGS := $(CURDIR)/drv/err.c $(CURDIR)/drv/host/host.c

BUILD := build-host

all: host

host: $(BUILD)/init

$(BUILD)/init: app/init.atm $(ENVS) $(DRV)
	@mkdir -p $(BUILD)
	ATMOS_CFLAGS="$(HOST_CFLAGS)" ATMOS_LDFLAGS="$(HOST_LDFLAGS)" \
	    $(ATMOS) -e $(ENV) -o $@ $<

$(BUILD)/tst/init: app/init.atm $(ENVS) $(DRV)
	@mkdir -p $(BUILD)/tst
	ATMOS_CFLAGS="$(HOST_CFLAGS)" ATMOS_LDFLAGS="$(HOST_LDFLAGS)" \
	    $(ATMOS) -e $(ENV) -o $@ $<

$(BUILD)/tst/%: tst/%.atm $(ENVS) $(DRV)
	@mkdir -p $(BUILD)/tst
	ATMOS_CFLAGS="$(HOST_CFLAGS)" ATMOS_LDFLAGS="$(HOST_LDFLAGS)" \
	    $(ATMOS) -e $(ENV) -o $@ $<

# Each tst/<t>.atm runs with tst/<t>.script (if any) as its input, and
# its output must be tst/<t>.out. The test `init` is app/init.atm.
TSTS := $(wildcard tst/*.atm) tst/init.atm

test: $(TSTS:tst/%.atm=$(BUILD)/tst/%)
	@for t in $(TSTS:tst/%.atm=%); do \
	    s=tst/$$t.script; [ -f $$s ] || s=/dev/null; \
	    ESP32_HOST_TRACE=1 ESP32_HOST_SCRIPT=$$s $(BUILD)/tst/$$t \
	        > $(BUILD)/tst/$$t.out 2>&1; \
	    if diff -u tst/$$t.out $(BUILD)/tst/$$t.out; then \
	        echo "ok   $$t"; \
	    else \
	        echo "FAIL $$t"; exit 1; \
	    fi; \
	done

clean:
	rm -rf $(BUILD)

.PHONY: all host test clean
