CFLAGS = -O3 -Wall -ffast-math -fexpensive-optimizations -fomit-frame-pointer -maccumulate-outgoing-args -fno-exceptions -pedantic
# When compiling on a 32-bit OS, add -D_FILE_OFFSET_BITS=64
# CFLAGS += -DNDEBUG 
# CFLAGS += -g -O0

HDRS = timeliner_diagnostics.h timeliner_cache.h timeliner_util.h

OBJS     = timeliner_util.o timeliner_diagnostics.o timeliner_cache.o
OBJS_PRE = $(OBJS) timeliner_pre.o
OBJS_RUN = $(OBJS) timeliner_run.o alsa.o
OBJS_ALL = $(OBJS_RUN) $(OBJS_PRE)

LIBS_PRE = -lsndfile -lgsl -lblas 
LIBS_RUN = -lasound -lglut -lGLU -lGL -L/usr/X11R6/lib -lXmu -lXi -lXext -lX11 -lsndfile -lpthread -lrt

all: testmono

clean:
	rm -f timeliner_run timeliner_pre $(OBJS_ALL) timeliner.log 
# Windows might not yet be able to run ./timeliner_pre to create example/mono/marshal/* .
clean-for-linux:
	rm -f timeliner_run timeliner_pre $(OBJS_ALL) timeliner.log example/mono/marshal/*

.cpp.o: $(HDRS)
	g++ $(CFLAGS) -c $<

timeliner_run: $(OBJS_RUN)
	g++ $(CFLAGS) -o $@ $(OBJS_RUN) $(LIBS_RUN)
timeliner_pre: $(OBJS_PRE)
	g++ $(CFLAGS) -o $@ $(OBJS_PRE) $(LIBS_PRE)

testEEG: timeliner_run Makefile example/EEG/marshal/mixed.wav
	export ALSA_CARD=0 && ./timeliner_run example/EEG/marshal
example/EEG/marshal/mixed.wav: timeliner_pre /r/timeliner/testcases/eeg/eeg.rec example/EEG/config.txt
	cd example && ../timeliner_pre EEG/marshal EEG/config.txt

teststereo: timeliner_run Makefile example/stereo/marshal/mixed.wav
	export ALSA_CARD=0 && ./timeliner_run example/stereo/marshal
example/stereo/marshal/mixed.wav: timeliner_pre example/stereo/choral-stereo.wav example/stereo/config.txt
	cd example && ../timeliner_pre stereo/marshal stereo/config.txt

testmono: timeliner_run Makefile example/mono/marshal/mixed.wav
	export ALSA_CARD=0 && ./timeliner_run example/mono/marshal
example/mono/marshal/mixed.wav: timeliner_pre example/mono/choral.wav example/mono/config.txt
	cd example && ../timeliner_pre mono/marshal mono/config.txt
