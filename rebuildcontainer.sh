#!/bin/bash

if [ -f ./cbsrt/libsrt.a ]; then
    echo "cbsrt already exists"
else
    git clone https://github.com/Haivision/srt.git ./cbsrt
    pushd cbsrt
    echo "Configuring libsrt"
    git checkout tags/v1.4.4 -b v1.4.4
    ./configure --prefix=/usr
    make -j8
    if [ -f "libsrt.a" ]; then
        echo "srthub: libsrt.a built properly"
    else
        echo "srthub: libsrt.a did not build properly - please check manually, aborting installation!"
        exit
    fi
    popd
fi

if [ -f ./cblibcurl/lib/.libs/libcurl.a ]; then
    echo "cblibcurl already exists"
else
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
    ./configure --prefix=/usr --enable-static --enable-pthreads --without-ssl --without-librtmp --without-libidn2 --without-nghttp2
    make -j8
    if [ -f "./lib/.libs/libcurl.a" ]; then
        echo "srthub: libcurl.a was compiled correctly"
    else
        echo "srthub: libcurl.a was not compiled correctly - please check manually, aborting installation!"
        exit
    fi
    popd
fi

if [ -f ./cbffmpeg/libavcodec/libavcodec.a ]; then
    echo "cbffmpeg already exists"
else
    echo "Clone of FFmpeg libraries (used for decoding audio/video preview)"
    git clone https://github.com/cannonbeach/FFmpeg.git ./cbffmpeg
    pushd cbffmpeg
    ./configure --prefix=/usr --disable-encoders --enable-avresample --disable-iconv --disable-v4l2-m2m --disable-muxers --disable-vaapi --disable-vdpau --disable-videotoolbox --disable-muxers --disable-avdevice --enable-encoder=mjpeg
    make -j8
    if [ -f "./libavcodec/libavcodec.a" ]; then
        echo "srthub: libavcodec.a was compiled correctly"
    else
        echo "srthub: libavcodec.a was not compiled correctly - please check manually, aborting installation!"
        exit
    fi
    popd
fi

echo "Building opensrthub"
make clean
make
if [ -f "srthub" ]; then
    echo "srthub: srthub compiled correctly"
else
    echo "srthub: srthub did not get built - please check manually, aborting installation!"
    exit
fi
pushd docker
sudo cp ../srthub .
echo "Building docker package for srthub"
sudo docker build -t dockersrthub .
popd

sudo cp ./webapp/server.js /var/app
sudo cp ./webapp/public/index.html /var/app/public
sudo cp ./webapp/public/client.js /var/app/public
echo "Docker Package Built and Installed"
echo "Please restart NodeJS, sudo pm2 restart opensrthub"
echo "Any existing containers will need to be restarted to use new version of code"
echo "Done!"
