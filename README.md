# Deadman

Deadman is a Linux kernel module, which enables registering an arbitrary 'precious' process by PID for 'livelihood' monitoring. That is, if the process dies, killed or stopped by the user, deadman will reboot the machine. It's original purpose was/is to launch [`xwrits`](https://www.lcdf.org/xwrits/) or a similar break reminder and make sure you cannot 'cheat' by killing the reminder process in order to skip breaks. If you do, `deadman` will reboot your linux machine and you will lose more time rebooting it and setting your work environment back up than you would have by just taking the break in the first place..

Linux kernel headers version 5.6 or higher is required to compile this module (builds cleanly against 6.12; the `task_struct` `state`/`__state` rename in 5.14 is handled).

## TL;DR
Assuming you are using a Debian based Linux distribution, the following will install `xwrits` and the module build tools. Then it will run the [`xwrits-deadman.sh`](./xwrits-deadman.sh) wrapper, which will install the module, launch `xwrits` (with `libxwrits-sendevent.so` preloaded) and the `wayland-xwrits-bridge` daemon so keyboard/mouse activity is tracked under Wayland as well as X11, and register both PIDs with `/proc/deadman`:

```bash
sudo apt-get install -y xwrits build-essential libelf-dev linux-headers-`uname -r`
make            # builds & signs deadman.ko, and builds wayland-xwrits-bridge and libxwrits-sendevent.so
export TYPETIME=40
export BREAKTIME=4
./xwrits-deadman.sh
```

> **Secure Boot note:** `make` automatically signs `deadman.ko`. If your system already has an enrolled Ubuntu/Debian MOK key at `/var/lib/shim-signed/mok/MOK.der`, `make sign` will use it automatically (no reboot needed). Otherwise it generates `signing_key.priv` / `signing_key.x509`; with Secure Boot enabled, you must enroll the key once via `make enroll-key`, **reboot**, and select **Enroll MOK** in the blue UEFI MokManager prompt before the kernel will accept `insmod ./deadman.ko`.

> **Wayland note:** On GNOME (Mutter) and KDE (ScreenSaver), `wayland-xwrits-bridge` queries idle time via the user's Session DBus with no extra permissions needed. On other Wayland compositors (e.g. Sway, Hyprland), add your user to the `input` group (`sudo usermod -aG input "$USER"`) so `wayland-xwrits-bridge` can monitor `/dev/input/event*` directly.

## Process Registering for monitoring:

Registering processes is done via the `/proc/deadman` file (up to 32 PIDs can be registered across multiple writes or separated by spaces/commas in a single write; if any registered process dies or is stopped, deadman triggers):

```bash
echo 514 | sudo tee /proc/deadman
echo 515 | sudo tee /proc/deadman
```

## Checking deadman status:

This can be done via the `/proc/deadman` info file:

```bash
cat /proc/deadman
```

## Module Parameters:

- **loopsleep**: how many seconds to wait between each pid status check (default 30)
- **nowayout**: once a process is registered, the module can not be unloaded - i.e. deadman can not be stop (default true) 
- **noboot**: don't boot even if pid is lost - use this for testing (default false)
- **reboot_delay**: Deadman will delay reboot by that many seconds - enough time for example to exit a desktop session (default 180)

