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

if [ ! -f "/opt/srthub/users.json" ]; then
    cat <<EOF > /opt/srthub/users.json
    [
        {
           "username":"admin",
           "password":"password"
        }
    ]
    EOF
    echo "srthub: setting up users.json to username: admin and password: password, please edit this file to change login information"
else
    echo "srthub: users.json already exists, not overwriting it"
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

if [ ! -d "/var/app/cert" ]; then
    echo "srthub: creating directory /var/app/cert"
    sudo mkdir -p /var/app/cert
else
    echo "srthub: directory /var/app/cert already exists"
fi

#if [ -f "./webapp/package.json" ]; then
#    echo "srthub: installing package.json to /var/app"
#    sudo cp ./webapp/package.json /var/app
#else
#    echo "srthub: unable to find package.json"
#    exit
#fi

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
sudo apt-get install net-tools -y
echo "srthub: installing ntp"
sudo apt-get install ntp -y
echo "srthub: installing jq"
sudo apt-get install jq -y
echo "srthub: installing cpufrequtils"
sudo apt-get install cpufrequtils -y
echo "srthub: installing ethtool"
sudo apt-get install ethtool -y
echo "srthub: installing nasm"
sudo apt-get install nasm -y
echo "srthub: installing libz"
sudo apt-get install libz-dev -y
echo "srthub: installing liblzma-dev"
sudo apt-get install liblzma-dev -y
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
echo "srthub: nodejs, installing express-session (global)"
sudo npm install -g express-session
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
echo "srthub: nodejs, install moment (global)"
sudo npm install -g moment
echo "srthub: nodejs, install js-yaml (global)"
sudo npm install -g js-yaml
echo "srthub: nodejs, install url-exists (global)"
sudo npm install -g url-exists
echo "srthub: nodejs, install shelljs (global)"
sudo npm install -g shelljs
echo "srthub: installing express-validator"
sudo npm install -g express-validator
echo "srthub: installing helmet sanitizer"
sudo npm install -g helmet
echo "srthub: installing sanitize sanitizer"
sudo npm install -g sanitize
echo "srthub: installing sanitize sanitizer"
sudo npm install -g validator
echo "srthub: nodejs, creating node_modules symbolic link to current directory"
sudo ln -s /usr/lib/node_modules ./node_modules
popd

echo "srthub: installing Docker"
sudo apt-get install docker.io -y
echo "srthub: installing docker buildx"
sudo apt-get install docker.buildx -y
echo "srthub: installing tcpdump"
sudo apt-get install tcpdump -y
echo "srthub: installing ifstat"
sudo apt-get install ifstat -y
echo "srthub: installing zip"
sudo apt-get install zip -y
echo "srthub: installing unzip"
sudo apt-get install unzip -y
echo "srthub: updating pm2 (if actually needed)"
sudo npm install pm2@latest -g
sudo pm2 update
echo ""
echo "All Done With Standard System Setup"
echo ""
echo "Generating public/private key pair for system"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -pkeyopt rsa_keygen_pubexp:65537 | openssl pkcs8 -topk8 -nocrypt -outform pem > /opt/srthub/private_opensrthub.key
openssl pkey -pubout -inform pem -in /opt/srthub/private_opensrthub.key -out /opt/srthub/public_opensrthub.key
open "Generating HTTPS key and certificate for system"
openssl req -x509 -nodes -days 365 -newkey rsa:2048 -keyout /var/app/cert/server.key -out /var/opt/cert/server.crt
echo "Done!"
