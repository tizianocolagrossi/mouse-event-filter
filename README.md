# x11-block-rightclick

Block right-clicks on selected X11 windows by matching window title or `WM_CLASS`, with configuration loaded from a user config file and optional startup through a `systemd --user` service.

## What it does

This program uses Xlib and XInput2 to intercept `Button3` presses on X11, resolve the target client window under the pointer, read its title and class, and decide whether to block or replay the click based on a denylist.

Typical use case: prevent context menus on desktop panels, desktop background windows, or other specific X11 surfaces such as LXQt panel or `pcmanfm` desktop windows.

## Features

- Intercepts global right-clicks on X11 using XInput2.
- Matches windows by title and `WM_CLASS`.
- Loads denylist entries from a config file instead of hardcoding them.
- Includes detailed in-source documentation for maintenance.
- Supports running as a `systemd --user` service.
- Ships with a Makefile that can build, install, and enable the service.

## Requirements

You need:

- A Linux system running **X11**.
- Development headers for Xlib and XInput.
- `pkg-config` for compiler flags.
- `systemd` user services if you want automatic startup.

On Debian/Ubuntu-like systems:

```bash
sudo apt install build-essential pkg-config libx11-dev libxi-dev
```

## Build

```bash
make
```

This builds the executable:

```text
x11-block-rightclick
```

## Install

The provided Makefile can:

- build the binary,
- install it to `~/.local/bin`,
- create `~/.config/x11-block-rightclick.conf` if it does not exist,
- create a user unit in `~/.config/systemd/user/`,
- reload the user `systemd` manager,
- enable and start the service.

Run:

```bash
make install
```

## Configuration

The program reads its denylist from:

```text
~/.config/x11-block-rightclick.conf
```

You can also override the path with:

```bash
X11_BLOCK_RIGHTCLICK_CONFIG=/path/to/config ./x11-block-rightclick
```

Example config:

```ini
x11BlockRightclick.denyTitle: LXQt Panel
x11BlockRightclick.denyTitle.1: pcmanfm-desktop0
x11BlockRightclick.denyTitle.2: pcmanfm-desktop1
x11BlockRightclick.denyTitle.3: pcmanfm-desktop2
x11BlockRightclick.denyTitle.4: pcmanfm-desktop3

x11BlockRightclick.denyClass: pcmanfm
x11BlockRightclick.denyClass.1: lxqt-panel
```

### Matching behavior

- `denyTitle*` entries are checked as substring matches against the window title.
- `denyClass*` entries are checked as substring matches against the combined `res_name/res_class` string from `WM_CLASS`.
- Matching is case-sensitive unless you change the source.

## Running manually

Start it directly in the current X11 session:

```bash
./x11-block-rightclick
```

Or with an explicit config path:

```bash
X11_BLOCK_RIGHTCLICK_CONFIG="$HOME/.config/x11-block-rightclick.conf" ./x11-block-rightclick
```

## systemd user service

The Makefile installs a user service, not a system-wide root service.

Useful commands:

```bash
systemctl --user status x11-block-rightclick.service
systemctl --user restart x11-block-rightclick.service
systemctl --user stop x11-block-rightclick.service
journalctl --user -u x11-block-rightclick.service -f
```

If your desktop session does not export the X11 environment correctly to the user `systemd` instance, you may need to adjust `DISPLAY` and `XAUTHORITY` in the generated service file.

## Makefile targets

Common targets:

```bash
make                # build
make debug          # debug build
make install        # build + install binary/config/service + enable service
make restart        # restart the user service
make status         # service status
make logs           # follow service logs
make uninstall      # remove binary and service, keep config
make clean          # remove build artifacts
```

## How it works

At a high level, the program:

1. Opens the X display.
2. Checks for XInput2 availability.
3. Grabs mouse button 3 globally on the root window.
4. Receives right-click events.
5. Resolves the actual client window under the pointer.
6. Reads `_NET_WM_NAME` / `WM_NAME` and `WM_CLASS`.
7. Compares them against configured denylist entries.
8. Replays the click if allowed, or consumes it if denied.

## Notes and limitations

- This is for **X11 only** and does not work on Wayland.
- It depends on X server behavior, window manager behavior, and the target application's window hierarchy.
- Some applications may expose unexpected child windows or metadata, so matching may require experimentation.
- `WM_CLASS` and title matching are substring-based, which is convenient but not as strict as exact matching.

## Debugging tips

To inspect a window's X11 metadata, tools such as `xprop` can help.

For example:

```bash
xprop WM_CLASS WM_NAME _NET_WM_NAME
```

Then click the target window and use the observed values in your config file.

For service debugging:

```bash
journalctl --user -u x11-block-rightclick.service -n 100
```

