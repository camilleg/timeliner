### Timeliner

A browser for long audio recordings.

1-minute [example video](http://vimeo.com/88076447).

6-page [description](http://zx81.isl.uiuc.edu/camilleg/acmmm12.pdf).

News story giving [historical context](http://www.ece.illinois.edu/mediacenter/article.asp?id=7568).

### Building on Ubuntu 10.04 or 12.04

`sudo apt-get install g++ freeglut3-dev gsl-bin libgsl0-dev libglm-dev libsndfile1-dev libxi-dev libxmu-dev libasound2-dev audiofile-tools libglew-dev libpng12-dev`

Install [HTK 3.4.1](http://htk.eng.cam.ac.uk).
(For audio files exceeding 2 GB on 64-bit Linux:
after running HTK's `./configure`, but before running `make`,
in all of HTK's subdiectories' Makefiles, remove `-m32` from the `CFLAGS` definitions.)

Optionally install [QuickNet](http://www.icsi.berkeley.edu/Speech/qn.html).

`make`

### Building on Windows

Install [libsndfile](http://www.mega-nerd.com/libsndfile/#Download).
Use the 32-bit version, not the 64-bit one.

Install Microsoft Visual Studio Express 2012 for Windows Desktop.

Install [freeglut](http://freeglut.sourceforge.net), using Visual Studio.
- Build as Win32, not x64 (see the Configuration Manager).
- Build all 4 configurations: debug and release, static and non-static.

Install [GLM](http://glm.g-truc.net).
- Copy the folder `glm` (that contains `glm.hpp`, etc.) somewhere.

Install [HTK 3.4.1](http://htk.eng.cam.ac.uk).  Corrected instructions:
- Don't bother adding "C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC\bin" to your PATH.
- Instead of VCVARS32, run "C:\Program Files (x86)\Microsoft Visual Studio 11.0\VC\bin\vcvars32.bat" (note the double quotes).
- Ignore every .c file's compiler warnings D9035, D9036, and D9002.  HCopy will still run.
- Either add to your PATH the folder `bin.win32`, wherever you'd put that,
or copy `bin.win32/HCopy.exe` to some folder on your PATH.

Install [libpng](http://gnuwin32.sourceforge.net/packages/libpng.htm).

Install [zlib](http://zlib.net).

Start Visual Studio.  Open timeliner\timeliner.sln.
Within that solution, in each project, rightclick Properties;
*   For Configuration, choose All Configurations.
    *   In VC++ Directories, add the Include Directory and Library Directory to where you installed libsndfile, freeglut, GLM, libpng, and zlib.
            (This is smarter than copying `*.lib` and `*.h` into the Timeliner project.)  
            (Beware the different directories `Program Files` and `Program Files (x86)`.)
    *   In Debugging, for Command Arguments specify:
    	* for project `timeliner_pre`, a marshal dir and a config file, e.g. `example\stereo\marshal example\stereo\config.txt`.
    	* for project `timeliner`, a marshal dir, e.g. `example\stereo\marshal`.

Build the debug and/or release versions of projects `timeliner_pre` and `timeliner`.

Copy `freeglut.dll` and `libsndfile-1.dll` to `timeliner\Debug` and `timeliner\Release`.

Run the projects `timeliner_pre` and `timeliner` (hit Ctrl+F5).

(Audio is built on RtAudio, which uses Windows' own DirectSound.)

### How to adjust

The maximum width of a vector of values in a node of the agglomerative cache
is the compile-time constant `CQuartet_widthMax` in `timeliner_cache.h`.
