#!/bin/bash
mkdir build-aarch64
meson setup --cross-file aarch64.ini build-aarch64
cd build-aarch64
meson compile

