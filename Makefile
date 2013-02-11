CFLAGS = -O3 -Wall -ffast-math -fexpensive-optimizations -fomit-frame-pointer -maccumulate-outgoing-args -fno-exceptions
LIBS = -lasound -lglut -lGLU -lGL -L/usr/X11R6/lib -lXmu -lXi -lXext -lX11 -lsndfile -lpthread -lrt -lm
SRCS = timeliner_run.cpp timeliner_cache.cpp alsa.cpp
HDRS = timeliner_diagnostics.h timeliner_cache.h
OBJS = timeliner_run.o timeliner_cache.o alsa.o

all: teststereo

clean:
	rm -f timeliner_run $(OBJS) timeliner.log 
# Windows might not yet be able to run ./timeliner_init.rb to create example/mono/marshal/* .
clean-for-linux:
	rm -f timeliner_run $(OBJS) timeliner.log example/mono/marshal/*

.cpp.o:
	g++ $(CFLAGS) -c $<

timeliner_run.o:   $(HDRS)
timeliner_cache.o: $(HDRS)

timeliner_run: $(OBJS)
	g++ $(CFLAGS) -o $@ $(OBJS) $(LIBS)

test: timeliner_run Makefile example/mono/marshal/mixed.wav
	export ALSA_CARD=0 && ./timeliner_run example/mono/marshal
example/mono/marshal/mixed.wav: timeliner_init.rb example/mono/choral.wav example/mono/config.txt
	cd example && ../timeliner_init.rb mono/marshal mono/config.txt

teststereo: timeliner_run Makefile example/stereo/marshal/mixed.wav
	export ALSA_CARD=0 && ./timeliner_run example/stereo/marshal
example/stereo/marshal/mixed.wav: timeliner_init.rb example/stereo/choral-stereo.wav example/stereo/config.txt
	cd example && ../timeliner_init.rb stereo/marshal stereo/config.txt
