# wterm - xterm for wayland


wterm is an xterm replacement based on an st fork using wld.

st is a simple terminal emulator for X originally made by suckless.

(currently broken come back later)

![logo](contrib/logo/wterm.png "ebin logo")

## Requirements

* pkg-config
* fontconfig
* wayland
* xkbcommon
* pixman
* libdrm

On ubuntu it's

    sudo apt install libdrm-dev libfontconfig1-dev libwayland-dev libxkbcommon-dev libpixman-1-dev pkg-config

## build

    make

## install

    sudo make install

## Credits

Based on Aurélien APTEL <aurelien dot aptel at gmail dot com> st source code.
