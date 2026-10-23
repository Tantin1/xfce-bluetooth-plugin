# xfce-bluetooth-plugin

*[Versión en español](README.md)*

A lightweight Bluetooth plugin for the Xfce panel. It covers everyday use, on
par with what the Cinnamon or GNOME desktops ship, without opening a separate
program.

## What it does

- Connect, disconnect, pair, forget, block and mark devices as trusted (so
  they connect on their own).
- **Reconnect** in one click, for the headset that is connected but silent.
- **Connect a single profile** of a device: audio, hands-free, keyboard or
  mouse.
- **Rename** a paired device. Leaving the name empty restores the factory
  name.
- Ask for and confirm pairing codes.
- Make the computer visible to other devices. Visibility **turns itself off
  after 3 minutes**.
- Show the device's battery level, when the device reports it.
- Optional desktop notifications on connect and disconnect.
- **Send files** to a phone or another computer, with a progress bar.
- **Receive files**, with a prompt to accept or reject, a progress bar and a
  "Show in folder" button.
- Handle several adapters, each with its own switch.

The device list stays still: connected devices first, then paired ones in
alphabetical order, and nearby unpaired ones last, grouped by distance.

## What it does not do, on purpose

- **Bluetooth internet sharing from a phone.** If you use NetworkManager, its
  own panel icon already does that well.
- **Choosing between high-quality audio and hands-free.** The sound server
  (PipeWire or PulseAudio) decides that, from its volume control.
- **Blueman's advanced features**, such as the local services window or the
  step-by-step wizard. If you need them, Blueman is still the right tool.

## Requirements

- Xfce 4.18 or later, on X11. Also on Wayland, with panel 4.20 or later
  (tested on labwc).
- Tested on CachyOS. Also intended for Debian 12 and its derivatives
  (Loc OS, CuerdOS).
- Minimum versions: GTK 3.24, GLib 2.56, Xfce panel 4.18 and, for
  notifications, libnotify 0.7.

## Install

### 1. Build dependencies

On Debian and derivatives:

```bash
sudo apt install build-essential meson ninja-build pkg-config gettext \
                 libgtk-3-dev libxfce4panel-2.0-dev libglib2.0-dev libnotify-dev
```

On Arch and derivatives:

```bash
sudo pacman -S --needed base-devel meson ninja gettext gtk3 xfce4-panel libnotify
```

`libnotify` is optional: without it the plugin still builds, just without
notifications.

### 2. Build and install

```bash
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
xfce4-panel -r
```

On Arch and derivatives, if installing complains, use
`sudo meson install -C build --no-rebuild`.

Then: right-click the panel → Panel → Add New Items, and pick the Bluetooth
plugin.

If the plugin does not show up in that list, the compiled file ended up in a
folder your panel does not look in. Point it to the right one when
configuring (the example is Debian's):

```bash
meson setup build --prefix=/usr -Dplugin_dir=/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins
```

Building with CMake is also possible; see [DESARROLLO.md](DESARROLLO.md)
(developer notes, in Spanish).

### 3. Runtime dependencies

- **`bluez`**, with its service running. Required. To check:

  ```bash
  systemctl status bluetooth
  ```

- **The file transfer service**, only if you want to send or receive files.
  It comes in a separate package: `bluez-obex` on Arch, `bluez-obexd` on
  Debian. On some distributions it also has to be enabled:

  ```bash
  systemctl --user enable --now obex.service
  ```

  No guessing needed: Properties → "File transfer" tells you what is missing
  and gives you the exact command.

## Uninstall

From the same folder where you built it:

```bash
sudo ninja -C build uninstall
xfce4-panel -r
```

## Receiving files

It is **off** by default, because turning it on announces that your computer
accepts files. Enable it in Properties → "File transfer" → "Receive files
from other devices", where you also pick the folder (Downloads by default).

**If the phone says "transfer forbidden"**, the file transfer service is
almost always to blame: some versions refuse to write when the folder goes
through a symbolic link, and others reject everything because of a bug of
their own. That same window shows a **"Fix automatically"** button that
solves it without asking for a password.

**Only one program can receive files at a time.** If Blueman is running, the
plugin tells you so and retries on its own every minute: just close Blueman.

To investigate a failure, the useful log is the transfer service's:

```bash
journalctl --user -u obex --since "5 minutes ago" --no-pager
```

## Translations

Available in English and Spanish. To add a language, see
[DESARROLLO.md](DESARROLLO.md).

## License

GPL-2.0-or-later.
