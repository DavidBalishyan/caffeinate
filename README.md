# caffeinate

Keeps your Linux box awake. It's a clone of the macOS `caffeinate` command:
run it and the system won't idle-sleep (or the display won't blank, if you ask
for that) until it's done.

I wrote it because the usual advice for "stop my laptop sleeping" is
`systemd-inhibit`, and that only exists if you run systemd. This doesn't care
what your init system is.

## How it avoids caring about your init system

It talks to D-Bus directly through `libdbus-1`, which has nothing to do with
init. The lock itself goes through the `login1` D-Bus interface, and the trick
is that two different projects implement that same interface:

- systemd-logind, if you're on systemd
- elogind, if you're not (Void, Devuan, Artix, Gentoo with OpenRC, and so on)

Same binary, both worlds. If neither is around, it falls back to the session-bus
interfaces desktops expose (`org.freedesktop.ScreenSaver` and
`org.freedesktop.PowerManagement.Inhibit`), which is what GNOME and KDE use. At
no point does it link against systemd or shell out to it.

## Building

You need a C compiler and the libdbus-1 headers:

```sh
sudo apt install libdbus-1-dev     # Debian/Ubuntu
sudo dnf install dbus-devel        # Fedora
sudo pacman -S dbus                # Arch
sudo xbps-install dbus-devel       # Void

make
sudo make install                  # /usr/local by default
```

## Using it

```
caffeinate [-dismu] [-t seconds] [-w pid] [-v] [command [args...]]
```

The flags mirror macOS:

- `-d` keep the display awake
- `-i` prevent idle sleep (this is the default if you pass nothing)
- `-m` prevent disk idle sleep - Linux has no separate notion of this, so it's
  accepted for compatibility and treated like `-i`
- `-s` prevent the machine from sleeping at all (suspend/hibernate)
- `-u` mark the user as active - wakes the display for the timeout (5s if you
  don't give `-t`)
- `-t seconds` hold for that long, then quit
- `-w pid` hold until that process exits
- `command ...` run the command and hold until it finishes, then exit with its
  status
- `-v` say what's happening (which backends actually engaged)
- `-h` / `--help` / `--version` the obvious

If you don't give `-t`, `-w`, or a command, it just runs until you Ctrl-C it.
When you give more than one stopping condition, the first one to happen wins.

## Examples

```sh
caffeinate                       # stay awake until I Ctrl-C
caffeinate -d mpv movie.mkv      # don't blank the screen while the film plays
caffeinate -s -t 3600            # no suspend for the next hour
caffeinate -w $(pgrep -n rsync)  # hold on until that rsync is done
caffeinate -v make -j8           # stay awake through a long build
```

## Checking it's really working

While it's running, ask logind what's holding a lock:

```sh
caffeinate -s -t 30 &
systemd-inhibit --list
```

You'll see a `caffeinate` row until it exits. The lock is tied to an open file
descriptor, so even if the process gets killed the kernel closes the fd and the
lock goes away on its own - it can't get stuck on.

## Exit status

If you gave it a command to run, it exits with that command's status (or
`128 + signal` if the command was killed, or `127` if it couldn't be started at
all). Otherwise it's `0` for a normal finish and `2` if you got the arguments
wrong.

## License

[MIT](LICENSE)
