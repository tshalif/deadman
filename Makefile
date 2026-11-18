obj-m := deadman.o
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

SIGN_FILE ?= $(KDIR)/scripts/sign-file
SIGN_HASH ?= sha256
SIGN_KEY ?= signing_key.priv
SIGN_CERT ?= signing_key.x509
SYSTEM_MOK_KEY ?= /var/lib/shim-signed/mok/MOK.priv
SYSTEM_MOK_CERT ?= /var/lib/shim-signed/mok/MOK.der

.PHONY: default all modules user sign enroll-key clean

default: modules sign user

all: default

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

user: wayland-xwrits-bridge libxwrits-sendevent.so

wayland-xwrits-bridge: wayland-xwrits-bridge.c
	$(CC) $(CFLAGS) -o $@ $< -ldl

libxwrits-sendevent.so: libxwrits-sendevent.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -ldl

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f wayland-xwrits-bridge libxwrits-sendevent.so

$(SIGN_KEY) $(SIGN_CERT): x509.genkey
	openssl req -x509 -new -nodes -utf8 -sha256 -days 36500 \
		-batch -config x509.genkey -outform DER \
		-out $(SIGN_CERT) \
		-keyout $(SIGN_KEY)
	chmod 600 $(SIGN_KEY)

enroll-key: $(SIGN_CERT)
	@if command -v mokutil >/dev/null 2>&1 && mokutil --test-key "$(SIGN_CERT)" >/dev/null 2>&1; then \
		echo "MOK certificate $(SIGN_CERT) is already enrolled in UEFI MOK."; \
	else \
		sudo mokutil --import "$(SIGN_CERT)"; \
		echo "IMPORTANT: Reboot now and select 'Enroll MOK' -> 'Continue' -> 'Yes' in the blue UEFI MokManager screen."; \
	fi

sign: modules
	@if [ -f "$(SYSTEM_MOK_CERT)" ] && command -v mokutil >/dev/null 2>&1 && \
	    mokutil --test-key "$(SYSTEM_MOK_CERT)" >/dev/null 2>&1 && \
	    ( [ ! -f "$(SIGN_CERT)" ] || ! mokutil --test-key "$(SIGN_CERT)" >/dev/null 2>&1 ); then \
		echo "Signing deadman.ko with enrolled system MOK key ($(SYSTEM_MOK_CERT))..."; \
		sudo "$(SIGN_FILE)" "$(SIGN_HASH)" "$(SYSTEM_MOK_KEY)" "$(SYSTEM_MOK_CERT)" deadman.ko; \
	else \
		if [ ! -f "$(SIGN_KEY)" ] || [ ! -f "$(SIGN_CERT)" ]; then \
			$(MAKE) "$(SIGN_KEY)" "$(SIGN_CERT)"; \
		fi; \
		echo "Signing deadman.ko with $(SIGN_CERT)..."; \
		"$(SIGN_FILE)" "$(SIGN_HASH)" "$(SIGN_KEY)" "$(SIGN_CERT)" deadman.ko; \
		if command -v mokutil >/dev/null 2>&1 && mokutil --sb-state 2>/dev/null | grep -q "SecureBoot enabled" && \
		   ! mokutil --test-key "$(SIGN_CERT)" >/dev/null 2>&1; then \
			echo "WARNING: Secure Boot is enabled, but $(SIGN_CERT) is not yet enrolled in the kernel's MOK list."; \
			echo "         Run 'make enroll-key' and reboot (selecting 'Enroll MOK' in MokManager) before insmod."; \
		fi; \
	fi
