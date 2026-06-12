CC              := cc
CSTD            := -std=c11
WARN            := -Wall -Wextra -Wpedantic
OPT             := -O2

PKG_CFLAGS      := $(shell pkg-config --cflags x11 xi)
PKG_LIBS        := $(shell pkg-config --libs x11 xi)

CFLAGS          := $(CSTD) $(WARN) $(OPT) $(PKG_CFLAGS)
LDFLAGS         :=
LDLIBS          := $(PKG_LIBS)

TARGET          := x11-block-rightclick
SRC             := x11_block_rightclick_target_window.c
OBJ             := $(SRC:.c=.o)

PREFIX          := $(HOME)/.local
BINDIR          := $(PREFIX)/bin
CONFIGDIR       := $(HOME)/.config
UNITDIR         := $(CONFIGDIR)/systemd/user

CONFIG_FILE     := $(CONFIGDIR)/x11-block-rightclick.conf
SERVICE_FILE    := $(UNITDIR)/x11-block-rightclick.service

.PHONY: all clean debug install install-bin install-config install-service \
        enable-service disable-service uninstall status logs restart

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

debug: CFLAGS := $(CSTD) $(WARN) -O0 -g3 $(PKG_CFLAGS)
debug: clean all

install: install-bin install-config install-service enable-service

install-bin: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)

install-config:
	install -d $(CONFIGDIR)
	@if [ ! -f "$(CONFIG_FILE)" ]; then \
		printf '%s\n' \
			'x11BlockRightclick.denyTitle: LXQt Panel' \
			'x11BlockRightclick.denyTitle.1: pcmanfm-desktop0' \
			'x11BlockRightclick.denyTitle.2: pcmanfm-desktop1' \
			'x11BlockRightclick.denyTitle.3: pcmanfm-desktop2' \
			'x11BlockRightclick.denyTitle.4: pcmanfm-desktop3' \
			> "$(CONFIG_FILE)"; \
		echo "Created $(CONFIG_FILE)"; \
	else \
		echo "Keeping existing $(CONFIG_FILE)"; \
	fi

install-service:
	install -d $(UNITDIR)
	@printf '%s\n' \
		'[Unit]' \
		'Description=Block right-click on selected X11 windows' \
		'After=graphical-session.target' \
		'PartOf=graphical-session.target' \
		'' \
		'[Service]' \
		'Type=simple' \
		'ExecStart=%h/.local/bin/$(TARGET)' \
		'Environment=X11_BLOCK_RIGHTCLICK_CONFIG=%h/.config/x11-block-rightclick.conf' \
		'Restart=on-failure' \
		'RestartSec=2' \
		'' \
		'[Install]' \
		'WantedBy=graphical-session.target' \
		> "$(SERVICE_FILE)"
	@echo "Created $(SERVICE_FILE)"
	systemctl --user daemon-reload

enable-service:
	systemctl --user enable --now $(TARGET).service

disable-service:
	systemctl --user disable --now $(TARGET).service || true

restart:
	systemctl --user daemon-reload
	systemctl --user restart $(TARGET).service

status:
	systemctl --user status $(TARGET).service

logs:
	journalctl --user -u $(TARGET).service -f

uninstall: disable-service
	rm -f "$(SERVICE_FILE)"
	systemctl --user daemon-reload
	rm -f "$(BINDIR)/$(TARGET)"
	@echo "Binary and service removed. Config kept at $(CONFIG_FILE)"

clean:
	rm -f $(OBJ) $(TARGET)