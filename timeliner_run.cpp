#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS		// for strcpy, getenv, _snprintf, fopen, fscanf
#endif

#undef DEBUG

#include "timeliner_common.h"
#include "timeliner_diagnostics.h"

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>

#ifdef _MSC_VER
#include <windows.h>
#include <time.h>
#include <iostream>
#include <sstream>
#else
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <linux/soundcard.h>
#include <unistd.h>
#endif

#include <sndfile.h> // aptitude install libsndfile1-dev, or www.mega-nerd.com/libsndfile/ libsndfile-1.0.25-w64-setup.exe
#include <vector>

#include <GL/glut.h>

#if !defined(GLUT_WHEEL_UP)
#define GLUT_WHEEL_UP   (3)
#define GLUT_WHEEL_DOWN (4)
#endif

using namespace std;

#include "timeliner_cache.h"

#ifdef _MSC_VER
void snooze(double sec) { Sleep(DWORD(sec * 1e3)); }
#else
void snooze(double sec) { (void)usleep(sec * 1e6); }
#endif

inline double sq(double _) { return _*_; }

#ifdef _MSC_VER
#include <windows.h>
#include <time.h>
#include <iostream>
 
#if defined(_MSC_VER) || defined(_MSC_EXTENSIONS)
  #define DELTA_EPOCH_IN_MICROSECS  11644473600000000Ui64
#else
  #define DELTA_EPOCH_IN_MICROSECS  11644473600000000ULL
#endif
 
struct timezone
{
  int  tz_minuteswest; /* minutes W of Greenwich */
  int  tz_dsttime;     /* type of dst correction */
};
int gettimeofday(struct timeval *tv, struct timezone *tz)
{
// Define a structure to receive the current Windows filetime
  FILETIME ft;
 
// Initialize the present time to 0 and the timezone to UTC
  unsigned __int64 tmpres = 0;
  static int tzflag = 0;
 
  if (NULL != tv)
  {
    GetSystemTimeAsFileTime(&ft);
 
// The GetSystemTimeAsFileTime returns the number of 100 nanosecond 
// intervals since Jan 1, 1601 in a structure. Copy the high bits to 
// the 64 bit tmpres, shift it left by 32 then or in the low 32 bits.
    tmpres |= ft.dwHighDateTime;
    tmpres <<= 32;
    tmpres |= ft.dwLowDateTime;
 
// Convert to microseconds by dividing by 10
    tmpres /= 10;
 
// The Unix epoch starts on Jan 1 1970.  Need to subtract the difference 
// in seconds from Jan 1 1601.
    tmpres -= DELTA_EPOCH_IN_MICROSECS;
 
// Finally change microseconds to seconds and place in the seconds value. 
// The modulus picks up the microseconds.
    tv->tv_sec = (long)(tmpres / 1000000UL);
    tv->tv_usec = (long)(tmpres % 1000000UL);
  }
 
  if (NULL != tz)
  {
    if (!tzflag)
    {
      _tzset();
      tzflag++;
    }
  
	 // Adjust for the timezone west of Greenwich
     long tmp;
	 _get_timezone(&tmp);
	 tz->tz_minuteswest = tmp / 60;
    _get_daylight(&tz->tz_dsttime);
  }
 
  return 0;
}
#endif

// seconds since app started
double appnow()
{
  static bool fFirst = true;
  static struct timeval t0;
  if (fFirst) {
    fFirst = false;
    if (gettimeofday(&t0, NULL) < 0) {
      perror("appnow() first time");
      return -1;
    }
  }
  struct timeval t;
  if (gettimeofday(&t, NULL) < 0) {
    perror("appnow()");
    return -1;
  }
  return (t.tv_sec - t0.tv_sec) + (t.tv_usec - t0.tv_usec) / 1e6;
}

class Logger {
public:
  Logger(const char* filename): _o(filename) {}
  ~Logger() { _o.close(); }
  void warn (const string& _) { _log(_, 'W', " WARN"); }
  void info (const string& _) { _log(_, 'I', " INFO"); }
  void debug(const string& _) { _log(_, 'D', "DEBUG"); }
private:
  ofstream _o;
  void _log(const string& _, char c, const char* name) {
    _o << c << ", [" << appnow() << "] " << name << ": " << _ << "\n";
  }
};
const char* logfilename = "./timeliner.log";
Logger* applog = NULL;

double tFPSPrev = appnow();
double secsPerFrame = 1/60.0;

double tShowBound[2] = {0.0, 90000.0};
double tShowPrev[2] = {-2.0, -1.0};	// deliberately bogus
double tShow[2] = {tShowBound[0], tShowBound[1]};	// displayed interval of time, updated each frame
double tAim[2] = {tShow[0], tShow[1]};	// setpoint for tShow, updated more slowly

inline double dsecondFromGL(double dx) { return dx * (tShow[1] - tShow[0]); }
inline double secondFromGL(double x) { return dsecondFromGL(x) + tShow[0]; }
inline double glFromSecond(double s) { return (s - tShow[0]) / (tShow[1] - tShow[0]); }

void testConverters()
{
  for (double i = -30.0; i < 50.0; ++i) {
    assert(abs(i - glFromSecond(secondFromGL(i))) < 1e-10);
    assert(abs(i - secondFromGL(glFromSecond(i))) < 1e-10);
  }
}

long wavcsamp = -1L; // int wraps around too soon
short* wavS16 = NULL;

bool onscreen(double sec) { return sec >= tShow[0] && sec <= tShow[1]; }

#ifdef _MSC_VER
#include <process.h>
#else
#include <pthread.h>
#endif

// Windows version and extra features are in syzygy's language/arThread.h,
// from which this is excerpted.
class arLock {
#ifdef _MSC_VER
  HANDLE _mutex;
  bool _fOwned; // Owned by this app.  Does NOT mean "locked."
  bool _fLocked;
#else
  pthread_mutex_t _mutex;
#endif
  const char* _name;
public:

#ifdef _MSC_VER
  arLock(const char* name) : _fOwned(true) {
    _setName( name );
    _mutex = CreateMutex(NULL, FALSE, NULL);
    const DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS && _mutex) {
      // Another app has this.  (Only do this with global things like arLogStream.)
      _fOwned = false;
    }
	if (_mutex != NULL)
	  // success
      return;
    if (e == ERROR_ALREADY_EXISTS) {
      cerr << "arLock warning: CreateMutex('" << name <<
        "') failed (already exists).\n";
      return;
    }
    if (e == ERROR_ACCESS_DENIED) {
      cerr << "arLock warning: CreateMutex('" << name <<
        "') failed (access denied); backing off.\n";
LBackoff:
      // _mutex = OpenMutex(SYNCHRONIZE, FALSE, name);
      // Fall back to a mutex of scope "app" not "the entire PC".
      _mutex = CreateMutex(NULL, FALSE, NULL);
      if (!_mutex) {
        cerr << "arLock warning: failed to create mutex.\n";
      }
    }
    else if (e == ERROR_PATH_NOT_FOUND) {
      cerr << "arLock warning: CreateMutex('" << name <<
        "') failed (backslash?); backing off.\n";
      goto LBackoff;
    }
    else {
      cerr << "arLock warning: CreateMutex('" << name <<
        "') failed; backing off.\n";
      goto LBackoff;
    }
  }

  void _setName( const char* name ) {
    if (name != NULL) {
      _name = new char[strlen(name)+1];
      memcpy( (void*)_name, name, strlen(name)+1 );
    } else {
      _name = new char[7];
      memcpy( (void*)_name, "NONAME", 7 );
    }
  }

  bool valid() const { return _mutex != NULL; }

  ~arLock() {
    if (_fOwned && _mutex) {
      (void)ReleaseMutex(_mutex); // paranoid
      CloseHandle(_mutex);
    }
    delete[] _name;
  }

  void lock() {
    if (!valid()) return;
    const DWORD msecTimeout = 3000;
    for (;;) {
      const DWORD r = WaitForSingleObject( _mutex, msecTimeout );
      switch (r) {
      case WAIT_OBJECT_0:
	_fLocked = true;
	return;
      default:
      case WAIT_ABANDONED:
	// Another thread terminated without releasing _mutex.
	// << "arLock acquired abandoned lock.\n";
	return;
      case WAIT_TIMEOUT:
	break;
      case WAIT_FAILED:
	const DWORD e = GetLastError();
	if (e == ERROR_INVALID_HANDLE) {
	  // << "arLock warning: invalid handle.\n";
	  // _mutex is bad, so stop using it.
	  _mutex = NULL;
	  // Desperate fallback: create a fresh (unnamed) mutex.
	  _mutex = CreateMutex(NULL, FALSE, NULL);
	  if (_mutex)
	    continue;
	  // << "arLock unrecoverably failed to recreate handle.\n";
	}
	else {
	  // << "arLock internal error: GetLastError()==" << e << ".\n";
	}
	return;
      }
    }
  }
  void unlock() {
    if (!ReleaseMutex(_mutex)) {
      // << "arLock warning: failed to unlock.\n";
      CloseHandle(_mutex);
      _mutex = NULL;
    };
  }

#else

  arLock(const char* name) : _name(name) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);
    pthread_mutex_init(&_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    //fprintf(stderr, "%s created\n", _name);
  }
  ~arLock() {
    //fprintf(stderr, "%s destroy\n", _name);
    pthread_mutex_destroy(&_mutex);
  }

  void lock() {
    //fprintf(stderr, "%s lock\n", _name);
    // A relative time (a duration) requires pthread_mutex_timedlock_np
    // or pthread_mutex_reltimedlock_np, but Ubuntu doesn't define those.
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += 1; // Overkill.  Slower than that really shouldn't happen with only 3 threads.
    const int r = pthread_mutex_timedlock(&_mutex, &t);
    if (r != 0) {
      //fprintf(stderr, "internal mutex error, %s %d: ", _name, r);
      switch(r) {
	case EINVAL: fprintf(stderr, "EINVAL\n"); break;
	case ETIMEDOUT: fprintf(stderr, "ETIMEDOUT\n"); break;
	case EAGAIN: fprintf(stderr, "EAGAIN\n"); break;
	case EDEADLK: fprintf(stderr, "EDEADLK\n"); break;
	default: fprintf(stderr, "unknown\n"); break;
      }
      quit("internal mutex error");
    }
    //fprintf(stderr, "%s locked\n", _name);
  }
  void unlock() {
    pthread_mutex_unlock(&_mutex);
  }

#endif

};

// Implicitly unlocks when out of scope.
class arGuard {
  arLock& _l;
public:
  arGuard(arLock& l): _l(l) { _l.lock(); }
  ~arGuard() { _l.unlock(); }
};

arLock vlockAudio("vlockAudio"); // guards next three
int vnumSamples = 0; // high-low-water-mark between samplewriter() and samplereader()
const short* vpsSamples = NULL; // vpsSamples and visSamples are set by samplewriter() via emit(), and cleared by samplereader().
int visSamples = 0;

class S2Splay {
public:
  S2Splay() : _lock("s2s"), _sPlay(-1.0), _tPlay(appnow()), _fPlaying(false), _sampBgn(10000) {}
  bool playing() const { arGuard _(_lock); return _fPlaying; }
  void soundpause() { arGuard _(_lock); _fPlaying = false; }

  // Stuff dst with positions of playback cursors, in seconds or opengl offsets.

  bool sPlayCursors(double* dst) const {
    arGuard _(_lock);
    if (!_fPlaying)
      return false;
    dst[0] = _sPlay;
    dst[1] = sPlayCursor1Nolock();
    return true;
  }
  bool xPlayCursors(double* dst) const {
    arGuard _(_lock);
    if (!_fPlaying)
      return false;
    dst[0] = glFromSecond(_sPlay);
    dst[1] = glFromSecond(sPlayCursor1Nolock());
    // sPlayCursors(), with glFromSecond *inside* the lock.
    return true;
  }

  // Report position of the next numSamples of audio,
  // starting implicitly at _sampBgn,
  // by setting vpsSamples and visSamples,
  // and updating _sampBgn for the next emit().
  //
  // Called from inside vlockAudio.lock();
  void emit(int numSamples /* actually is vnumSamples */) {
    assert(vnumSamples > 0); // true but not helpful
    arGuard _(_lock);
    long sampEnd = _sampBgn + numSamples;
    const bool fPastEnd = sampEnd > wavcsamp;
    if (fPastEnd) {
      // truncate, and cease playing thereafter
      sampEnd = wavcsamp;
      numSamples = sampEnd - _sampBgn;
    }
    const short* psStart = wavS16 + _sampBgn;
    vpsSamples = psStart;
    visSamples += numSamples;
    _sampBgn += numSamples;
    //not true when hit space to stop playing. assert(_fPlaying);
    if (fPastEnd)
      _fPlaying = false;

    // Stop playing if cursor moves offscreen, or screen moves off cursor.
    _fPlaying &= onscreen(sPlayCursor1Nolock());
  }

  void spacebar(double s) {
    bool f;
    {
      arGuard _(_lock);
      f = _fPlaying;
      // If s no longer equals _sPlayPrev,
      // either user moved purple cursor during playback
      // which means he wants to keep listening from the new position;
      // or user panned purple cursor left-offscreen
      // in which case resuming playback from left edge is a bug,
      // to be fixed by defining yet another flag to catch either that case,
      // or the user's explicit repositioning of purple cursor (that's better).
      if (f && s==_sPlayPrev) {
	_fPlaying = false;
      } else {
	_sPlayPrev = s;
	soundplayNolock(s);
      }
    }
  }

private:
  // Start playing at offset of t seconds,
  // primarily by setting _sampBgn so emit() knows where to look for samples.
  void soundplayNolock(double t) {
    const long sampBgn = long(double(SR) * t);
    if (sampBgn<0 || sampBgn > wavcsamp) {
      warn("play out of range");
      return;
    }
    // arGuard _(_lock); *would* go here.
    _sampBgn = sampBgn;
    _tPlay = appnow();
    _sPlay = t;
    _fPlaying = true;
    //info("playing at #{t}");
  }

  double sPlayCursor1Nolock() const { return _sPlay + (appnow() - _tPlay); }

  mutable arLock _lock; // guards all member variables
  double _sPlay;
  double _sPlayPrev;
  double _tPlay;
  bool _fPlaying;
  long _sampBgn;
};

S2Splay s2s;

GLuint texNoise;
const double YTick = 0.03;
int pixelSize[2] = {1150, 550};

double dxChar = 0.01;
#define font GLUT_BITMAP_9_BY_15

// As z from 0 to 1, lerp from a to b.
inline float lerp(const float z, const float a, const float b) { return z*b + (1.0F-z)*a; }
inline double lerp(const double z, const double a, const double b) { return z*b + (1.0-z)*a; }

// To set color: *before* glRasterPos2d, call glColor (with lighting disabled).
char sprintfbuf[10000];
void putsGlut(const char* pch = sprintfbuf)
{
  while (*pch) glutBitmapCharacter(font, *pch++);
}

class Mmap {
  char* _pch;
#ifdef _MSC_VER
  LARGE_INTEGER _cch;
  HANDLE h, h2;
#else
  size_t _cch;
  int _fd;
#endif

public:
#ifdef _MSC_VER
  Mmap(const string& szFilename, bool fOptional=true) : _pch(NULL) {
    h = CreateFile(
		std::wstring(szFilename.begin(), szFilename.end()).c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
      if (!fOptional)
	    warn("problem opening file" + szFilename);
      return;
    }
    h2 = CreateFileMapping(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (h2 == NULL)
      warn("problem #2 opening file" + szFilename);
    _pch = (char*)MapViewOfFile(h2, FILE_MAP_READ, 0, 0, 0);

    _cch.QuadPart = 0;
    if (GetFileSizeEx(h, &_cch) == 0)
      warn("problem measuring file" + szFilename);
  }
  ~Mmap() {
	if (h != INVALID_HANDLE_VALUE) {
      UnmapViewOfFile(_pch);
	  if (h2 != NULL)
		CloseHandle(h2);
	  CloseHandle(h);
	}
  }
  const size_t cch() const { return size_t(_cch.QuadPart); }
#else
  Mmap(const string& szFilename, bool fOptional=true) : _pch(NULL), _cch(0), _fd(-1) {
    _fd = open(szFilename.c_str(), O_RDONLY);
    if (_fd < 0) {
      if (!fOptional)
	warn("problem opening file" + szFilename);
      return;
    }
    struct stat s;
    fstat(_fd, &s);
    _cch = s.st_size; // possibly 0, for an empty file
    _pch = (char*)mmap(NULL, _cch, PROT_READ, MAP_SHARED, _fd, 0);
    if (_pch == MAP_FAILED) {
      _pch = NULL;
      warn("mmap failed");
    }
  }

  ~Mmap() {
    if (_pch && munmap(_pch, _cch) == -1)
      warn("munmap failed");
    if (_fd >= 0)
      close(_fd);
  }

  const size_t cch() const { return _cch; }
#endif

  const char*  pch() const { return _pch; }
  const bool valid() const { return _pch != NULL; } // Better would be C++11 safe-bool explicit operator bool() const;
};

void prepTexture(GLuint t)
{
  glBindTexture(GL_TEXTURE_2D, t);
  assert(glIsTexture(t) == GL_TRUE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

void prepTextureMipmap(GLuint t)
{
  glBindTexture(GL_TEXTURE_1D, t);
  assert(glIsTexture(t) == GL_TRUE);
  // Prevent bleeding onto opposite edges like a torus.
#ifndef GL_CLAMP_TO_EDGE
  #define GL_CLAMP_TO_EDGE 0x812F
#endif
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glTexParameterf(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
  //needed? glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_PRIORITY, 0.99);
}

float gpuMBavailable()
{
  // glerror GL_OUT_OF_MEMORY ?
  // kernel: [4391985.748987] NVRM: VM: nv_vm_malloc_pages: failed to allocate contiguous memory

  // http://developer.download.nvidia.com/opengl/specs/GL_NVX_gpu_memory_info.txt
  // AMD/ATI equivalent: WGL_AMD_gpu_association wglGetGPUIDsAMD wglGetGPUIDsAMD wglGetGPUInfoAMD
  // www.opengl.org/registry/specs/ATI/meminfo.txt
#if 0
  #define GL_GPU_MEM_INFO_TOTAL_AVAILABLE_MEM_NVX 0x9048
  GLint total_mem_kb = 0;
  glGetIntegerv(GL_GPU_MEM_INFO_TOTAL_AVAILABLE_MEM_NVX, &total_mem_kb);
  // zx81 has 1048576 kb = 1 GB.
#endif
  #define GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX 0x9049
  GLint cur_avail_mem_kb = 0;
  glGetIntegerv(GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX, &cur_avail_mem_kb);
  return cur_avail_mem_kb/1000.0F;

  // A feature of width 65536 (almost 11 minutes at 100 samples/sec)
  // uses ~145 MB texture RAM.
  // That's 8 chunks of width 8192.
  // 1 chunk is 8192x1 texels * 2 for mipmap * 3 for GL_RGB * 62 vectorsize = 3047424 B = 2.9MB;
  // 8 chunks is 23 MB.  But 145MB is used?!  (RGBA not just RGB?)
}

class Feature {

  enum { vecLim = CQuartet_widthMax+1 }; // from timeliner_cache.h
  static int mb;
  enum { mbUnknown, mbZero, mbPositive };

  class Slartibartfast {
  public:
    GLuint tex[vecLim];
  };

public:
  int cchunk;
  vector<Slartibartfast> rgTex;

  Feature(int /*iColormap*/, const char* filename, const char* dirname): _fValid(false) {
    if (mb == mbUnknown) {
      mb = gpuMBavailable() > 0.0f ? mbPositive : mbZero;
      if (!hasGraphicsRAM())
	warn("Found no dedicated graphics RAM.  May run slowly.");
    }
    const Mmap foo(string(dirname) + "/" + string(filename));
    if (!foo.valid())
      return;
    binaryload(foo.pch(), foo.cch()); // stuff many member variables
    makeMipmaps();
    _fValid = true;
    // ~Mmap closes file
  };

  bool hasGraphicsRAM() const { return mb == mbPositive; }

  void binaryload(const char* pch, long cch) {
    assert(4 == sizeof(float));
    strcpy(_name, pch);				pch += strlen(_name) + 1;
    _iColormap = int(*(float*)pch);		pch += sizeof(float);
    _period = *(float*)pch;			pch += sizeof(float);
    const int slices = int(*(float*)pch);	pch += sizeof(float);
    _vectorsize = int(*(float*)pch);		pch += sizeof(float);
    _cz = (cch - long(strlen(_name) + 1 + 4*sizeof(float))) / 4;
#ifdef NDEBUG
    _unused(slices);
#else
    assert(slices*_vectorsize == _cz);
#endif
    _pz = (const float*)pch; // _cz floats
  };

  bool fValid() const { return _fValid; }
  int vectorsize() const { return _vectorsize; }
  int samples() const { return _cz / _vectorsize; }
  const char* name() const { return _name; }

  void makeMipmaps() {
    // Adaptive subsample is too tricky, until I can better predict GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX.
    // (Adapt the prediction itself??  Allocate a few textures of various sizes, and measure reported GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX.  But implement this only after getting 2 or 3 different PCs to test it on.)
    // Subsampling to coarser than 100 Hz would be pretty limiting.
    const char* pch = getenv("timeliner_zoom");
    unsigned subsample = pch ? atoi(pch) : 1;
    if (subsample < 1)
      subsample = 1;
    if (subsample > 1)
      printf("Subsampling %dx from env var timeliner_zoom.\n", subsample);

    // width of texture is smallest power of two that's larger than features' # of samples.
    unsigned level;
    const int csample = samples();
    unsigned width = 1; // could end up as large as 1048576, or far more.
    while (width < csample/subsample)
      width *= 2;
    //printf("feature has %d samples, for tex-chunks' width %d.\n", csample, width);

    // Keep cchunk no larger than needed, to conserve RAM and increase FPS.
    {
      GLint widthLim; // often 2048..8192 (zx81 has 8192), rarely 16384, never greater.
      glGetIntegerv(GL_MAX_TEXTURE_SIZE, &widthLim);
      assert(widthLim >= 0); // because width is unsigned
      if (width > unsigned(widthLim)) assert(width%widthLim==0);	// everything is a power of two
      cchunk = width<unsigned(widthLim) ? 1 : width/widthLim;
      //printf("width = %d, cchunk = %d, widthLim = %d\n", width, cchunk, widthLim);
      assert(GLint(width/cchunk) == widthLim);
    }

    rgTex.resize(cchunk);
    for (int ichunk=0; ichunk<cchunk; ++ichunk) {
      glGenTextures(_vectorsize, rgTex[ichunk].tex);
      for (int j=0; j < _vectorsize; ++j)
	prepTextureMipmap(rgTex[ichunk].tex[j]);
    }

    const CHello cacheHTK(_pz, _cz, 1.0f/_period, subsample, _vectorsize);
    glEnable(GL_TEXTURE_1D);
    const float mb0 = hasGraphicsRAM() ? gpuMBavailable() : 0.0f;
    // www.opengl.org/archives/resources/features/KilgardTechniques/oglpitfall/ #5 Not Setting All Mipmap Levels.
    for (level=0; (width/cchunk)>>level >= 1; ++level) {
      //printf("  computing feature %d's mipmap level %d.\n", i, level);
      makeTextureMipmap(cacheHTK, level, width >> level);
    }

    if (hasGraphicsRAM()) {
      const float mb1 = gpuMBavailable();
      printf("Feature used %.1f graphics MB;  %.0f MB remaining.\n", mb0-mb1, mb1);
      if (mb1 < 50.0) {
#ifdef _MSC_VER
	warn("Running out of graphics RAM.  Try setting the environment variable timeliner_zoom to 10 or so, and restarting.");
#else
	// Less than 50 MB free may hang X.  Mouse responsive, Xorg 100% cpu, network up, console frozen.
	warn("Running out of graphics RAM.  Try export timeliner_zoom=15.");
#endif
      }
    }
  }

  const void makeTextureMipmap(const CHello& cacheHTK, int mipmaplevel, int width) const {
    assert(vectorsize() <= vecLim);
    assert(width % cchunk == 0);
    width /= cchunk;

    //printf("    computing %d mipmaps of width %d.\n", cchunk, width);
    //printf("vectorsize %d\n", vectorsize());
    unsigned char* bufByte = new unsigned char[vectorsize()*width];
    for (int ichunk=0; ichunk<cchunk; ++ichunk) {
      const double chunkL = ichunk     / double(cchunk); // e.g., 5/8
      const double chunkR = (ichunk+1) / double(cchunk); // e.g., 6/8
      cacheHTK.getbatchByte(bufByte,
	  lerp(chunkL, tShowBound[0], tShowBound[1]),
	  lerp(chunkR, tShowBound[0], tShowBound[1]),
	  vectorsize(), width, _iColormap);
      for (int j=0; j<vectorsize(); ++j) {
	assert(glIsTexture(rgTex[ichunk].tex[j]) == GL_TRUE);
	glBindTexture(GL_TEXTURE_1D, rgTex[ichunk].tex[j]);

#if 0	// Fails on ubuntu tagalong and Sarah's winVista.  No idea why.
	// Error check.  Faster would be to do this just once, with mipmaplevel==1.
	glTexImage1D(GL_PROXY_TEXTURE_1D, mipmaplevel, GL_INTENSITY8, width, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
	GLenum e = glGetError();
	if (e != GL_NO_ERROR) {
	  printf("%x\n", e);
	  quit("Oh no, glTexImage1D failed.");
	}
#endif
	glTexImage1D(GL_TEXTURE_1D, mipmaplevel, GL_INTENSITY8, width, 0, GL_RED, GL_UNSIGNED_BYTE, bufByte + width*j);
#if 0	// Fails on Sarah's winVista.  No idea why.
	GLenum e = glGetError();
	if (e != GL_NO_ERROR) {
	  printf("%x\n", e); // 0x500 GL_INVALID_ENUM /usr/include/GL/gl.h
	  quit("Oh no, glTexImage1D failed.");
	}
#endif
      }
    }
    delete [] bufByte;
  };

private:
  bool _fValid;
  int _iColormap;
  float _period;
  int _vectorsize;
  const float* _pz;
  long _cz;
  char _name[1000];
};
int Feature::mb = mbUnknown;
vector<Feature*> features;

void drawFeatures()
{
  if (features.empty()) {
    return;
  }

  vector<Feature*>::const_iterator f;

  // Compute y's: amortize among vectorsizes.  Sqrt gives "thin" features more space.
  double rgdy[100]; //hardcoded;;  instead, grow a vector.
  assert(features.size() < 100);
  int i=0;
  double sum = 0.0;
  for (f=features.begin(); f!=features.end(); ++f)
    sum += rgdy[i++] = sqrt(double((*f)->vectorsize()));
  // rgdy[0 .. i-1] are heights.
  double rgy[100]; //hardcoded;;
  rgy[0] = YTick*6.0;
  rgy[i] = 1.0;
  const double rescale = sum / (rgy[i] - rgy[0]);
  int j;
  for (j=1; j<i; ++j)
    rgy[j] = rgy[j-1] + rgdy[j-1] / rescale;
  // rgy [0 .. i] are boundaries between features.

  for (f=features.begin(),i=0; f!=features.end(); ++f,++i) {
    const double* p = rgy + i;
      glDisable(GL_TEXTURE_2D);
      glEnable(GL_TEXTURE_1D);
      const int jMax = (*f)->vectorsize();
      glColor4d(0.9,1.0,0.4, 1.0);
      for (int ichunk=0; ichunk<(*f)->cchunk; ++ichunk) {
	for (j=0; j < jMax; ++j) {
	  const double chunkL = ichunk     / double((*f)->cchunk); // e.g., 5/8
	  const double chunkR = (ichunk+1) / double((*f)->cchunk); // e.g., 6/8
	  const double tBoundL = lerp(chunkL, tShowBound[0], tShowBound[1]);
	  const double tBoundR = lerp(chunkR, tShowBound[0], tShowBound[1]);
	  const double xL = (tBoundL - tShow[0]) / (tShow[1] - tShow[0]);
	  const double xR = (tBoundR - tShow[0]) / (tShow[1] - tShow[0]);
	  if (xR < 0.0 || xL > 1.0)
	    continue; // offscreen
	  const double uL = 0.0;
	  const double uR = 1.0;
	  assert(glIsTexture((*f)->rgTex[ichunk].tex[j]) == GL_TRUE);
	  glBindTexture(GL_TEXTURE_1D, (*f)->rgTex[ichunk].tex[j]);
	  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	  const double yMin = lerp(double(j  )/jMax, p[0], p[1]);
	  const double yMax = lerp(double(j+1)/jMax, p[0], p[1]);
	  assert(p[0] <= yMin);
	  assert(yMin <  yMax);
	  assert(yMax <= p[1]);
	  glBegin(GL_QUADS);
	    glTexCoord1d(uL); glVertex2d(xL, yMin);
	    glTexCoord1d(uL); glVertex2d(xL, yMax);
	    glTexCoord1d(uR); glVertex2d(xR, yMax);
	    glTexCoord1d(uR); glVertex2d(xR, yMin);
	  glEnd();
	}
      }
      glDisable(GL_TEXTURE_1D);
    glColor4f(1,1,0,1);
    glRasterPos2d(0.01, p[0] + 0.005);
    putsGlut((*f)->name());
  }
}

double linearlerp(double z, double min, double max)
{
  return z*max + (1.0-z)*min;
}

double geometriclerp(double z, double min, double max)
{
  if (z<0.0 || min<0.0 || max<0.0)
    return -1.0;			// handle error arbitrarily
  z = sqrt(z);
  min = sqrt(min);
  max = sqrt(max);
  const double r = (z-min) / (max-min);
  return r*r;
}

void drawWaveformScaled(const float* minmaxes, double yScale) {
  const double ramp = (1.0/yScale - 5000.0) / (33100.0 - 5000.0) / 11.0; // 1.0 downto 0.009
  glColor4d(linearlerp(ramp,0,0.1), linearlerp(ramp,0.08,1.0), linearlerp(ramp,0.1,0.15), 1.0);
  glPushMatrix();
    glScaled(1.0/pixelSize[0], yScale, 1.0);
    glBegin(GL_LINES);
    for (int x = 0; x < pixelSize[0]; ++x) {
      glVertex2f(GLfloat(x), minmaxes[x*2]);
      glVertex2f(GLfloat(x), minmaxes[x*2+1]);
    }
    glEnd();
  glPopMatrix();
}

double scaleWavFromSampmax(double s)
{
  const double sMin = 500.0;
  if (s < sMin)
    s = sMin;
  return 0.95 * YTick*2 / s;
}
const double scaleWavDefault = scaleWavFromSampmax(32768.0);
double scaleWav = scaleWavDefault;
double scaleWavAim = scaleWavDefault;
CHello* cacheWav = NULL;

void drawWaveform()
{
  // 0 < x < 1
  // y above timeline, and rescale audio values from +-32768.

  // Adaptively scale (vertically zoom).
  // Convert x-pixel extent to [t,t+dt] from [t-dt/2, t+dt/2].  Edge-centered not body-centered, sort of.
  const double hack = (tShow[1] - tShow[0])*0.5 * (pixelSize[0] / (pixelSize[0]-1));

  float dst[2];
  assert(cacheWav);
  cacheWav->getbatch(dst, tShow[0]+hack, tShow[1]+hack, 1, 0.0);
  float& xmin = dst[0];
  float& xmax = dst[1];
  // [xmin.abs, xmax.abs].max is a shortcut for minmaxes.map{|x| x.abs}.max 
  // 0.8 not 0.99, to hint that it's not really maximum amplitude.
  scaleWavAim = scaleWavFromSampmax(max(abs(xmin), abs(xmax)));

  float minmaxes[6000*2]; // hardcoded
  assert(6000 > pixelSize[0]);
  cacheWav->getbatch(minmaxes, tShow[0], tShow[1], pixelSize[0], 1.0/(scaleWav * pixelSize[1]));
  // Last arg avoids vanishingly short vertical lines.

  // As 1/scaleWav from default 331000. to zoomedin 5000.0,
  // color from brightgreen 0.1,1.0,0.2 to cyan 0.0,0.25,0.3,
  // roughly constant #pixels times brightness.

  glPushMatrix();
    glTranslated(0.0, YTick*4, 0.0);
    // Scaled waveform is dark echo behind bright unscaled one.
    drawWaveformScaled(minmaxes, scaleWav);
    drawWaveformScaled(minmaxes, scaleWavDefault);
  glPopMatrix();
}

double sMouseRuler = 0.0;
void drawMouseRuler()
{
  const double xL = glFromSecond(sMouseRuler);
  const double xR = xL + 0.06;
  if (xR < 0.0 || xL > 1.0)
    return; // offscreen
  const double yMid = YTick * 6;
  // Behind waveform.  Don't distort HTK colors.
  glBegin(GL_POLYGON);
    glColor3d(0.12,0,0.12);
    glVertex2d(xL, 0.0);
    glVertex2d(xL, yMid);
    glColor3d(0,0,0);
    glVertex2d(xR, yMid);
    glVertex2d(xR, 0.0);
  glEnd();
  // Sharp onset, drawn after polygons so it's over them.
  glColor3f(1,0,1); // purple
  glBegin(GL_LINE_STRIP);
    glVertex2d(xL, 0.0);
    glVertex2d(xL, 1.0);
  glEnd();
}

class Flash {
public:
  Flash(double fadeFrames=20.0):
    _fadeStep(1.0/fadeFrames), _t(-1.0) {
    memset(_color, 0, sizeof(_color));
  };
  void blink(double t, float r, float g, float b) {
    _t = t;
    _color[0] = _colorFaded[0] = r;
    _color[1] = _colorFaded[1] = g;
    _color[2] = _colorFaded[2] = b;
                _colorFaded[3] = 0.0;
    _color[3]                  = 1.0;
  };
  void draw() {
    if (_color[3] <= 0.0)
      return;			// faded away
    _color[3] -= float(_fadeStep);	// fade

    const double x = glFromSecond(_t);
    const double xL = x - 0.08;
    const double xR = x + 0.08;
    if (xR < -0.01 || xL > 1.01)
      return;			// cull offscreen
    glBegin(GL_QUAD_STRIP);
      glColor4fv(_colorFaded);
	glVertex2d(xL, 0.0);
	glVertex2d(xL, 1.0);
      glColor4fv(_color);
	glVertex2d(x, 0.0);
	glVertex2d(x, 1.0);
      glColor4fv(_colorFaded);
	glVertex2d(xR, 0.0);
	glVertex2d(xR, 1.0);
    glEnd();
    glColor4fv(_color);
    glBegin(GL_LINE_STRIP);
      glVertex2d(x, 0.0);
      glVertex2d(x, 1.0);
    glEnd();
  };
private:
  double _fadeStep;
  double _t; // offset in seconds
  float _color[4];
  float _colorFaded[4];
};
Flash flashTag(10);
Flash flashLimits[2];

void limitBlink(int i)
{
  assert(i==0 || i==1);
  flashLimits[i].blink(tShowBound[i], 1.0f, 0.1f, 0.1f);
}
void zoomBlink(int i)
{
  assert(i==0 || i==1);
  flashLimits[i].blink(tShow[i], 0.8f, 0.4f, 0.1f);
}

void aimCrop()
{
  if (tAim[0] < tShowBound[0]) {
    tAim[0] = tShowBound[0];
    limitBlink(0);
  }
  if (tAim[1] > tShowBound[1]) {
    tAim[1] = tShowBound[1];
    limitBlink(1);
  }
}

bool fHeld = false;			// Left  mouse button is held down.
bool fHeldRight = false;		// Right mouse button is held down.
bool fDrag = false;			// Mouse was left-dragged, not merely left-clicked.
double xyMouse[2] = {0.5, 0.15};	// Mouse position in GL coords.
double xAim = 0.0, yAim = 0.0;		// Mouse position in world coords.  Setpoint for xyMouse.

void aim()
{
  double lowpass = 0.0345 * exp(67.0 * secsPerFrame);
  // Disable aim/show at slow frame rates, to avoid flicker.
  if (secsPerFrame > 1.0/30.0)
    lowpass = 1.0;
  if (lowpass > 1.0)
    lowpass = 1.0;
  double lowpass1 = 1.0 - lowpass;
  xyMouse[0] = xyMouse[0]*lowpass1 + xAim*lowpass;
  xyMouse[1] = xyMouse[1]*lowpass1 + yAim*lowpass;

  assert(tShowBound[0] < tShowBound[1]);
  assert(tAim[0] < tAim[1]);

  // If tAim is disjoint with tShowBound (!!), pan it back into range.
  if (tAim[0] >= tShowBound[1]) {
    tAim[0] -= tAim[1] - tShowBound[1];
    tAim[1] = tShowBound[1];
    limitBlink(1);
  }
  else if (tAim[1] <= tShowBound[0]) {
    tAim[1] += tShowBound[0] - tAim[0];
    tAim[0] = tShowBound[0];
    limitBlink(0);
  }
  assert(tAim[0] < tShowBound[1]);
  assert(tAim[1] > tShowBound[0]);

  aimCrop();

  // Forbid zoom in past 100 samples, tweaked for 5 msec undersampling.
  // (This may interact with the constraint on tShowBound,
  // but any oscillation should converge.)

  const double dtZoomMin = 100.0/SR * 3*80;
  const bool fZoominLimit = tAim[1] - tAim[0] < dtZoomMin;
  if (fZoominLimit) {
    const double tFixed = (tAim[0] + tAim[1]) / 2.0;
    tAim[0] = tFixed - dtZoomMin * 0.5001;
    tAim[1] = tFixed + dtZoomMin * 0.5001;
  }

  aimCrop();

  for (int i=0; i<2; ++i) {
    tShow[i] = tShow[i]*lowpass1 + tAim[i]*lowpass;
    if (fZoominLimit)
      zoomBlink(i);
  }
  tShowPrev[0] = tShow[0];
  tShowPrev[1] = tShow[1];

  // Copypaste.
  // Asymmetric.  Grow slowly, shrink quickly.
  // Slowly, but not so slow that it distracts while nothing else changes.
  // Aim should complete within about 0.15 seconds.
  lowpass = 0.0345 * exp(67.0 * secsPerFrame) * (scaleWavAim > scaleWav ? 5.0 : 20.0);
  // Disable aim/show at slow frame rates, to avoid flicker.
  if (secsPerFrame > 1.0/30.0)
    lowpass = 1.0;
  if (lowpass > 1.0)
    lowpass = 1.0;
  lowpass1 = 1.0 - lowpass;
  scaleWav = scaleWav*lowpass1 + scaleWavAim*lowpass;
}

// later, increase step for successive steps in same direction, resetting this after 0.5 seconds elapse.
void scrollwheel(double x, bool fIn, bool fFast)
{
  const double tFixed = secondFromGL(x);
  double zoom = fFast ? 1.14 : 1.19;	// fFast is actually reversed.  hold key down, vs mouse wheel.
  if (fIn)
    zoom = 1.0 / zoom;
  tAim[0] = tFixed + (tAim[0]-tFixed) * zoom;
  tAim[1] = tFixed + (tAim[1]-tFixed) * zoom;
}

int qsortCompare(const void* a, const void* b)
{
  const double _ = ((const double*)a)[2] - ((const double*)b)[2];
  return _<0.0 ? 1 : _>0.0 ? -1 : 0;
}

void drawTimeline()
{
  const int iMax = 61;
  static bool fFirst = true;
  static double pow2[iMax];
  int i;
  if (fFirst) {
    for (i=0; i<iMax; ++i) pow2[i] = pow(2.0, i-iMax/2);
  }

  const double spanMin = 1e-5;	// long streaks,  low frequency, zoomed in
  const double spanMax = 1.0;	// grainy,       high frequency, zoomed out
  double uua[iMax][3];		// Each entry is [u,u,alpha]. u's are uv texture coords.

  int iMic = iMax+1;
  int iMac = -1;
  for (i=0; i<iMax; ++i) {
    double* p = uua[i];
    p[0] = tShow[0] * pow2[i];	// left end
    p[1] = tShow[1] * pow2[i];	// right end
    const double span = p[1] - p[0];
    // Ignore entries whose span is out of range.
    if (span>=spanMin && span<=spanMax) {
      if (i<iMic) iMic=i;
      if (i>iMac) iMac=i;
#ifndef M_PI
#define M_PI (3.1415926535)
#endif
      p[2] = cos(geometriclerp(span, spanMin, spanMax) *2.0*M_PI ) /-2.0 +0.5; // opacity
    }
  }
  const int diFew = 4;
  if (iMic+diFew > iMac)
    return;			// Should never happen.

  // Sort by decreasing opacity.
  qsort(uua + iMic, iMac-iMic, 3*sizeof(double), qsortCompare);
  // Keep the first few (the most opaque).
  memmove(uua, uua+iMic, diFew*3*sizeof(double));
  // Normalize sum of opacities.
  double opacity = 0.0;
  for (i=0; i<diFew; ++i) opacity += uua[i][2];
  for (i=0; i<diFew; ++i) uua[i][2] /= opacity;
  // Avoid roundoff error when used as uv coords.
  for (i=0; i<diFew; ++i) {
    if (uua[i][0] < 1.0)
      continue;
    const double f = floor(uua[i][0]);
    uua[i][0] -= f;
    uua[i][1] -= f;
  }

  // Draw.
  glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texNoise);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glPushMatrix();
      const double YTick = 0.03;
      glScaled(1.0, YTick*2, 1.0);
      glBegin(GL_QUADS);
      for (i=0; i<diFew; ++i) {
	const double* p = uua[i];
	// todo: brighter (more contrast) when rest of screen has less contrast.
	glColor4d(0.05,0.4,0.4, p[2]);
	glTexCoord2d(p[0], 0.0); glVertex2f(0.0, 0.0);
	glTexCoord2d(p[0], 1.0); glVertex2f(0.0, 1.0);
	glTexCoord2d(p[1], 1.0); glVertex2f(1.0, 1.0);
	glTexCoord2d(p[1], 0.0); glVertex2f(1.0, 0.0);
      }
      glEnd();
    glPopMatrix();
  glDisable(GL_TEXTURE_2D);
}

void drawCursors()
{
  double rgx[2] = {0,0};
  if (!s2s.xPlayCursors(rgx))
    return;
  glColor3d(0.2, 1.0, 0.2); // two green cursors
  glBegin(GL_LINES);
    for (int i=0; i<2; ++i) {
      glVertex2d(rgx[i], 0.0);
      glVertex2d(rgx[i], 1.0);
    }
  glEnd();
}

bool vfQuit = false;

double xyPanPrev[2] = {0.0, 0.0};
inline double xFromMouse(double xM) { return       xM/pixelSize[0]; }
inline double yFromMouse(double yM) { return 1.0 - yM/pixelSize[1]; }

class Egg {
public:
  double secDelay;
  double secEnd;
  int iEgg;
  double ampl;
#ifdef DEBUG
  const char* dump() const {
    static char _[1000];
    snprintf(_, 1000, "%lf %lf %d %lf", secDelay, secEnd, iEgg, ampl);
    return _;
  }
#endif
};

vector<Egg> eggs;
vector<char*> eggNames;
vector<double> eggsFound;

int eggsFind(double t)
{
  for (unsigned i = 0; i < eggs.size(); ++i) {
    Egg& e = eggs[i];
    if (t < e.secDelay)
      return -1;
    if (t <= e.secEnd)
      return i;
  }
  return -1;
}

int numEggsFound()
{
  int c = 0;
  for (unsigned i = 0; i < eggsFound.size(); ++i)
    if (eggsFound[i] >= 0.0)
      ++c;
  return c;
}

const char* whenEggsFound()
{
  return "";
}

int itagShow = 0;
double rgtagShow[90000]; // buffer overflow

void drawTags()
{
  // copypaste from drawCursors.
  glColor3f(1.0, 1.0, 1.0);
  glBegin(GL_LINES);
  for (int i=0; i<itagShow; ++i) {
    const double s = rgtagShow[i];
    const double x = glFromSecond(s);
    if (x < -0.01 || x > 1.0)
      continue;
    glVertex2d(x, YTick*2);
    glVertex2d(x, 1.0);
  }
  glEnd();
}

void keyboard(unsigned char key, int x, int /*y*/)
{
  // Fraction of horizontal screen to pan, per frame.
  // Slow enough to let user react to what they see, when holding key down.
  // Fast enough to not be tedious, requiring multiple keystrokes when investigating in detail.
  const double panspeed = 0.25;

  // When playing, tags apply to the starting point (which might have been moved offscreen).
  double rgs[2] = {0,0};
  double sTag = s2s.sPlayCursors(rgs) ? rgs[0] : sMouseRuler;
  if (sTag < 0.0)
    sTag = 0.0;
  static double eggsUndo[3] = {-1,-1,-1}; // iEgg, eggsFound[iEgg], sTag

  switch(key) {
    case 3: // ctrl+C
      vfQuit = true;
      snooze(0.6); // let other threads notice vfQuit
      delete applog;
      exit(0);
    case 'q':
      {
	if (eggs.empty())
	  break;
	const int iEgg = int(eggsUndo[0]);
	if (iEgg >= 0) {
	  sTag = eggsUndo[2];
	  if (!onscreen(sTag)) {
	    break;
	  }
	  eggsFound[iEgg] = eggsUndo[1];
	  eggsUndo[0] = -1;
	  eggsUndo[1] = -1;
	  eggsUndo[2] = -1;
	  if (itagShow > 0)
	    --itagShow;
	}
	else if (itagShow > 0) {
	  // Flash at the line that will get erased.
	  sTag = rgtagShow[itagShow-- - 1];
	  if (!onscreen(sTag)) {
	    ++itagShow;
	    break;
	  }
	} else {
	  // Nothing left to undo.
	  sTag = -1.0;
	}
	flashTag.blink(sTag, 0.2f, 0.5f, 1.0f);
      }
      break;
    case 'e':
      {
	if (eggs.empty())
	  break;
	rgtagShow[itagShow++] = sTag;
	const int i = eggsFind(sTag);
	if (i >= 0) {
	  eggsUndo[0] = double(i);
	  eggsUndo[1] = eggsFound[i];
	  eggsUndo[2] = sTag;
	  eggsFound[i] = appnow();
	}
	flashTag.blink(sTag, 0.9f, 1.0f, 1.0f);
      }
      break;
    case ' ':
      // Snap offscreen cursor back onscreen,
      // because subjects twitch-game rather than constructing deep subplans.
      // Anyways, right now there's no way to re-find the cursor after it's scrolled offscreen.
      if (!onscreen(sMouseRuler))
	sMouseRuler = tShow[0];
      s2s.spacebar(sMouseRuler);
      break;
    case 'd':
      {
	// 1e-4 allows pan slightly outside tShowBound, for visual warning.
	const double dsec = min(dsecondFromGL(panspeed), tShowBound[1]-tAim[1] + 1e-4);
	if (dsec > 0.0) {
	  tAim[0] += dsec;
	  tAim[1] += dsec;
	}
      }
      break;
    case 'a':
      {
	const double dsec = min(dsecondFromGL(panspeed), tAim[0]-tShowBound[0] + 1e-4);
	if (dsec > 0.0) {
	  tAim[0] -= dsec;
	  tAim[1] -= dsec;
	}
      }
      break;
    case 'w':
      scrollwheel(xFromMouse(x), true, true);
      break;
    case 's':
      scrollwheel(xFromMouse(x), false, true);
      break;
    default:
      printf("Ignoring keystroke %d\n", key); // see www.bbdsoft.com/ascii.html
      break;
  }
}

bool vfFakeDrag = false;
void drag(int x, int y)
{
  static double xDrag = -1.0;
  if (xDrag < 0.0) {
    xDrag = x;
  }
  xDrag = x;
  // Mouse may not have actually moved, if called from button callback.

  if (fHeld) {
    xAim = xFromMouse(x);
    yAim = yFromMouse(y);
    const double xpanNew = xAim;
    const double dsec = dsecondFromGL(xyPanPrev[0] - xpanNew);
    tAim[0] += dsec;
    tAim[1] += dsec;
    xyPanPrev[0] = xpanNew;
    fDrag |= !vfFakeDrag; // distinguish left click from left drag
  }
  if (fHeldRight)
    sMouseRuler = secondFromGL(xFromMouse(x));
#ifdef UNUSED
  if (fHeld || fHeldRight)
    xyPanPrev[1] = yFromMouse(y);
#endif
  // If you're dextrous enough to drag while holding both buttons, feel free to do so.
}

void mouse(int button, int state, int x, int y)
{
  const int _ = 100000;
  switch (button*_+state) {
    case GLUT_WHEEL_UP*_+GLUT_DOWN:
      scrollwheel(xFromMouse(x), true, false);
      break;
    case GLUT_WHEEL_DOWN*_+GLUT_DOWN:
      scrollwheel(xFromMouse(x), false, false);
      break;
    case GLUT_LEFT_BUTTON*_+GLUT_DOWN:
      fHeld = true;
      fDrag = false;
      xyMouse[0] = xyPanPrev[0] = xFromMouse(x);
      xyMouse[1] = xyPanPrev[1] = yFromMouse(y);
      break;
    case GLUT_RIGHT_BUTTON*_+GLUT_DOWN:
      fHeldRight = true;
      fDrag = false;
      xyPanPrev[0] = xFromMouse(x);
      xyPanPrev[1] = yFromMouse(y);
      break;
    case GLUT_LEFT_BUTTON*_+GLUT_UP:
      if (!fDrag)
	sMouseRuler = secondFromGL(xFromMouse(x));
        // click not drag
      fHeld = false;
      fDrag = false;
      break;
    case GLUT_RIGHT_BUTTON*_+GLUT_UP:
      fHeldRight = false;
      fDrag = false;
      break;
  }
  vfFakeDrag = true; // Ugly global, because drag()'s signature can't change.
  drag(x,y);
  vfFakeDrag = false;
}

double ticked[1000][2];
int ctick = 0;
void tickreset() { ctick = 0; }
void tick(double x, const char* label)
{
  if (ctick >= 1000)
    return; // silently
  const double width2 = strlen(label) * dxChar / 2.0;
  ticked[ctick][0] = x - width2;
  ticked[ctick][1] = x + width2;
  ++ctick;
}

bool fTickIncluded(double x, const char* label)
{
  const double width2 = strlen(label) * dxChar / 2.0;
  for (int i=0; i<ctick; ++i) {
    const double& l = ticked[i][0];
    const double& r = ticked[i][1];
    const double xl = x - width2;
    const double xr = x + width2;
    // Does [xl,xr] intersect [l,r]?
    if ((l <= xl && xl <= r) || (xl <= l && l <= xr))
      return true;
  }
  return false;
}

void doTick(double x, const char* label)
{
  if (fTickIncluded(x, label))
    return; // covered by another tick
  tick(x, label);
  const double width2 = strlen(label) * dxChar / 2.0;
  glRasterPos2d(x-width2, YTick);
  putsGlut(label);
}

void drawTicks()
{
  const int cUnit = 7;
  const char* szTick[cUnit] = { "week", "day", "h", "10m", "m", "10s", "s" }; // ds cs ms us
  const double secTick[cUnit] = { 7 * 86400, 86400, 3600, 600, 60, 10, 1 };   // .1 .01 .001 1e-6
  // Sadly, no standard units fill the gaps between h and m, m and s, ms and us.
  const double gl0 = glFromSecond(0);

  const double dxMin = 3.0/65.0;
  tickreset();
  for (int i=0; i<cUnit; ++i) {
    const char* label = szTick[i];
    const double scale = secTick[i];
    if (scale > tShowBound[1] - tShowBound[0])
      continue;

    const double dx = glFromSecond(scale) - gl0;
    assert(dx > 0.0);
    if (dx < dxMin)
      continue;

    const double dxFade = dxMin * 1.8;
    glColor4d(1,1,0, 1 - (dx < dxFade ? (dxFade-dx) / (dxFade-dxMin) : 0));

    // 5.0/2, 3.0/2 left- and right-justify strlen "Start" and "End".
    double x = glFromSecond(tShowBound[0]) + (5.0/2 * dxChar);
    doTick(x, "Start");

    x = glFromSecond(tShowBound[1]) - (3.0/2 * dxChar);
    doTick(x, "End");

    const double xLast = glFromSecond( ceil(tShow[1]/scale) *scale);
    x                  = glFromSecond(floor(tShow[0]/scale) *scale);
    for (; x<xLast && x<1.001; x+=dx) {
      if (x >= -0.04)
	doTick(x, label);
    }
  }
}

void reshape(int w, int h)
{
  glViewport(0, 0, pixelSize[0]=w, pixelSize[1]=h);
  // Assume monospace.
  dxChar = glutBitmapWidth(font, 'a') / double(pixelSize[0]);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluOrtho2D(0,1, 0,1);
  glMatrixMode(GL_MODELVIEW);
}

void drawThumb()
{
  const double yTop = YTick * 0.9;
  const double dt = tShowBound[1] - tShowBound[0];
  double x0 = max(0.0, (tShow[0] - tShowBound[0]) / dt);
  assert(pixelSize[0] > 0);
  const double dxMin = 2.0/pixelSize[0];
  const double x1Min = x0 + dxMin;
  double x1 = max(x1Min, (tShow[1] - tShowBound[0]) / dt);
  if (x1 > 1.0) {
    // scoot left to be visible
    x0 -= x1 - 1.0;
    x1 = 1.0;
  }
  if (x0 < 0.0) {
    x0 = 0.0;
  }

  assert(0.0 <= x0);
  assert(x0 <= x1);
  assert(x1 <= 1.0);

  // fade out top edge, so thumb doesn't look manipulable.
  glBegin(GL_QUADS);
    // background
    glColor4d(0.0,1,0.0, 0.3);
    glVertex2d(1.0, 0.0);
    glVertex2d(0.0, 0.0);
    glColor4d(0.0,1,0.0, 0.0);
    glVertex2d(0.0, yTop);
    glVertex2d(1.0, yTop);
    // thumbnail indicating pan and zoom
    glColor4d(0,1,0,0.9);
    glVertex2d(x1, 0.0);
    glVertex2d(x0, 0.0);
    glColor4d(0,1,0,0.0);
    glVertex2d(x0, yTop);
    glVertex2d(x1, yTop);
  glEnd();
}

// Simpler and faster than a variadic function using va_list and va_start.
#define printf_safe(...) snprintf(sprintfbuf, sizeof(sprintfbuf), __VA_ARGS__)

void drawAll()
{
  const double t = appnow();
  const double dt = t - tFPSPrev;
  // dt==0 is possible on Windows, but not on Linux.
  if (dt > 0.0) {
    tFPSPrev = t;
    // moving average
    const int c = 30;
    static double secsPerFrameStore[c] = {0};
    secsPerFrame -= secsPerFrameStore[c-1] / c;
    memmove(secsPerFrameStore+1, secsPerFrameStore, (c-1)*sizeof(double));
    secsPerFrame += (secsPerFrameStore[0] = dt) / c;
  }

  glClear(GL_COLOR_BUFFER_BIT);
  drawFeatures();
  drawMouseRuler();
  drawWaveform();
  drawTimeline();
  drawTicks();
  drawThumb();
  flashTag.draw();
  flashLimits[0].draw();
  flashLimits[1].draw();
  drawCursors();
  drawTags();
  glutSwapBuffers();
  aim();
}

#ifdef _MSC_VER
inline float drand48() { return float(rand()) / float(RAND_MAX); } 
#endif

void makeTextureNoise()
{
  const int w = 2048; // Wider than widest window.
  const int h = 8;
  float a[w*h];
  for (int i=0; i<w*h; ++i) a[i] = drand48();
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_FLOAT, a);
}

// When audio data arrives in {vpsSamples,visSamples},
// send it to the soundcard.
#ifdef _MSC_VER
extern void rtaudioInit(), rtaudioTick(const short*, int), rtaudioPause(bool), rtaudioTerm();
extern int rtaudioBuf();
int isKicked = 0; // how many more samples producer needs right now
void kickProducer(int is) {
	//isKicked = is;
}

void samplereader(void*) {
  rtaudioInit();

  vlockAudio.lock();
    vnumSamples = 960;
    info("samplereader primed");
  vlockAudio.unlock();

  while (!vfQuit) {
    vlockAudio.lock();
	  const bool f = visSamples > 0;
	  if (f) {
		  rtaudioTick(vpsSamples, visSamples*2);
		  //isKicked -= visSamples;
		  //if (isKicked < 0) printf("whoa, producer, request more than that.\n");;;;
		  visSamples = 0;
		  vnumSamples = rtaudioBuf();
	  }
    vlockAudio.unlock();
    snooze(f ? 0.001 : 0.015);
	// In ALSA, 0.015 matched period_time=15000 usec.
	// Here in RtAudio, 256 samples at 16 kHz is 0.016 seconds.
	// Nope.  Alsa's snd_pcm_writei() is blocking.  But RtAudio no longer supports blocking writes.
	// So rtaudioTick() returns immediately, whereas alsaTick() returned only after 15 msec.
	// OLD: 0.015 is just too short: producer runs too fast.  0.025 is too long: producer runs too slow, dropouts.
  }
  rtaudioTerm();
}

#else
void* samplereader(void*) {
  extern void alsaInit(), alsaTick(const short*, ssize_t), alsaTerm();
  extern int alsaBuf();
  alsaInit();

  vlockAudio.lock();
    vnumSamples = alsaBuf();
    info("samplereader primed");
  vlockAudio.unlock();

  while (!vfQuit) {
    vlockAudio.lock();
      const bool f = visSamples>0;
      if (f) {
	// Send visSamples audio-samples to ALSA.
	const ssize_t cb = visSamples*2;
	alsaTick(vpsSamples, cb);
	visSamples = 0;
	// Underruns are ok.  So set vnumSamples instead of incrementing it.
	vnumSamples = alsaBuf();
      }
    vlockAudio.unlock();
    snooze(f ? 0.005 : 0.015);
  }

  alsaTerm();
  return NULL;
}
#endif

// If audio needs playing, report the next bufferful of samples
// (from main memory) as {vpsSamples,visSamples}.
#ifdef _MSC_VER
void samplewriter(void*)
#else
void* samplewriter(void*)
#endif
{
  while (!vfQuit) {
    while (!s2s.playing() && !vfQuit)
      snooze(0.03); // much longer than that of than samplereader?
    if (vfQuit)
      break;
    vlockAudio.lock();
      if (vnumSamples > 0) {
	s2s.emit(vnumSamples);
	vnumSamples = 0;
      }
    vlockAudio.unlock();
  }
#ifndef _MSC_VER
  return NULL;
#endif
}

const char* dirMarshal = ".timeliner_marshal";

void readEggs()
{
  const string eggfile(dirMarshal + string("/eggs"));
  FILE* pf = fopen(eggfile.c_str(), "r");
  if (!pf) {
    info("no file dirMarshal/eggs");
    return;
  }

  unsigned ceggNames = 0;
  if (1 != fscanf(pf, "%u", &ceggNames))
    quit("dirMarshal/eggs syntax error");
  if (ceggNames <= 0)
    quit("dirMarshal/eggs nonpositive number of eggnames");
  eggNames.reserve(ceggNames);

  unsigned i;
  for (i=0; i<ceggNames; ++i) {
    char* s = new char[200];
    if (1 != fscanf(pf, "%s", s))
      quit("dirMarshal/eggs syntax error");
    eggNames.push_back(s);
  }
  assert(eggNames.size() == ceggNames);

  unsigned ceggs = 0;
  if (1 != fscanf(pf, "%u", &ceggs))
    quit("dirMarshal/eggs syntax error");
  if (ceggs <= 0)
    quit("dirMarshal/eggs nonpositive number of eggs");
  eggs.reserve(ceggs);
  eggsFound.reserve(ceggs);

  for (i=0; i<ceggs; ++i) {
    Egg e;
    if (4 != fscanf(pf, "%lf %lf %d %lf", &e.secDelay, &e.secEnd, &e.iEgg, &e.ampl))
      quit("dirMarshal/eggs syntax error");
    eggs.push_back(e);
    eggsFound.push_back(-1.0);
  }
  assert(eggs.size() == ceggs);
}

int main(int argc, char** argv)
{
  testConverters();

#ifndef _MSC_VER
  // Nvidia: prevent image tearing, and as a side effect limit fps to 60.
  // Environment variable doesn't persist after app exists.
  // If this fails on one of two TwinView monitors,
  // (most noticeable during playback as the green cursor line moves).
  // drag Timeliner to the other monitor.
  // http://us.download.nvidia.com/XFree86/Linux-x86/180.22/README/chapter-11.html
  setenv("__GL_SYNC_TO_VBLANK", "1", 1);
#endif

  glutInit(&argc, argv);
  if (argc > 3)
    quit("Usage: timeliner_run dirMarshal [logfile]");
  if (argc >= 2)
    dirMarshal = argv[1];
  if (argc == 3)
    logfilename = argv[2];
  applog = new Logger(logfilename);

  readEggs();

  const string wav(dirMarshal + string("/mixed.wav"));
  // www.mega-nerd.com/libsndfile/api.html
  SF_INFO sfinfo;
  sfinfo.format = 0;
  SNDFILE* pf = sf_open(wav.c_str(), SFM_READ, &sfinfo);
  if (!pf) {
#ifdef _MSC_VER
	char buf[MAX_PATH];
	(void)GetFullPathNameA(wav.c_str(), MAX_PATH, buf, NULL);
    quit(std::string("no wav file ") + std::string(buf));
#else
	quit("no wav file");
#endif
  }
  //printf("%ld frames, %d SR, %d channels, %x format, %d sections, %d seekable\n",
  //  long(sfinfo.frames), sfinfo.samplerate, sfinfo.channels, sfinfo.format, sfinfo.sections, sfinfo.seekable);
  if (sfinfo.samplerate != SR || sfinfo.channels != 1 || sfinfo.format != (SF_FORMAT_WAV|SF_FORMAT_PCM_16))
    warn("wavfile has unexpected format");
  wavcsamp = long(sfinfo.frames);
  wavS16 = new short[wavcsamp];
  long cs = long(sf_read_short(pf, wavS16, wavcsamp));
  if (cs != wavcsamp)
    quit("failed to read wav");
  if (0 != sf_close(pf))
    warn("failed to close wav");
  const double msec_resolution = 0.3;
  const double undersample = max(1.0, msec_resolution * 1e-3 * SR);
  cacheWav = new CHello(wavS16, wavcsamp, float(SR), int(undersample), 1);
  if (!cacheWav)
    quit("failed to cache wav");

  tAim[0] = tShow[0] = tShowBound[0] = 0.0;
  tAim[1] = tShow[1] = tShowBound[1] = wavcsamp / double(SR);

  glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
  glutInitWindowSize(pixelSize[0], pixelSize[1]);
  glutCreateWindow("FODAVA Timeliner");
  glutKeyboardFunc(keyboard);
  glutMouseFunc(mouse);
  glutMotionFunc(drag);
  glutPassiveMotionFunc(drag);
  glutReshapeFunc(reshape);
  glutDisplayFunc(drawAll);
  glutIdleFunc(drawAll);

  glClearColor(0,0,0,0);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_BLEND);
  glDisable(GL_LIGHTING);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  info("reading marshaled htk features");
  // Ugly and brute-force.  Just let filenames fail if they don't exist.
  int i;
  char filename[10] = "features9";
  for (i=9; i>=0; --i) {
    filename[8] = char('0' + i);
    Feature* f = new Feature(-1, filename, dirMarshal);
    if (f->fValid())
      features.push_back(f);
  }
  if (features.empty()) {
    warn("No HTK features");
  }
  else {
    info("cached HTK features");
  }

  glGenTextures(1, &texNoise);
  prepTexture(texNoise);
  makeTextureNoise();
#ifdef _MSC_VER
  unsigned long idDummy;
  idDummy = _beginthread(samplereader, 0, NULL);
  idDummy = _beginthread(samplewriter, 0, NULL);
  // todo: if (_threadID == (unsigned long)(-1L)) ...
#else
  pthread_t idDummy;
  pthread_create(&idDummy, NULL, &samplereader, NULL);
  pthread_create(&idDummy, NULL, &samplewriter, NULL);
#endif

  printf("\nHow to use:\n\
	Pan or zoom       : w a s d\n\
			  : left click or drag\n\
			  : scrollwheel\n\
\n\
	Set playback cursor: right click or drag\n\
	Play              : space\n\
\n\
	Tag an anomaly    : e\n\
	Undo last tag     : q\n\
\n\
	Quit              : ctrl+C\n\n");

  glutMainLoop();
  return 0;
}
