CC=gcc
CXX=g++
CFLAGS=-g -c -O2 -m64 -Wall -Wfatal-errors -funroll-loops -Wno-deprecated-declarations -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-format-truncation
SRC=./source
INC=-I./include
OBJS=crc.o tsdecode.o mempool.o dataqueue.o cJSON.o cJSON_Utils.o
LIB=libsrthub.a
BASELIBS=

BASELIBS += ./cbffmpeg/libavfilter/libavfilter.a \
	    ./cbffmpeg/libavformat/libavformat.a \
	    ./cbffmpeg/libswscale/libswscale.a \
	    ./cbffmpeg/libavcodec/libavcodec.a \
	    ./cbffmpeg/libavutil/libavutil.a \
	    ./cbffmpeg/libswresample/libswresample.a \
	    ./cbsrt/libsrt.a \
	    /usr/lib/x86_64-linux-gnu/libcrypto.a

INC += -I./cbffmpeg
INC += -I./cbsrt/srtcore
INC += -I./cbsrt

BASELIBS += -lz -ldl -lssl

all: $(LIB) srthub

srthub: srthub.o $(OBJS)
	$(CXX) srthub.o $(OBJS) -L./ $(BASELIBS) -lm -lpthread -o srthub

$(LIB): $(OBJS)
	ar rcs $(LIB) $(OBJS)
	@echo finishing building lib

srthub.o: $(SRC)/srthub.c
	$(CC) $(CFLAGS) $(INC) $(SRC)/srthub.c

dataqueue.o: $(SRC)/dataqueue.c
	$(CC) $(CFLAGS) $(INC) $(SRC)/dataqueue.c

mempool.o: $(SRC)/mempool.c
	$(CC) $(CFLAGS) $(INC) $(SRC)/mempool.c

tsdecode.o: $(SRC)/tsdecode.c
	$(CC) $(CFLAGS) $(INC) $(SRC)/tsdecode.c

crc.o: $(SRC)/crc.c
	$(CC) $(CFLAGS) $(INC) $(SRC)/crc.c

cJSON.o: $(SRC)/cJSON.c
	$(CC) $(CFLAGS) $(INC) $(SRC)/cJSON.c

cJSON_Utils.o: $(SRC)/cJSON_Utils.c
	$(CC) $(CFLAGS) $(INC) $(SRC)/cJSON_Utils.c

clean:
	rm -rf *o srthub
