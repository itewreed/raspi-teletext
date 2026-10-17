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

If the raspi-teletext gets interrupted at startup check if a desktop manager is running. It has to be stopped before the program can run, i.e.

```sudo systemctl stop lightdm```

At startup the programs measure the field lengths to find out which
field is which, and print the result. If teletext pages come out
garbled, try again with `RASPI_TELETEXT_SWAP_FIELDS=1` set in the
environment, and please report which one works.

The data is moved 20 pixels left of where dispmanx put it, which puts
the clock run-in where real broadcasts have it on a Pi 4. Set
`RASPI_TELETEXT_XSHIFT` to a number of pixels (74 ns each) to adjust
it.

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

Channel VideoDat
----------------

`videodat` sends data the way Channel VideoDat did, a service Pro7 ran
in the VBI from 1990 to 1994. Together with the decoder in
https://github.com/itewreed/channel-videodat-decoder this allows the
whole chain from files to a VBI capture and back to be tested.

    videodat [-m first field lines] [-o second field lines] [-l level] \
             [-x start] [-s seed] [-]

With no arguments, a small text file and the clock are sent over and
over. Running "videodat -" sends the byte stream read from stdin, which
`vdenc` makes from files:

    ./vdenc -l -i MYPACKET FILE1.EXE README.TXT | sudo ./videodat -

The line masks select the lines carrying data: bit n is line 7+n on the
first field and 320+n on the second (note that this is the other way
round from teletext). The default, 0x0070, is lines 11-13 and 324-326,
which is where Pro7 sent it. `-x` moves the first bit, in pixels of
74 ns, and `-s` seeds the scrambling. The other options are as for
teletext.

    vdenc [-a stream] [-u] [-r count] [-c seconds] [-b rate] [-i text] \
          [-t time] [-s seed] [-l] file...

writes the files as packets to stdout: a header packet, one packet per
255 bytes and an end packet per file, with clock packets in between.
`-r` sets how often each group of 4 packets is sent (2, as broadcast),
`-l` loops forever. vdenc doesn't need a Raspberry Pi, and its output
can be read by `videodat_decoder.py --bytes`.

    vdvbi [-m mask] [-o mask] [-x us] [-b black] [-w white] [-g ns] \
          [-n noise] [-s seed] < stream > capture.vbi

turns the byte stream into what a bttv card captures from /dev/vbi0
(2048 samples per line, 32 lines per frame), with a band limit and
optional noise, so error correction can be tried out without hardware:

    ./vdenc FILE.ZIP | ./vdvbi -n 15 > test.vbi
    python3 videodat_decoder.py test.vbi

The signal: 10 bytes per line, NRZ at 2 Mbit/s starting about 11.7 us
after the line sync. Each byte is an inverted UART frame: a white start
bit, 8 data bits LSB first with 1 = black, and a black stop bit. The
packets (sync 56 AE, header, scrambled payload, 16 bit sum) are
described in vdpacket.c.

Some details could not be checked against a recording and are guesses:
the group counter in the header counts groups of 4 packets, the send
time in file headers is a DOS date and time, and the 6 bytes before the
name in an info block are zero.
