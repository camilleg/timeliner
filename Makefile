CFLAGS = -O3 -Wall -ffast-math -fexpensive-optimizations -fomit-frame-pointer -maccumulate-outgoing-args -pedantic
# When compiling on a 32-bit OS, add -D_FILE_OFFSET_BITS=64
# CFLAGS += -DNDEBUG 
# CFLAGS += -g -O0

HDRS = timeliner_diagnostics.h timeliner_cache.h timeliner_util.h timeliner_util_threads.h timeliner_feature.h

OBJS     = timeliner_util.o timeliner_diagnostics.o timeliner_cache.o
OBJS_PRE = $(OBJS) timeliner_pre.o
OBJS_RUN = $(OBJS) timeliner_run.o timeliner_util_threads.o timeliner_feature.o alsa.o
OBJS_ALL = $(OBJS_RUN) $(OBJS_PRE)

LIBS_PRE = -lsndfile -lgsl -lblas 
LIBS_RUN = -lasound -lglut -lGLU -lGL -lGLEW -L/usr/X11R6/lib -lXmu -lXi -lXext -lX11 -lsndfile -lpng -lpthread -lrt

all: test-mono

clean:
	rm -f timeliner_run timeliner_prp $(OBJS_ALL) timeliner.log example/mono/marshal/*

.cpp.o: $(HDRS)
	g++ $(CFLAGS) -c $<

timeliner_run: $(OBJS_RUN)
	g++ $(CFLAGS) -o $@ $(OBJS_RUN) $(LIBS_RUN)
timeliner_prp: $(OBJS_PRE)
	g++ $(CFLAGS) -o $@ $(OBJS_PRE) $(LIBS_PRE)

# Testcases don't depend on e.g. example/mono/marshal/mixed.wav,
# because that's a symlink with an irrelevant timestamp.

test-EEG: timeliner_run Makefile
	./timeliner_run example/EEG/marshal
example/EEG/marshal/mixed.wav: timeliner_prp /r/timeliner/testcases/eeg/eeg.rec example/EEG/config.txt
	cd example && ../timeliner_prp EEG/marshal EEG/config.txt

test-stereo: timeliner_run Makefile
	./timeliner_run example/stereo/marshal
example/stereo/marshal/mixed.wav: timeliner_prp example/stereo/choral-stereo.wav example/stereo/config.txt
	cd example && ../timeliner_prp stereo/marshal stereo/config.txt

test-mono: timeliner_run Makefile
	./timeliner_run example/mono/marshal
example/mono/marshal/mixed.wav: timeliner_prp example/mono/choral.wav example/mono/config.txt
	cd example && ../timeliner_prp mono/marshal mono/config.txt
