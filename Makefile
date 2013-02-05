CFLAGS = -O3 -Wall -ffast-math -fexpensive-optimizations -fomit-frame-pointer -maccumulate-outgoing-args -fno-exceptions
LIBS = -lasound -lglut -lGLU -lGL -L/usr/X11R6/lib -lXmu -lXi -lXext -lX11 -lsndfile -lpthread -lrt -lm
SRCS = timeliner_run.cpp timeliner_cache.cpp alsa.cpp
HDRS = timeliner_diagnostics.h timeliner_cache.h
OBJS = timeliner_run.o timeliner_cache.o alsa.o

all: test

clean:
	rm -f timeliner_run $(OBJS) timeliner.log example/marshal/*

.cpp.o:
	g++ $(CFLAGS) -c $<

timeliner_run.o:   $(HDRS)
timeliner_cache.o: $(HDRS)

timeliner_run: $(OBJS)
	g++ $(CFLAGS) -o $@ $(OBJS) $(LIBS)

test: timeliner_run Makefile example/mixed.wav
	export ALSA_CARD=0 && ./timeliner_run example/marshal
example/mixed.wav: timeliner_init.rb example/choral.wav example/beeps
	./timeliner_init.rb example/marshal example/config.txt
