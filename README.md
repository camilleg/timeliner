# timeliner

A browser for long audio recordings.

# How to build and run Timeliner on Ubuntu 10.04 or 12.04

`sudo apt-get install g++ freeglut3-dev gsl-bin libgsl0-dev libglm-dev libsndfile1-dev \`
`libxi-dev libxmu-dev libasound2-dev audiofile-tools`

Install [HTK 3.4.1](http://htk.eng.cam.ac.uk).
(To handle files larger than 2 GB on 64-bit Linux:
after running HTK's `./configure`, but before running `make`:
in all of HTK's subdiectories' Makefiles, remove `-m32` from the `CFLAGS` definitions.)

Optionally install [QuickNet](http://www.icsi.berkeley.edu/Speech/qn.html).

`make`

# How to build Timeliner on Windows

Install [libsndfile](http://www.mega-nerd.com/libsndfile/#Download).
Use the 32-bit version, not the 64-bit.

Install "Microsoft Visual Studio Express 2012 for Windows Desktop."

Install [freeglut](http://freeglut.sourceforge.net), using Visual Studio.
- Build all 4 configurations: debug and release, static and non-static.
- Build them as x64 not Win32 (see the Configuration Manager).

Install [GLM](http://glm.g-truc.net).
- Copy glm/glm.hpp somewhere.

Install [HTK 3.4.1](http://htk.eng.cam.ac.uk).  Corrected instructions:
- Don't bother adding "C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC\bin" to your PATH.
- Instead of VCVARS32, run "C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC\bin\vcvars32.bat" (note the double quotes).
- Ignore every .c file's compiler warnings D9035, D9036, and D9002.  HCopy will still run.
- Either add to your PATH the bin.win32 folder, wherever you put that,
or copy bin.win32/HCopy.exe to some folder on your PATH.

Start Visual Studio.  Open timeliner\timeliner.sln.
Within that solution, in each project, rightclick Properties;
*   For Configuration, choose All Configurations.
    *   In VC++ Directories, adjust the Include Directory and Library Directory to where you installed freeglut and GLM.
    *   In VC++ Directories, add the Include Directory and Library Directory to where you installed libsndfile.  
            (This is smarter than copying `*.lib` and `*.h` into the Timeliner project.)  
            (Beware the different directories `Program Files` and `Program Files (x86)`.)
    *   In Debugging, for Command Arguments specify a marshal dir, e.g. `example\marshal`.

Build the debug and/or release versions of timeliner_run and timeliner_pre.
Run Timeliner (with Ctrl+F5).

(Audio is built on RtAudio, which uses Windows' own DirectSound.)

# How to adjust Timeliner

Read Camille's paper [Effective Browsing of Long Audio Recordings](http://zx81.isl.uiuc.edu/camilleg/acmmm12.pdf).

The maximum width of a vector of values in a node of the agglomerative cache
is the compile-time constant `CQuartet_widthMax` in `timeliner_cache.h`.
