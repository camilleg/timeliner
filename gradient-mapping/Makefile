CFLAGS := -I.. -Wall -fno-exceptions -pedantic -O3
CFLAGS += -DNDEBUG
# CFLAGS += -g -O0

HDRS = ../timeliner_cache.h

OBJS     = timeliner_cache.o
OBJS_RUN = $(OBJS) timeliner_run.o 
OBJS_ALL = $(OBJS_RUN)

LIBS_RUN = -lglut -lGLU -lGL -lGLEW -L/usr/X11R6/lib -lXmu -lXi -lXext -lX11

all: testmono

clean:
	rm -f timeliner_run $(OBJS_ALL) timeliner.log 

# "Pattern rules" may have dependencies, unlike "Suffix rules."
%.o : %.cpp $(HDRS) Makefile
	g++ $(CFLAGS) -c $<

timeliner_run: $(OBJS_RUN)
	g++ $(CFLAGS) -o $@ $(OBJS_RUN) $(LIBS_RUN)

testmono: timeliner_run
	./timeliner_run
