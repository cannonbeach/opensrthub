#!/bin/bash

echo "Clone libsrt from public Github repository"
git clone https://github.com/Haivision/srt.git ./cbsrt
pushd cbsrt
echo "Configuring libsrt"
git checkout tags/v1.5.4 -b v1.5.4
./configure --prefix=/usr
make -j8
if [ -f "libsrt.a" ]; then
    echo "srthub: libsrt.a built properly"
else
    echo "srthub: libsrt.a did not build properly - please check manually, aborting installation!"
    exit
fi
popd

echo "Clone libcurl"
git clone https://github.com/cannonbeach/curl.git ./cblibcurl
pushd cblibcurl
./buildconf
if [ -f "configure" ]; then
    echo "srthub: curl configure script generated"
else
    echo "srthub: curl configure script not generated - please check manually, aborting installation!"
    exit
fi
echo "Configuring curl for compilation"
./configure --prefix=/usr --enable-static --enable-pthreads --without-ssl --without-librtmp --without-libidn2 --without-nghttp2 --without-brotli
make -j8
if [ -f "./lib/.libs/libcurl.a" ]; then
    echo "srthub: libcurl.a was compiled correctly"
else
    echo "srthub: libcurl.a was not compiled correctly - please check manually, aborting installation!"
    exit
fi
popd

echo "Clone of FFmpeg libraries (used for decoding audio/video preview)"
git clone https://git.ffmpeg.org/ffmpeg.git ./cbffmpeg
pushd cbffmpeg
git checkout -b release6.1 remotes/origin/release/6.1
./configure --prefix=/usr --disable-encoders --disable-iconv --disable-v4l2-m2m --disable-muxers --disable-vaapi --disable-vdpau --disable-videotoolbox --disable-muxers --disable-avdevice --enable-encoder=mjpeg
make -j8
if [ -f "./libavcodec/libavcodec.a" ]; then
    echo "srthub: libavcodec.a was compiled correctly"
else
    echo "srthub: libavcodec.a was not compiled correctly - please check manually, aborting installation!"
    exit
fi
popd
