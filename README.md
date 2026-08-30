# fastcompmgr

__fastcompmgr__ is a _fast_ compositor for X, a fork of an early version
of __Compton__ which is a fork of __xcompmgr-dana__ which is a fork
of __xcompmgr__.

*This repository is a fork. Its recent maintenance changes were made with assistance from OpenAI GPT-5.6 Terra.*

I used to use good old xcompmgr for long, because compton always
felt a bit laggy when moving/resizing windows or kinetic-scrolling
a webpage. Having tested the latest picom-10.2, it seems, things got even
worse (see benchmark below). However, xcompmgr does not draw shadows
on argb windows (e.g. some terminals) and
has several other glitches. That's why I traveled back into 2011, where
this feature was just added, cherry picked some later compton commits
to get rid of spurious segfaults and memleaks and made that version even
faster, based on profiling.
For example, window move- and resize events are coalesced and all repaint work
is paced to the display refresh rate. Occluded windows are not
painted and memory allocations/deallocations are largely avoided,
allowing for faster repaints of the screen.

## Benchmark
While on my Dell Latitude E5570 window moving, resizing and scrolling
*feels* clearly faster, there are also some numbers to support this
observation. Given a number of stacked chromium-windows, where no window
is fully occluded, I performed the respective operations *by hand*,
so please beware that the benchmark is not very sophisticated. The touchpad
driver `xserver-xorg-input-synaptics` was used which enables for kinetic
scrolling (Wayland anyone?). The following CPU usages were measured:

| Compositor    | move  | resize  | scroll |
| ------------- | ----- | ------- | ------ |
| fastcompmgr   | 6.7%  | 4.4%    | 1.5%   |
| xcompmgr      | 7.8%  | 4.9%    | 1.6%   |
| compton       | 26.4% | 6.8%    | 17.1%  |
| picom         | 29.3% | 8.1%    | 23.1%  |


Tools were used with the following flags:
~~~
(v0.1) fastcompmgr -o 0.4 -r 12 -c -C
(v1.1.8 from Debian 11) xcompmgr -o 0.4 -r 12 -c -C
(v1 from Debian 11) compton --config /dev/null --backend xrender -o 0.4 -r 12 -c -C
(v10.2) picom --config /dev/null --backend xrender -o 0.4 -r 12 -c

# Calc average using
$ fastcompmgr -o 0.4 -r 12 -c -C &  pid=$!; sleep 4; \
    top -b -n 20 -d 0.5 -p $pid | LC_ALL=C awk -v pid=$pid \
    '$1==PID {++PIDCOUNT} $1==pid && PIDCOUNT>1 {print $9}' |  \
    datamash mean 1; kill $pid
~~~



## Installation
If you're lazy, just grab the binary from the [release page](https://github.com/ztancrell/fastcompmgr/releases).

Otherwise, install the development versions of the following libraries:
### Dependencies:

* libx11
* libxcomposite
* libxdamage
* libxfixes
* libxrender
* libxrandr
* pkg-config
* make
* a C compiler

To build:

~~~ bash
$ make
$ make install
~~~

## Usage

~~~ bash
$ fastcompmgr -o 0.4 -r 12 -c -C
~~~
All options:
~~~
   -d display
    Which display should be managed.
   -r radius
    The blur radius for shadows. (default 12)
   -o opacity
    The translucency for shadows. (default .75)
   -l left-offset
    The left offset for shadows. (default -15)
   -t top-offset
    The top offset for shadows. (default -15)
   -m opacity
    The opacity for menu, dropdown-menu, popup-menu, and combo windows.
    (default 1.0)
   -c
    Enable client-side shadows on windows. Shadows are not drawn on menus,
    popup menus, dropdown menus, combo-box popups, or tooltips, because those
    window types use an oversized rect with no shape info and would render as
    a transparent box around the popup.
   -C
    Avoid drawing shadows on dock/panel windows.
   -f
    Fade windows in and out when opening, closing, hiding, or restoring.
   -F
    Also fade opacity changes.
   -I fade-in-step
    Opacity change per fade step. (default 0.028)
   -O fade-out-step
    Opacity change per fade step. (default 0.03)
   -D fade-delta-time
    Time between fade steps in milliseconds. (default 10)
   -i opacity
    Opacity of inactive windows. (0.0 - 1.0)
   -e opacity
    Opacity of window titlebars and borders. (0.0 - 1.0)
   -S
    Enable synchronous operation for debugging. Normal operation is asynchronous.
   -h, --help
    Show usage information.
    --shadow-red value
    Red color value of shadow (0.0 - 1.0, defaults to 0).
    --shadow-green value
    Green color value of shadow (0.0 - 1.0, defaults to 0).
    --shadow-blue value
    Blue color value of shadow (0.0 - 1.0, defaults to 0).
    --refresh-rate N
    Override display refresh rate in Hz (0 = autodetect via XRandR, default).

~~~


## License

xcompmgr has gotten around. As far as I can tell, the lineage for this
particular tree is something like:

* Keith Packard (original author)
* Matthew Hawn
* ...
* Dana Jansens
* Christopher Jeffrey
* Tycho Kirchner

Not counting the tens of people who forked it in between.

See LICENSE for more info.
