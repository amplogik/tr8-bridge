PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
UDEVDIR    ?= /etc/udev/rules.d
SYSTEMDDIR ?= /etc/systemd/system

CFLAGS  ?= -O2 -Wall -Wextra -pedantic
CFLAGS  += -std=c11
LDLIBS   = -lasound -lpthread

BIN = tr8-bridge

all: $(BIN)

$(BIN): src/tr8-bridge.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: $(BIN)
	install -Dm755 $(BIN)                          $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 udev/71-tr8-bridge.rules        $(DESTDIR)$(UDEVDIR)/71-tr8-bridge.rules
	install -Dm644 systemd/tr8-bridge@.service     $(DESTDIR)$(SYSTEMDDIR)/tr8-bridge@.service
	@echo
	@echo "Now run:"
	@echo "  sudo udevadm control --reload"
	@echo "  sudo systemctl daemon-reload"
	@echo "Then re-plug the TR-8."

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(UDEVDIR)/71-tr8-bridge.rules
	rm -f $(DESTDIR)$(SYSTEMDDIR)/tr8-bridge@.service

clean:
	rm -f $(BIN)

.PHONY: all install uninstall clean
