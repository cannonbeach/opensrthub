#!/bin/bash

if [ ! -d "/opt/srthub" ]; then
    echo "srthub: creating directory /opt/srthub"
    sudo mkdir -p /opt/srthub
else
    echo "srthub: directory /opt/srthub already exists"
fi

if [ ! -d "/opt/srthub/configs" ]; then
    echo "srthub: creating directory /opt/srthub/configs"
    sudo mkdir -p /opt/srthub/configs
else
    echo "srthub: directory /opt/srthub/configs already exists"
fi

if [ ! -d "/opt/srthub/status" ]; then
    echo "srthub: creating directory /opt/srthub/status"
    sudo mkdir -p /opt/srthub/status
else
    echo "srthub: directory /opt/srthub/status already exists"
fi

if [ ! -d "/opt/srthub/thumbnail" ]; then
    echo "srthub: creating directory /opt/srthub/thumbnail"
    sudo mkdir -p /opt/srthub/thumbnail
else
    echo "srthub: directory /opt/srthub/thumbnail already exists"
fi

if [ ! -d "/var/app" ]; then
    echo "srthub: creating directory /var/app"
    sudo mkdir -p /var/app
else
    echo "srthub: directory /var/app already exists"
fi

if [ ! -d "/var/app/public" ]; then
    echo "srthub: creating directory /var/app/public"
    sudo mkdir -p /var/app/public
else
    echo "srthub: directory /var/app/public already exists"
fi

if [ -f "./webapp/server.js" ]; then
    echo "srthub: installing server.js to /var/app"
    sudo cp ./webapp/server.js /var/app
else
    echo "srthub: unable to find server.js"
    exit
fi

if [ -f "./webapp/public/client.js" ]; then
    echo "srthub: installing client.js to /var/app/public"
    sudo cp ./webapp/public/client.js /var/app/public
else
    echo "srthub: unable to find client.js"
    exit
fi

if [ -f "./webapp/public/index.html" ]; then
    echo "srthub: installing index.html to /var/app/public"
    sudo cp ./webapp/public/index.html /var/app/public
else
    echo "srthub: unable to find index.html"
    exit
fi

if [ -f "./webapp/package.json" ]; then
    echo "srthub: installing package.json to /var/app"
    sudo cp ./webapp/package.json /var/app
else
    echo "srthub: unable to find package.json"
    exit
fi

echo "srthub: performing global Ubuntu update"
sudo apt-get update -y
echo "srtgub: performing global Ubuntu upgrade of packages"
sudo apt-get upgrade -y
echo "srthub: installing build-essential packages"
sudo apt-get install build-essential -y
echo "srthub: installing tclsh"
sudo apt-get install tclsh -y
echo "srthub: installing autoconf"
sudo apt-get install autoconf -y
echo "srthub: installing automake"
sudo apt-get install automake -y
echo "srthub: intallling libssl-dev packages"
sudo apt-get install libssl-dev -y
echo "srthub: installing curl packages"
sudo apt-get install curl -y
echo "srthub: installing cmake"
sudo apt-get install cmake -y
echo "srthub: installing libtool"
sudo apt-get install libtool -y
echo "srthub: installing net-tools"
sudo apt-get install net-tools
echo "srthub: installing nasm"
sudo apt-get install nasm -y
echo "srthub: installing libz"
sudo apt-get install libz-dev -y
echo "srthub: installing pkg-config"
sudo apt-get install pkg-config -y
echo "srthub: installing ca-certificates"
sudo apt-get install ca-certificates -y
echo "srthub: -installing gnupg"
sudo apt-get install gnupg -y
echo "srthub: making keyrings directory /etc/apt/keyrings for Nodesource GPG key"
sudo mkdir -p /etc/apt/keyrings
echo "srthub: importing Nodesource GPG key"
curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key | sudo gpg --dearmor -o /etc/apt/keyrings/nodesource.gpg
NODE_MAJOR=18
echo "deb [signed-by=/etc/apt/keyrings/nodesource.gpg] https://deb.nodesource.com/node_$NODE_MAJOR.x nodistro main" | sudo tee /etc/apt/sources.list.d/nodesource.list
echo "srthub: performing global Ubuntu update (again)"
sudo apt-get update -y
echo "srthub: installing nodejs"
sudo apt-get install -y nodejs
echo "srthub: installing pm2 package"
sudo npm install -g pm2
pushd /var/app
echo "srthub: nodejs, installing express (global)"
sudo npm install -g express
echo "srthub: nodejs, installing body-parser (global)"
sudo npm install -g body-parser
echo "srthub: nodejs, installing fs (global)"
sudo npm install -g fs
echo "srthub: nodejs, install archiver (global)"
sudo npm install -g archiver
echo "srthub: nodejs, install winston logger (global)"
sudo npm install -g winston
echo "srthub: nodejs, install read-last-lines file reader (global)"
sudo npm install -g read-last-lines
echo "srthub: nodejs, creating node_modules symbolic link to current directory"
sudo ln -s /usr/lib/node_modules ./node_modules
popd

echo "srthub: installing Docker"
sudo apt-get install docker.io -y
echo "srthub: installing tcpdump"
sudo apt-get install tcpdump -y
echo "srthub: installing ifstat"
sudo apt-get install ifstat -y
echo "srthub: installing zip"
sudo apt-get install zip -y
echo "srthub: installing unzip"
sudo apt-get install unzip -y
#echo "srthub: installing apache"
#sudo apt-get install apache2 -y
echo "srthub: updating pm2 (if actually needed)"
sudo npm install pm2@latest -g
sudo pm2 update
echo ""
echo "All Done With Standard System Setup"
echo ""
echo "Clone libsrt from public Github repository"
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

echo "Building opensrthub"
make
if [ -f "srthub" ]; then
    echo "srthub: srthub compiled correctly"
else
    echo "srthub: srthub did not get built - please check manually, aborting installation!"
    exit
fi
pushd docker
cp ../srthub .
echo "Building docker package for srthub"
sudo docker build -t dockersrthub .
popd
echo "Docker Package Built and Installed"
echo "Done!"
