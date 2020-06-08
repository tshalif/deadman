# Deadman

Deadman is a Linux kernel module, which enables registring an arbitrary 'precious' process by PID for 'livlihood' monitoring. That is, if the process dies, killed or stopped by the user, deadman will reboot the machine. It's original purpose was/is to launch [`xwrits`](https://www.lcdf.org/xwrits/) or a similar break reminder and make sure you cannot 'cheat' by killing the reminder process in order to skip breaks. If you do, `deadman` will reboot your linux machine and you will lose more time rebooting it and setting your work environment back up than you would have by just taking the break in the first place..

## TL;DR
Assuming you are using a Debian based Linux distribution, the fillowing will install `xwrits` and the module build tools. Then it will run the [`xwrits-deadman.sh`](./xwrits-deadman.sh) wrapper, which will install the module and launch `xwrits` with 40 minutes of allowed typing/mouse activity and 4 minutes required break in between activity sessions:

```bash
sudo apt-get install -y xwrits build-essential libelf-dev linux-headers-`uname -r`
make
export TYPETIME=40
export BREAKTIME=4
./xwrits-deadman.sh
```

## Process Registring for monitring:

Registring processes is done via the `/proc/deadman` file:

```bash
echo 514 | sudo tee /proc/deadman
```

## Checking deadman status:

This can be done via the `/proc/deadman` info file:

```bash
cat /proc/deadman
```

## Module Parameters:

- **margin**: how many seconds to wait between each pid status check (default 60)
- **nowayout**: once a process is registrered, the module can not be unloaded - i.e. deadman can not be stop (default true) 
- **noboot**: don't boot even if pid is lost - use this for testing (default false)
- **reboot_delay**: Deadman will delay reboot by that many seconds - enough time for example to exit a desktop session (default 180)

