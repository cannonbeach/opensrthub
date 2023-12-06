FROM ubuntu:20.04
LABEL maintainer="cannonbeachgoonie@gmail.com"
RUN apt-get update
RUN apt-get -y upgrade
RUN apt-get install -y apt-utils
RUN apt-get install -y libssl-dev
RUN apt-get install -y net-tools
RUN apt-get install -y iputils-ping
RUN apt-get install -y libnuma1
RUN apt-get install -y tcpdump
RUN apt-get install -y libcrypt-dev
ADD srthub /usr/bin/srthub
