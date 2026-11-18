obj-m := deadman.o
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: default all modules user clean

default: modules user

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

signing_key.priv signing_key.x509:
	openssl req -x509 -new -nodes -utf8 -sha256 -days 36500 \
		-batch -config x509.genkey -outform DER \
		-out signing_key.x509 \
		-keyout signing_key.priv
	sudo mokutil --import signing_key.x509

sign: signing_key.priv signing_key.x509
	$(KDIR)/scripts/sign-file sha256 $^ deadman.ko
