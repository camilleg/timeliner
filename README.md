# timeliner

A browser for long audio recordings.

# How to build and run Timeliner on Ubuntu 10.04 or 12.04

`sudo apt-get install g++ freeglut3-dev gsl-bin libgsl0-dev libsndfile1-dev \`
`libxi-dev libxmu-dev libasound2-dev audiofile-tools`

Install [HTK 3.4.1](http://htk.eng.cam.ac.uk).
(To handle files larger than 2 GB on 64-bit Linux:
after running HTK's `./configure`, but before running `make`:
in all of HTK's subdiectories' Makefiles, remove `-m32` from the `CFLAGS` definitions.)

Install [QuickNet](http://www.icsi.berkeley.edu/Speech/qn.html).

`make`

# How to build Timeliner on Windows

Install "Microsoft Visual Studio Express 2012 for Windows Desktop."
- File, project, new, win32 (not x64), empty.
- Add the source files `*.h` and `*.cpp`.

Install [libsndfile](http://www.mega-nerd.com/libsndfile/#Download).
Use the 32-bit version, not the 64-bit.

Install [freeglut](http://freeglut.sourceforge.net), using Visual Studio.
- Build all 4 configurations: debug and release, static and non-static.
- Build them as x64 not Win32 (see the Configuration Manager).

Start Visual Studio.  In the Timeliner project, rightclick Properties.
*   For Configuration, choose All Configurations.
    *   In VC++ Directories, add the Include Directory and Library Directory where you installed freeglut.
    *   In VC++ Directories, add the Include Directory and Library Directory where you installed libsndfile.  
            (This is smarter than copying `*.lib` and `*.h` into the Timeliner project.)  
            (Beware the different directories `Program Files` and `Program Files (x86)`.)
    *   In Debugging, for Command Arguments specify a marshal dir, e.g. `example\marshal`.
    *   In Linker/Input, additional, prepend `dsound.lib;libsndfile-1.lib;opengl32.lib;freeglut_static.lib;`.
*   For Configuration, choose Debug.
    * In C/C++ Preprocessor, define `FREEGLUT_STATIC`.
    * In C/C++, All Options, Runtime Library, choose multi-threaded debug (no DLL).
*   For Configuration, choose Release.
    * In C/C++ Preprocessor, define `FREEGLUT_STATIC`.
    * In C/C++, All Options, Runtime Library, choose multi-threaded (no DLL).

Build the debug and/or release versions of Timeliner.
Run Timeliner (with Ctrl+F5).

(Audio is built on RtAudio, which uses Windows' own DirectSound.)

# How to adjust Timeliner

Read Camille's paper [Effective Browsing of Long Audio Recordings](http://zx81.isl.uiuc.edu/camilleg/acmmm12.pdf).

The maximum width of a vector of values in a node of the agglomerative cache
is the compile-time constant `CQuartet_widthMax` in `timeliner_cache.h`.
