CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  := -lcjson -lsqlite3 -lcrypto -lcurl -lpthread -lm
PREFIX  ?= /usr/local

.PHONY: all debug test test-asan install clean

all: mpesa_webhook

mpesa_webhook: src/mpesa_webhook.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Build with AddressSanitizer + UndefinedBehaviorSanitizer to catch memory bugs.
debug: src/mpesa_webhook.c
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o mpesa_webhook_debug $< $(LDLIBS)

# End-to-end tests against a mock Daraja server (needs python3, no internet).
test: mpesa_webhook
	python3 test/test_e2e.py

# Same tests, run against the sanitizer build.
test-asan: debug
	MPESA_BIN=./mpesa_webhook_debug python3 test/test_e2e.py

install: mpesa_webhook
	install -D -m 755 mpesa_webhook $(DESTDIR)$(PREFIX)/bin/mpesa_webhook

clean:
	rm -f mpesa_webhook mpesa_webhook_debug *.db *.db-wal *.db-shm
