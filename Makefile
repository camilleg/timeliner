MAKEFLAGS += --jobs=99
CFLAGS := -std=c++17 -O3 -Wall -Werror -ffast-math -pedantic
# On a 32-bit OS, CFLAGS += -D_FILE_OFFSET_BITS=64
# CFLAGS += -DNDEBUG 

OBJS     = timeliner_util.o timeliner_diagnostics.o timeliner_cache.o
OBJS_PRE = $(OBJS) timeliner_pre.o
OBJS_RUN = $(OBJS) timeliner_run.o timeliner_util_threads.o timeliner_feature.o alsa.o
OBJS_ALL = $(sort $(OBJS_RUN) $(OBJS_PRE))

LIBS_PRE := -lsndfile -lgsl -lgslcblas
LIBS_RUN := -lsndfile -lasound -lGLEW -lglut -lGLU -lGL -lpng -lpthread

# Optional file containing debugging options for CFLAGS and LIBS_*.
-include Rules.debug

all: test-mono

timeliner_run: $(OBJS_RUN)
	g++ $(CFLAGS) -o $@ $(OBJS_RUN) $(LIBS_RUN)
timeliner_prp: $(OBJS_PRE)
	g++ $(CFLAGS) -o $@ $(OBJS_PRE) $(LIBS_PRE)

# Testcases don't depend on e.g. example/mono/marshal/mixed.wav,
# because that's a symlink with an irrelevant timestamp.

test-mono: timeliner_run Makefile
	./timeliner_run example/mono/marshal
example/mono/marshal/mixed.wav: timeliner_prp example/mono/choral.wav example/mono/config.txt
	cd example && ../timeliner_prp mono/marshal mono/config.txt

test-stereo: timeliner_run Makefile
	./timeliner_run example/stereo/marshal
example/stereo/marshal/mixed.wav: timeliner_prp example/stereo/choral-stereo.wav example/stereo/config.txt
	cd example && ../timeliner_prp stereo/marshal stereo/config.txt

test-openhouse: timeliner_run Makefile
	export timeliner_zoom=2 && ./timeliner_run example/openhouse/marshal
example/openhouse/marshal/mixed.wav: timeliner_prp example/openhouse/config.txt
	cd example && ../timeliner_prp openhouse/marshal openhouse/config.txt

test-EEG: timeliner_run Makefile
	./timeliner_run example/EEG/marshal
example/EEG/marshal/mixed.wav: timeliner_prp /r/timeliner/testcases/eeg/eeg.rec example/EEG/config.txt
	cd example && ../timeliner_prp EEG/marshal EEG/config.txt

test-farm: timeliner_run Makefile
	export timeliner_zoom=6 && ./timeliner_run example/farm/marshal
example/farm/marshal/mixed.wav: example/farm/config.txt
	cd example && ../timeliner_prp farm/marshal farm/config.txt
	# Cicadas, then bats (ultrasonic mic), then robins and cardinals and other songbirds.
	# HCopy takes 9 minutes.

DEPENDFLAGS = -MMD -MT $@ -MF $(patsubst %.o,.depend/%.d,$@)
%.o: %.cpp
	@mkdir -p .depend
	g++ $(CFLAGS) $(DEPENDFLAGS) -c $<
-include $(patsubst %.o,.depend/%.d,$(OBJS_ALL))

clean:
	rm -rf timeliner_run timeliner_prp $(OBJS_ALL) .depend timeliner.log

.PHONY: all clean test-mono test-stereo test-openhouse test-EEG test-farm
