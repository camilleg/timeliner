CFLAGS = -O3 -Wall -ffast-math -fexpensive-optimizations -fomit-frame-pointer -maccumulate-outgoing-args -fno-exceptions
HDRS = timeliner_diagnostics.h timeliner_cache.h timeliner_diagnostics.h timeliner_util.h

OBJS     = timeliner_util.o timeliner_diagnostics.o timeliner_cache.o
OBJS_RUN = $(OBJS) timeliner_run.o alsa.o
OBJS_PRE = $(OBJS) timeliner_pre.o
OBJS_ALL = $(OBJS_RUN) $(OBJS_PRE)

LIBS_RUN = -lasound -lglut -lGLU -lGL -L/usr/X11R6/lib -lXmu -lXi -lXext -lX11 -lsndfile -lpthread -lrt
LIBS_PRE = -lsndfile -lgsl -lblas 

all: testmono

clean:
	rm -f timeliner_run $(OBJS_ALL) timeliner.log 
# Windows might not yet be able to run ./timeliner_pre to create example/mono/marshal/* .
clean-for-linux:
	rm -f timeliner_run $(OBJS_ALL) timeliner.log example/mono/marshal/*

.cpp.o: $(HDRS)
	g++ $(CFLAGS) -c $<

timeliner_run: $(OBJS_RUN)
	g++ $(CFLAGS) -o $@ $(OBJS_RUN) $(LIBS_RUN)
timeliner_pre: $(OBJS_PRE)
	g++ $(CFLAGS) -o $@ $(OBJS_PRE) $(LIBS_PRE)

teststereo: timeliner_run Makefile example/stereo/marshal/mixed.wav
	export ALSA_CARD=0 && ./timeliner_run example/stereo/marshal
example/stereo/marshal/mixed.wav: timeliner_pre example/stereo/choral-stereo.wav example/stereo/config.txt
	cd example && ../timeliner_pre stereo/marshal stereo/config.txt

testmono: timeliner_run Makefile example/mono/marshal/mixed.wav
	export ALSA_CARD=0 && ./timeliner_run example/mono/marshal
example/mono/marshal/mixed.wav: timeliner_pre example/mono/choral.wav example/mono/config.txt
	cd example && ../timeliner_pre mono/marshal mono/config.txt
