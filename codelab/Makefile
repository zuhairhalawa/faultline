# Builds the codelab's client and server with the QNX SDP 8.0 toolchain.
# Run `source ~/qnx800/qnxsdp-env.sh` first. PLATFORM=aarch64le builds for a Raspberry Pi.
PLATFORM ?= x86_64
CC       = qcc -Vgcc_nto$(PLATFORM)
CFLAGS   = -O2 -Wall -Wextra -std=gnu11
BIN      = build/$(PLATFORM)

# Binaries get a cl- prefix so `slay` on a shared target can't hit anything else.
all: $(BIN)/cl-server $(BIN)/cl-client

$(BIN):
	mkdir -p $@

$(BIN)/cl-%: %.c ping.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf build

.PHONY: all clean
