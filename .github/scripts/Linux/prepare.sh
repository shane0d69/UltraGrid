#!/bin/bash

set -ex

echo "::set-env name=AJA_DIRECTORY::/var/tmp/ntv2sdk"
echo "::set-env name=CPATH::/usr/local/qt/include"
echo "::set-env name=LIBRARY_PATH::/usr/local/qt/lib"
echo "::set-env name=PKG_CONFIG_PATH::/usr/local/qt/lib/pkgconfig"
echo "::add-path::/usr/local/qt/bin"

sudo apt update
sudo apt install libcppunit-dev nvidia-cuda-toolkit
sudo apt install libglew-dev freeglut3-dev libgl1-mesa-dev
sudo apt install libx11-dev
sudo apt install libsdl2-dev
sudo apt install libssl-dev
sudo apt install portaudio19-dev libjack-jackd2-dev libasound-dev libv4l-dev
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
sudo apt install libopencv-dev
sudo apt install libglib2.0-dev libcurl4-nss-dev
( mkdir gpujpeg/build && cd gpujpeg/build && CC=gcc-6 CXX=g++-6 cmake .. && make && sudo make install && sudo ldconfig )
( sudo apt install uuid-dev && cd cineform-sdk/ && cmake . && make CFHDCodecStatic )
sudo chmod 777 /usr/local

# Install XIMEA
wget --no-verbose https://www.ximea.com/downloads/recent/XIMEA_Linux_SP.tgz
tar xzf XIMEA_Linux_SP.tgz
cd package
sudo ./install

# Install AJA
if [ -n "$sdk_pass" ]; then
        curl --netrc-file <(cat <<<"machine frakira.fi.muni.cz login sdk password $sdk_pass") https://frakira.fi.muni.cz/~xpulec/sdks/ntv2sdklinux.zip -O
        unzip ntv2sdklinux.zip -d /var/tmp
        mv /var/tmp/ntv2sdk* /var/tmp/ntv2sdk
        cd /var/tmp/ntv2sdk/ajalibraries/ajantv2
        make
fi

