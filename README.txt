1. How to build and run Timeliner on Ubuntu 10.04 or 12.04.

    sudo apt-get install g++ freeglut3-dev gsl-bin libgsl0-dev libsndfile1-dev ruby1.8 rubygems1.8 sox libxi-dev libxmu-dev libasound2-dev audiofile-tools
    sudo gem install gsl inline narray mmap rspec RubyInline

	If "gem install gsl" fails as it did circa January 2013,
	instead get the latest .tgz at http://rubyforge.org/frs/?group_id=285 ,
	and follow the "setup.rb" instructions in http://rb-gsl.rubyforge.org/ .
	You may even need to apply patches to the files it makes,
	according to https://gist.github.com/kovyrin/1217974 :
	append _with_eps to ext/matrix_complex.c's gsl_matrix_complex_equal()
	and to ext/vector_complex.c's gsl_vector_complex_equal().

    Install HTK 3.4.1 from http://htk.eng.cam.ac.uk .
    (To handle files larger than 2 GB on 64-bit Linux:
    after running HTK's ./configure, but before running its make,
    in all its subdiectories' Makefiles, remove -m32 from the CFLAGS definitions.)

    Install QuickNet from http://www.icsi.berkeley.edu/Speech/qn.html .

    make

2. How to build Timeliner on Windows.

    Install "Microsoft Visual Studio Express 2012 for Windows Desktop."
        File, project, new, win32 (not x64), empty.
	Add the source files *.h and *.cpp.
    Install http://www.mega-nerd.com/libsndfile/#Download .
        (Use the 32-bit version, not the 64-bit.)
    Install http://freeglut.sourceforge.net, using Visual Studio.
	Build all 4 configurations: debug and release, static and non-static.
	Build them as x64 not Win32 (see the Configuration Manager).
    In the Timeliner project, rightclick Properties.
        For Configuration, choose All Configurations.
	    In VC++ Directories, add the Include Directory and Library Directory where you installed freeglut.
	    In VC++ Directories, add the Include Directory and Library Directory where you installed libsndfile.
		(This is smarter than copying *.lib and *.h into the Timeliner project.)
		(Beware the different directories "Program Files" and "Program Files (x86).")
	    In Debugging, for Command Arguments specify a marshal dir, e.g. "example\marshal".
	    In Linker/Input, additional, prepend "dsound.lib;libsndfile-1.lib;opengl32.lib;freeglut_static.lib;"
        For Configuration, choose Debug.
	  In C/C++ Preprocessor, define FREEGLUT_STATIC.
	  In C/C++, All Options, Runtime Library, choose multi-threaded debug (*no* DLL).
        For Configuration, choose Release.
	  In C/C++ Preprocessor, define FREEGLUT_STATIC.
	  In C/C++, All Options, Runtime Library, choose multi-threaded (*no* DLL).
    Build the debug and/or release versions of Timeliner.
    Run Timeliner (with Ctrl+F5).

(Audio is built on RtAudio, which uses Windows' own DirectSound.)

3. How to replicate the "easter egg" experiments with larger data sets.

    100 hours of meeting-room audio (and video) is at http://corpus.amiproject.org/ .

    To extract .wav files from your own audio CD's, use the Linux command: cdparanoia -B

    Short clips useful as easter eggs are at http://www.findsounds.com/types.html .

4. How to adjust Timeliner.

    Read the paper "Effective Browsing of Long Audio Recordings" by Camille Goudeseune,
    http://zx81.isl.uiuc.edu/camilleg/acmmm12.pdf .

    The maximum width of a vector of values in a node of the agglomerative cache
    is the compile-time constant CQuartet_widthMax in timeliner_cache.h.
