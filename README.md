Teletext for Raspberry Pi
-------------------------

This software generates a teletext signal in software. No hardware
mods are needed.

This support both PAL (Teletext) and NTSC (CEA608 format captions).

(The demo mode should no longer crash TVs!)

Usage:

Have a Raspberry Pi connected to a TV by composite video.
It doesn't matter if you are running X or not. Dispmanx will draw over
anything else.

At least 32MB of GPU memory is required.

Raspbian defaults to NTSC composite output. To permanently change it
to PAL, put `sdtv=2` in `config.txt` and reboot.

Build the programs:

    make

Twiddle the registers (not needed on KMS systems, see below):

    sudo ./tvctl on

Ensure you see the message "Teletext output is now on."

If you're connected via PAL, run the demo:

    ./teletext

and press the text button on your TV remote.

If you're connected using NTSC, run the demo:

    ./cea608

and enable the TV's closed caption controls to show CC1.

Raspberry Pi 4 and other KMS systems
------------------------------------

Current Raspberry Pi OS and Debian images use the vc4-kms-v3d kernel
driver. Dispmanx is not available there, so the old renderer cannot
run. When `/opt/vc/include/bcm_host.h` is
missing, `make` builds the programs against DRM/KMS instead (force this
with `make RENDER=kms`, or the old renderer with `make RENDER=dispmanx`).
This needs the libdrm headers:

    sudo apt install libdrm-dev pkg-config

Enable composite output in `config.txt` (`/boot/firmware/config.txt`
on Bookworm) and reboot. On the Pi 4 this turns HDMI off:

    dtoverlay=vc4-kms-v3d,composite
    enable_tvout=1

There is no need to run tvctl. The programs set a video mode with extra
lines at the top of each field, which programs the same timing tvctl
does, and select PAL (teletext) or NTSC (cea608) themselves. They must
be the only program using the display, so run them as root from a text
console, not from inside a desktop session. The console stays visible
below the VBI lines, and the normal mode comes back when the program
exits.

At startup the programs measure the field lengths to find out which
field is which, and print the result. If teletext pages come out
garbled, try again with `RASPI_TELETEXT_SWAP_FIELDS=1` set in the
environment, and please report which one works.

Detailed Usage
--------------

    tvctl on|off

This tool prepares the composite out for teletext transmission by
shifting the output picture into the VBI area. It will check the
registers are in a known state before doing anything. "on" and
"off" commands will have no effect if the state is already on or
off, or if the registers are in an unknown state.

    teletext [-m even field line mask] [-o odd field line mask] \
             [-l white level] [-f] [-]

Optional line mask arguments are a 16 bit mask to create quiet lines
in vbi output, first line is LSB, last is MSB. For example running
"teletext -m 0xFFF0 -o 0x0FFF" will output teletext packets on the
first four lines of even fields and last four lines of odd fields.
If only one mask is provided the same value will be used for both
fields.

The white level is specified as a number between 0 and 100. This
is the "brightness" of the high bits of the teletext signal.
Default is 100.

The '-f' argument activates full-field mode, where every available
ine on the display is used for teletext, allowing much greater
bandwidth. Most decoders do not support this mode.

Running with no arguments will show a demo. Running "teletext -"
will read packets from stdin and display them. You can therefore
pipe packets from another tool, possibly over the network with ssh
or netcat.

The packet format is 42 byte raw binary packets, without the clock
run-in. Each packet received will be transmitted once, so you must
send packets endlessly. See

http://www.etsi.org/deliver/etsi_i_ets/300700_300799/300706/01_60/ets_300706e01p.pdf

for details of the teletext protocol.

    cea608 <->

Running with no arguments will show a demo.  Running "cea608 -"
will read data from stdin and display.  The data is in binary format,
with each field represented as two bytes of parity-encoded data.
This is the "raw" format some capture tools can output.  Data is
output at 59.97 fields per second.  See

https://en.wikipedia.org/wiki/EIA-608

for details on the protocol and links to the specifications.
