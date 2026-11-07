# Builds the Faultline target programs with the QNX SDP 8.0 toolchain.
# Run `source ~/qnx800/qnxsdp-env.sh` first. PLATFORM=aarch64le builds for a Raspberry Pi.
PLATFORM ?= x86_64
CC       = qcc -Vgcc_nto$(PLATFORM)
CFLAGS   = -O2 -Wall -Wextra -std=gnu11
BIN      = build/$(PLATFORM)
PROGS    = fl-supervisor fl-plant flow-sensor motor-drv pump-ctrl fl-inject fl-bridge

LDLIBS_fl-bridge = -lsocket
LDLIBS_fl-plant  = -lm

all: $(addprefix $(BIN)/,$(PROGS))

$(BIN):
	mkdir -p $@

$(BIN)/%: %.c fl.c fl.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< fl.c $(LDLIBS_$*)

clean:
	rm -rf build

.PHONY: all clean
