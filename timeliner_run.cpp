#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS // for strcpy, getenv, _snprintf, fopen, fscanf
#endif

#undef DEBUG

#include "timeliner_diagnostics.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>

#ifdef _MSC_VER
#define NOMINMAX // Don't #define min and max in windows.h, so std::min works.
#include <windows.h>
#include <time.h>
#include <iostream>
#include <sstream>
#include <functional>
#else
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <linux/soundcard.h>
#include <unistd.h>
#endif

// Linux:   apt-get install libsndfile1-dev
// Windows: www.mega-nerd.com/libsndfile/ libsndfile-1.0.25-w64-setup.exe
#include <sndfile.h>
#include <vector>

// Linux:   apt-get install libglew-dev
// Windows: http://glew.sourceforge.net/install.html
#include <GL/glew.h> // before gl.h
#include <GL/glut.h>

#if !defined(GLUT_WHEEL_UP)
#define GLUT_WHEEL_UP   (3)
#define GLUT_WHEEL_DOWN (4)
#endif

#include "timeliner_cache.h"
#include "timeliner_util.h"

#ifdef _MSC_VER
void snooze(double sec) { Sleep(DWORD(sec * 1e3)); }
#else
void snooze(double sec) { (void)usleep(sec * 1e6); }
#endif

#ifdef _MSC_VER
#include <windows.h>
#include <time.h>
#include <iostream>
 
// Difference between Unix epoch Jan 1 1970 and Windows epoch Jan 1 1601.
#if defined(_MSC_VER) || defined(_MSC_EXTENSIONS)
  #define DELTA_EPOCH_IN_MICROSECS  (11644473600000000Ui64)
#else
  #define DELTA_EPOCH_IN_MICROSECS  (11644473600000000ULL)
#endif
 
struct timezone
{
  int tz_minuteswest; // minutes W of Greenwich
  int tz_dsttime;     // type of dst correction
};
int gettimeofday(struct timeval *tv, struct timezone *tz)
{
  if (NULL != tv)
  {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const unsigned __int64 usecNow =
      (((unsigned __int64)(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10 - DELTA_EPOCH_IN_MICROSECS;
    tv->tv_sec  = (long)(usecNow / 1000000UL);
    tv->tv_usec = (long)(usecNow % 1000000UL);
  }
#ifdef unused_by_timeliner
  if (NULL != tz)
  {
    static bool fFirst = true;
    if (fFirst)
    {
      fFirst = false;
      _tzset();
    }
    long tmp;
    _get_timezone(&tmp);
    tz->tz_minuteswest = tmp / 60;
    _get_daylight(&tz->tz_dsttime);
  }
#endif
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

double tFPSPrev = appnow();
double secsPerFrame = 1/60.0;

const double yShowBound[2] = { 0.0, 1.0 };
double yShowPrev[2] = {-2.0, -1.0}; // deliberately bogus
double yZoom = 1.0;
double yShow[2] = {yShowBound[0], yShowBound[1]}; // displayed interval of y, updated each frame
double yAim[2] = {yShow[0], yShow[1]}; // setpoint for yShow, updated more slowly

double tShowBound[2] = {0.0, 90000.0};
double tShowPrev[2] = {-2.0, -1.0};	// deliberately bogus
double tShow[2] = {tShowBound[0], tShowBound[1]};	// displayed interval of time, updated each frame
double tAim[2] = {tShow[0], tShow[1]};	// setpoint for tShow, updated more slowly

inline double dsecondFromGL(const double dx) { return dx * (tShow[1] - tShow[0]); }
inline double secondFromGL(const double x) { return dsecondFromGL(x) + tShow[0]; }
inline double glFromSecond(const double s) { return (s - tShow[0]) / (tShow[1] - tShow[0]); }

// Like dsecondFromGL().
inline double dyFromGL(const double dy) { return dy * (yShow[1] - yShow[0]); }

// Like dyFromGL, but considering pan as well as zoom.  yFromGL : dyFromGL :: secondFromGL : dsecondFromGL.
// Inverse of drawAll's scale and translate.
inline double yFromGL(const double y) {
  return dyFromGL(y) + yShow[0];
}

// Ignores yTimeline translate-and-scale.
inline double glFromY(const double y) { return  (y - yShow[0]) / (yShow[1] - yShow[0]); }

void testConverters()
{
  for (double i = -30.0; i < 50.0; ++i) {
    assert(abs(i - glFromSecond(secondFromGL(i))) < 1e-10);
    assert(abs(i - secondFromGL(glFromSecond(i))) < 1e-10);
    assert(abs(i - glFromY(yFromGL(i))) < 1e-10);
    assert(abs(i - yFromGL(glFromY(i))) < 1e-10);
  }
}

long wavcsamp = -1L; // int wraps around too soon
short* wavS16 = NULL;

bool onscreen(const double sec) { return sec >= tShow[0] && sec <= tShow[1]; }

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
public:

#ifdef _MSC_VER
  arLock() : _fOwned(true) {
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
      std::cerr << "arLock warning: CreateMutex failed (already exists).\n";
      return;
    }
    if (e == ERROR_ACCESS_DENIED) {
      std::cerr << "arLock warning: CreateMutex failed (access denied); backing off.\n";
LBackoff:
      // _mutex = OpenMutex(SYNCHRONIZE, FALSE, name);
      // Fall back to a mutex of scope "app" not "the entire PC".
      _mutex = CreateMutex(NULL, FALSE, NULL);
      if (!_mutex) {
        std::cerr << "arLock warning: failed to create mutex.\n";
      }
    }
    else if (e == ERROR_PATH_NOT_FOUND) {
      std::cerr << "arLock warning: CreateMutex failed (backslash?); backing off.\n";
      goto LBackoff;
    }
    else {
      std::cerr << "arLock warning: CreateMutex failed; backing off.\n";
      goto LBackoff;
    }
  }

  bool valid() const { return _mutex != NULL; }

  ~arLock() {
    if (_fOwned && _mutex) {
      (void)ReleaseMutex(_mutex); // paranoid
      CloseHandle(_mutex);
    }
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

  arLock() {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);
    pthread_mutex_init(&_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
  }
  ~arLock() {
    pthread_mutex_destroy(&_mutex);
  }

  void lock() {
    // A relative time (a duration) requires pthread_mutex_timedlock_np
    // or pthread_mutex_reltimedlock_np, but Ubuntu doesn't define those.
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += 1; // Overkill.  Slower than that really shouldn't happen with only 3 threads.
    const int r = pthread_mutex_timedlock(&_mutex, &t);
    if (r != 0) {
      switch(r) {
	case EINVAL: fprintf(stderr, "EINVAL\n"); break;
	case ETIMEDOUT: fprintf(stderr, "ETIMEDOUT\n"); break;
	case EAGAIN: fprintf(stderr, "EAGAIN\n"); break;
	case EDEADLK: fprintf(stderr, "EDEADLK\n"); break;
	default: fprintf(stderr, "unknown\n"); break;
      }
      quit("internal mutex error");
    }
  }
  void unlock() {
    pthread_mutex_unlock(&_mutex);
  }

#endif

};

int SR = -1;

// Implicitly unlocks when out of scope.
class arGuard {
  arLock& _l;
public:
  arGuard(arLock& l): _l(l) { _l.lock(); }
  ~arGuard() { _l.unlock(); }
};

arLock vlockAudio; // guards next three
int vnumSamples = 0; // high-low-water-mark between samplewriter() and samplereader()
const short* vpsSamples = NULL; // vpsSamples and visSamples are set by samplewriter() via emit(), and cleared by samplereader().
int visSamples = 0;

class S2Splay {
public:
  S2Splay() : _sPlay(-1.0), _tPlay(appnow()), _fPlaying(false), _sampBgn(10000) {}
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
  void soundplayNolock(const double t) {
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
int pixelSize[2] = {1000,1000};

double dxChar = 0.01;
#define font GLUT_BITMAP_9_BY_15

double sq(double _) { return _*_; }

// As z from 0 to 1, lerp from min to max.
double geometriclerp(double z, double min, double max)
{
  if (z<0.0 || min<0.0 || max<0.0)
    return -1.0;   // handle error arbitrarily
  z   = sqrt(z  );
  min = sqrt(min);
  max = sqrt(max);
  return sq((z-min) / (max-min));
}

double lerp(const double z, const double min, const double max)
{
  return z*max + (1.0-z)*min;
}

// To set color: *before* glRasterPos2d, call glColor (with lighting disabled).
char sprintfbuf[10000];
void putsGlut(const char* pch = sprintfbuf)
{
  while (*pch) glutBitmapCharacter(font, *pch++);
}

void prepTexture(const GLuint t)
{
  glBindTexture(GL_TEXTURE_2D, t);
  assert(glIsTexture(t) == GL_TRUE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

void prepTextureMipmap(const GLuint t)
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
  #define GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX 0x9047
  GLint total_mem_kb = 0;
  glGetIntegerv(GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &total_mem_kb);
#endif
  #define GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
  GLint cur_avail_mem_kb = 0;
  glGetIntegerv(GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &cur_avail_mem_kb);
  return cur_avail_mem_kb/1000.0F;

  // A feature of width 65536 (almost 11 minutes at 100 samples/sec)
  // uses ~145 MB texture RAM.
  // That's 8 chunks of width 8192.
  // 1 chunk is 8192x1 texels * 2 for mipmap * 3 for GL_RGB * 62 vectorsize = 3047424 B = 2.9MB;
  // 8 chunks is 23 MB.  But 145MB is used?!  (RGBA not just RGB?)
}

#ifdef _MSC_VER
inline float drand48() { return float(rand()) / float(RAND_MAX); }
#endif

class Feature {

  enum { vecLim = CQuartet_widthMax+1 }; // from timeliner_cache.h
  static int mb;
  enum { mbUnknown, mbZero, mbPositive };

  class Slartibartfast {
  public:
    GLuint tex[vecLim];
  };

  double testpattern(const int t) const { // return 0.0 to 1.0
    return sin(t / 6.28 / 7) * 0.5 + 0.5;
  //return (t%30)/30.0;
  }

  template <class T> const T& min3(const T& a, const T& b, const T& c) { return std::min(a, std::min(b, c)); }
  template <class T> const T& max3(const T& a, const T& b, const T& c) { return std::max(a, std::max(b, c)); }
  template <class T> const T avg(const T& a, const T& b) { return (a+b)*0.5; }

public:
  int cchunk;
  std::vector<Slartibartfast> rgTex;

  Feature(int /*iColormap*/, const std::string& filename, const std::string& dirname): m_fValid(false) {
    if (mb == mbUnknown) {
      mb = gpuMBavailable() > 0.0f ? mbPositive : mbZero;
      if (!hasGraphicsRAM())
	warn("Found no dedicated graphics RAM.  Might run slowly.");
    }
    const Mmap marshaled_file(dirname + "/" + filename);
    if (!marshaled_file.valid())
      return;
    binaryload(marshaled_file.pch(), marshaled_file.cch()); // stuff many member variables
    makeMipmaps();
    m_fValid = true;
    // ~Mmap closes file
  };

  bool hasGraphicsRAM() const { return mb == mbPositive; }

  void binaryload(const char* pch, long cch) {
    assert(4 == sizeof(float));
    strcpy(m_name, pch);				pch += strlen(m_name);
    m_iColormap = int(*(float*)pch);		pch += sizeof(float);
    m_period = *(float*)pch;			pch += sizeof(float);
    const int slices = int(*(float*)pch);	pch += sizeof(float);
    m_vectorsize = int(*(float*)pch);		pch += sizeof(float);
    m_pz = (const float*)pch; // m_cz floats.  Not doubles.
    m_cz = (cch - long(strlen(m_name) + 4*sizeof(float))) / 4;
#ifndef NDEBUG
    printf("debugging feature: name %s, colormap %d, period %f, slices %d, width %d, cz %lu.\n", m_name, m_iColormap, m_period, slices, m_vectorsize, m_cz);
#endif
    if (m_iColormap<0 || m_period<=0.0 || slices<=0 || m_vectorsize<=0) {
      quit("binaryload: feature '" + std::string(m_name) + "' has corrupt data");
    }
    assert(slices*m_vectorsize == m_cz);
#ifdef NDEBUG
    _unused(slices);
#else

#ifndef _MSC_VER
	// VS2013 only got std::isnormal and std::fpclassify in July 2013:
	// http://blogs.msdn.com/b/vcblog/archive/2013/07/19/c99-library-support-in-visual-studio-2013.aspx
	for (int i=0; i<m_cz; ++i) {
    if (!std::isnormal(m_pz[i]) && m_pz[i] != 0.0) {
	printf("binaryload: feature's float %d of %ld is bogus: class %d, value %f\n", i, m_cz, std::fpclassify(m_pz[i]), m_pz[i]);
	quit("");
      }
    }
#endif
#endif
  };

  bool fValid() const { return m_fValid; }
  int vectorsize() const { return m_vectorsize; }
  int samples() const { return m_cz / m_vectorsize; }
  const char* name() const { return m_name; }

  void makeMipmaps() {
    // Adaptive subsample is too tricky, until I can better predict GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX.
    // (Adapt the prediction itself??  Allocate a few textures of various sizes, and measure reported GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX.  But implement this only after getting 2 or 3 different PCs to test it on.)
    // Subsampling to coarser than 100 Hz would be pretty limiting.
    const char* pch = getenv("timeliner_zoom");
    unsigned subsample = pch ? atoi(pch) : 1;
    if (subsample < 1)
      subsample = 1;
    if (subsample > 1)
      printf("Subsampling %dx from environment variable timeliner_zoom.\n", subsample);

    const int csample = samples();

    // Smallest power of two that exceeds features' # of samples.
    unsigned width = 1;
    while (width < csample/subsample)
      width *= 2;
    //printf("feature has %d samples, for tex-chunks' width %d.\n", csample, width);

    // Minimize cchunk to conserve RAM and increase FPS.
    {
      GLint widthLim; // often 2048..8192 (zx81 has 8192), rarely 16384, never greater.
      glGetIntegerv(GL_MAX_TEXTURE_SIZE, &widthLim);
      assert(widthLim >= 0); // because width is unsigned
      if (width > unsigned(widthLim)) assert(width%widthLim==0);	// everything is a power of two
      cchunk = width<unsigned(widthLim) ? 1 : width/widthLim;
      //printf("width = %d, cchunk = %d, widthLim = %d\n", width, cchunk, widthLim);
      if (width >= unsigned(widthLim))
	assert(GLint(width/cchunk) == widthLim);
    }

    rgTex.resize(cchunk);
    for (int ichunk=0; ichunk<cchunk; ++ichunk) {
      glGenTextures(m_vectorsize, rgTex[ichunk].tex);
      for (int j=0; j < m_vectorsize; ++j)
	prepTextureMipmap(rgTex[ichunk].tex[j]);
    }

    const CHello cacheHTK(m_pz, m_cz, 1.0f/m_period, subsample, m_vectorsize);
    glEnable(GL_TEXTURE_1D);
    const float mb0 = hasGraphicsRAM() ? gpuMBavailable() : 0.0f;
    // www.opengl.org/archives/resources/features/KilgardTechniques/oglpitfall/ #5 Not Setting All Mipmap Levels.
    for (unsigned level=0; (width/cchunk)>>level >= 1; ++level) {
      //printf("  computing feature's mipmap level %d.\n", level);
      makeTextureMipmap(cacheHTK, level, width >> level);
    }

    if (hasGraphicsRAM()) {
      const float mb1 = gpuMBavailable();
      printf("Feature used %.1f graphics MB;  %.0f MB remaining.\n", mb0-mb1, mb1);
      if (mb1 < 50.0) {
#ifdef _MSC_VER
	warn("Running out of graphics RAM.  Try increasing the environment variable timeliner_zoom to 15 or so.");
#else
	// Less than 50 MB free may hang X.  Mouse responsive, Xorg 100% cpu, network up, console frozen.
	// Or, the next request may "go negative", mb0-mb1<0.
	warn("Running out of graphics RAM.  Try export timeliner_zoom=15.");
#endif
      }
    }
  }

  const void makeTextureMipmap(const CHello& cacheHTK, const int mipmaplevel, int width) const {
    assert(vectorsize() <= vecLim);
    assert(width % cchunk == 0);
    width /= cchunk;

    //if (mipmaplevel<3) printf("    Computing %d mipmaps of width %d.\n", cchunk, width);
    //printf("vectorsize %d\n", vectorsize());
    unsigned char* bufByte = new unsigned char[vectorsize()*width];
    for (int ichunk=0; ichunk<cchunk; ++ichunk) {
      const double chunkL = ichunk     / double(cchunk); // e.g., 5/8
      const double chunkR = (ichunk+1) / double(cchunk); // e.g., 6/8
      cacheHTK.getbatchByte(bufByte,
	  lerp(chunkL, tShowBound[0], tShowBound[1]),
	  lerp(chunkR, tShowBound[0], tShowBound[1]),
	  vectorsize(), width, m_iColormap);
      for (int j=0; j<vectorsize(); ++j) {
	assert(glIsTexture(rgTex[ichunk].tex[j]) == GL_TRUE);
	glBindTexture(GL_TEXTURE_1D, rgTex[ichunk].tex[j]);
	glTexImage1D(GL_TEXTURE_1D, mipmaplevel, GL_INTENSITY8, width, 0, GL_RED, GL_UNSIGNED_BYTE, bufByte + width*j);
      }
    }
    delete [] bufByte;
  };

private:
  bool m_fValid;
  int m_iColormap;
  float m_period;	// seconds per sample
  int m_vectorsize;	// e.g., how many frequency bins in a spectrogram
  const float* m_pz;	// m_pz[0..m_cz] is the (vectors of) raw data
  long m_cz;
  char m_name[1000];
};
int Feature::mb = mbUnknown;
std::vector<Feature*> features;

GLuint myPrg = 0;
void shaderUse(bool f)
{
  glUseProgram(f ? myPrg : 0);
}
void shaderInit()
{
  glewInit();
  assert(glewIsSupported("GL_VERSION_2_0"));
  myPrg = glCreateProgram();
  assert(myPrg > 0);
  const GLuint myVS = glCreateShader(GL_VERTEX_SHADER);
  const GLuint myFS = glCreateShader(GL_FRAGMENT_SHADER);
  // For GLSL 1.30+, should pedantically define my own AttrMultiTexCoord0 instead of deprecated gl_MultiTexCoord0.
  const GLchar* prgV = "varying float u; void main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; u = gl_MultiTexCoord0.s; }";
  const GLchar* prgF = "varying float u; uniform sampler1D heatmap; uniform float palette[3*128]; \n void main() {\n\
    float i = texture1D(heatmap, u).r; // 0 to 1\n\
    int j = int(i*127.0) * 3; // 0 to 127*3, by 3's\n\
    // gl_FragColor = vec4(i,1.0-i,1.0-i,1.0);\n\
    gl_FragColor = vec4(palette[j],palette[j+1],palette[j+2],1.0);\n\
}";
  glShaderSource(myVS, 1, &prgV, NULL);
  glShaderSource(myFS, 1, &prgF, NULL);

  int r = 0;
  //int cch=0, cch2=0; char sz[10000] = "";

  glCompileShader(myVS); glGetShaderiv(myVS, GL_COMPILE_STATUS, &r); assert(r != GL_FALSE);
  //glGetShaderiv(myFS, GL_INFO_LOG_LENGTH, &cch);
  //glGetShaderInfoLog(myFS, cch, &cch2, sz);
  //if (cch2>0) printf("vert compile: %s\n", sz);

  glCompileShader(myFS); glGetShaderiv(myFS, GL_COMPILE_STATUS, &r); assert(r != GL_FALSE);
  //cch = cch2 = 0;
  //glGetShaderiv(myFS, GL_INFO_LOG_LENGTH, &cch);
  //glGetShaderInfoLog(myFS, cch, &cch2, sz);
  //if (cch2>0) printf("frag compile: %s\n", sz);

  glAttachShader(myPrg, myVS);
  glAttachShader(myPrg, myFS);

  glLinkProgram(myPrg); glGetProgramiv(myPrg, GL_LINK_STATUS,   &r); assert(r != GL_FALSE);
  //cch = cch2 = 0;
  //glGetProgramiv(myPrg, GL_INFO_LOG_LENGTH, &cch);
  //glGetProgramInfoLog(myPrg, cch, &cch2, sz);
  //if (cch2>0) printf("shader link: %d, %s\n", r, sz);

  // 128 works, but 256 fails to glLinkProgram (and segfaults in glGetProgramInfoLog).
  // 3000-ish bytes may be the maximum for a uniform array.
  //
  // Alternatives: UBO; texelFetch() a buffer texture (TBO), SSBO.
  // http://stackoverflow.com/questions/7954927/glsl-passing-a-list-of-values-to-fragment-shader
  // http://rastergrid.com/blog/2010/01/uniform-buffers-vs-texture-buffers/
  GLfloat bufPalette[3*128];
  for (int i=0; i<128; ++i) {
    const float z = float(sq(i/127.0f));
    // rgb, // a
    bufPalette[3*i+0] = z * 0.9f;
    bufPalette[3*i+1] = z * 1.0f;
    bufPalette[3*i+2] = z * 0.4f;
//  bufPalette[4*i+3] = 1.0;
  }

  shaderUse(true); // before calling any glUniform()s, so they know which program to refer to.
  assert(      glGetUniformLocation(myPrg, "palette") >= 0);
  glUniform1fv(glGetUniformLocation(myPrg, "palette"), 3*128, bufPalette);

  glActiveTexture(GL_TEXTURE0); // use texture unit 0
  assert(     glGetUniformLocation(myPrg, "heatmap") >= 0);
  glUniform1i(glGetUniformLocation(myPrg, "heatmap"), 0); // Bind sampler to texture unit 0.  www.opengl.org/wiki/Texture#Texture_image_units
}

// Top of timeline, measured from bottom of window (y==0) to top of window (y==1).
// todo: in resize(), keep this a constant # of pixels, e.g.  yTimeline = 20 / pixelSize[1];
const double yTimeline = 0.03;

#undef WAVEDRAW
#ifdef WAVEDRAW
// Measured from top of timeline (y==0) to top of window (y==1).
// Within the transformation that uses yTimeline.
// (Could adapt to # of channels and # of features?  Or will interleaving replace this?)
const double yBetweenWaveformAndFeatures = 0.2;
#else
const double yBetweenWaveformAndFeatures = 0.0;
#endif

void drawFeatures()
{
  if (features.empty())
    return;

  std::vector<Feature*>::const_iterator f;

  // Compute y's: amortize among vectorsizes.  Sqrt gives "thin" features more space.
  double rgdy[100]; //hardcoded;;  instead, grow a vector.
  assert(features.size() < 100);
  int i=0;
  double sum = 0.0;
  for (f=features.begin(); f!=features.end(); ++f) {
    // Waveform-features have vectorsize = 50, so hope for about 50 vertical pixels.
    // todo: adapt to pixelSize[1]
    const double dy = !strcmp((*f)->name(), "waveform-as-feature") ? 1.5 : sqrt(double((*f)->vectorsize()));
    sum += rgdy[i++] = dy;
    assert(i < 100); //hardcoded;;
  }
  // rgdy[0 .. i-1] are heights.
  double rgy[100]; //hardcoded;;
  rgy[0] = yBetweenWaveformAndFeatures;
  rgy[i] = 1.0;
  {
    const double rescale = sum / (rgy[i] - rgy[0]);
    for (int j=1; j<i; ++j)
      rgy[j] = rgy[j-1] + rgdy[j-1] / rescale;
    // rgy [0 .. i] are boundaries between features.
  }
  shaderUse(true);

  for (f=features.begin(),i=0; f!=features.end(); ++f,++i) {
    const double* p = rgy + i;
      glDisable(GL_TEXTURE_2D);
      glEnable(GL_TEXTURE_1D);
      glColor4d(0.9,1.0,0.4, 1.0);
      const int jMax = (*f)->vectorsize();
      for (int ichunk=0; ichunk < (*f)->cchunk; ++ichunk) {
	for (int j=0; j<jMax; ++j) {
	  const double chunkL =  ichunk    / double((*f)->cchunk); // e.g., 5/8
	  const double chunkR = (ichunk+1) / double((*f)->cchunk); // e.g., 6/8
	  const double tBoundL = lerp(chunkL, tShowBound[0], tShowBound[1]);
	  const double tBoundR = lerp(chunkR, tShowBound[0], tShowBound[1]);
	  const double xL = (tBoundL - tShow[0]) / (tShow[1] - tShow[0]);
	  const double xR = (tBoundR - tShow[0]) / (tShow[1] - tShow[0]);
	  if (xR < 0.0 || 1.0 < xL)
	    continue; // offscreen
	  assert(glIsTexture((*f)->rgTex[ichunk].tex[j]) == GL_TRUE);
	  glBindTexture(GL_TEXTURE_1D, (*f)->rgTex[ichunk].tex[j]);
	  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	  const double yMin = lerp(double(j  )/jMax, p[0], p[1]);
	  const double yMax = lerp(double(j+1)/jMax, p[0], p[1]);
	  assert(p[0]<=yMin && yMin<yMax && yMax<=p[1]);
	  glBegin(GL_QUADS);
	    glTexCoord1d(0.0); glVertex2d(xL, yMin); glVertex2d(xL, yMax);
	    glTexCoord1d(1.0); glVertex2d(xR, yMax); glVertex2d(xR, yMin);
	  glEnd();
	}
      }
      glDisable(GL_TEXTURE_1D);
    glColor4f(1,1,0,1);
    glRasterPos2d(0.01, p[0] + 0.005);
    putsGlut((*f)->name());
  }
}

unsigned int channels = 0; // == wavedrawers.size()

#ifdef WAVEDRAW

// Convert 0..32768 to what drawWaveformScaled() will scale the waveform by.  (bug: for actual pixels, consider yTimeline too? *=(1-yTimeline) ?)
double scaleWavFromSampmax(const double s)
{
  assert(0.0 <= s && s <= 32768.0);
  const double sMin = 25.0; // Threshold == 10 * log10(sMin^2 / 32768^2) == about -62 dB
  return 0.99 / std::max(s, sMin);
}
const double scaleWavDefault = scaleWavFromSampmax(32768.0);

class WaveDraw {
public:
  double scaleWav;
  double scaleWavAim;
  const CHello* cacheWav;
  WaveDraw(const CHello* p) : scaleWav(scaleWavDefault), scaleWavAim(scaleWavDefault), cacheWav(p)
    { if (!p) quit("failed to cache wav"); }
  void update()
    {
    // Asymmetric.  Grow slowly, shrink quickly.
    // Slowly, but not so slow that it distracts while nothing else changes.
    // Aim should complete within about 0.15 seconds.
    //
    // Copypaste.
    double lowpass = 0.0345 * exp(67.0 * secsPerFrame) * (scaleWavAim > scaleWav ? 5.0 : 20.0);
    // Disable aim/show at slow frame rates, to avoid flicker.
    if (secsPerFrame > 1.0/30.0)
      lowpass = 1.0;
    if (lowpass > 1.0)
      lowpass = 1.0;
    const double lowpass1 = 1.0 - lowpass;
    scaleWav = scaleWav*lowpass1 + scaleWavAim*lowpass;
    }
};

std::vector<WaveDraw> wavedrawers;

void drawWaveformScaled(const float* minmaxes, const double yScale) {
  // As yScale from default 331000. to zoomedin 5000.0,
  // color from brightgreen 0.1,1.0,0.2 to cyan 0.0,0.25,0.3,
  // roughly constant #pixels times brightness.
  double ramp = yScale==scaleWavDefault ? 1.0 :
    (0.85/yScale - 5000.0) / (33100.0 - 5000.0) / 11.0; // 1.0 downto 0.009
  ramp = std::max(ramp, 0.44); // Not too dark.
  glColor4d(lerp(ramp,0.07,0.1), geometriclerp(ramp,0.15,1.0), lerp(ramp,0.06,0.15), 1.0);
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

void drawWaveform()
{
  shaderUse(false);
  // 0 < x < 1
  // y above timeline, and rescale audio values from +-32768.

  // Adaptively scale (vertically zoom).
  // Convert x-pixel extent to [t,t+dt] from [t-dt/2, t+dt/2].  Edge-centered not body-centered, sort of.
  const double hack = (tShow[1] - tShow[0])*0.5 * (pixelSize[0] / (pixelSize[0]-1));

  const double YWavMax = features.empty() ? 1.0 : yBetweenWaveformAndFeatures;
  const double heightPerChannel = (YWavMax)/channels;
  const double yTweak = (YWavMax/2) /channels;
  assert(channels == wavedrawers.size());
  unsigned i=0;
  for (std::vector<WaveDraw>::iterator it = wavedrawers.begin(); it != wavedrawers.end(); ++it,++i) {
    float dst[2];
    it->cacheWav->getbatch(dst, tShow[0]+hack, tShow[1]+hack, 1, 1.0/(scaleWavDefault * pixelSize[1]));
    float& xmin = dst[0];
    float& xmax = dst[1];
    it->scaleWavAim = scaleWavFromSampmax(std::max(abs(xmin), abs(xmax)));

    glPushMatrix();
      // The Y-extent of a monophonic waveform is 0 .. yBetweenWaveformAndFeatures.
      // 0..1 height is then yBetweenWaveformAndFeatures/2.
      // 0..1 height is then YWavMax/2.

      // Draw channels from top to bottom.
      const double centerOfChannel = heightPerChannel*0.5 + (channels-1-i)*heightPerChannel;
      glTranslated(0.0, centerOfChannel, 0.0);
      glScaled(1.0, yTweak, 1.0);
      // Scaled waveform is dark echo behind bright unscaled one.
      // getbatch()'s last arg avoids vanishingly short vertical lines, which would render as a missing horizontal line.
      float minmaxes[9000*2]; // 9000 hardcoded
      assert(9000 > pixelSize[0]);

      it->cacheWav->getbatch(minmaxes, tShow[0], tShow[1], pixelSize[0], 0.5/(it->scaleWav * pixelSize[1])/yTweak);
      // When zoomed in so only a thin dark line is barely visible, make this an area by extending each minmax to zero.
      // For example, [.2,.3] becomes [0,.3];  [-.8,-.4] becomes [-.8,0];  [-.1,.1] is unchanged.
      // (Even prettier would be to extend only to the scaleWavDefault curve, instead of all the way to the x-axis.)
      for (int x = 0; x < pixelSize[0]; ++x) {
	float& yMin = minmaxes[x*2];
	float& yMax = minmaxes[x*2+1];
	if (yMin > 0.0) yMin = 0.0;
	if (yMax < 0.0) yMax = 0.0;
      }
      drawWaveformScaled(minmaxes, it->scaleWav);

      // Bug in cache?  When multichannel and only partially zoomed in (>1 value per x-pixel), lines are sometimes dotted.
      // But increasing 0.5 makes the lines so fat that they're ugly.
      it->cacheWav->getbatch(minmaxes, tShow[0], tShow[1], pixelSize[0], 0.5/(scaleWavDefault * pixelSize[1])/yTweak/yZoom);
      drawWaveformScaled(minmaxes, scaleWavDefault);
    glPopMatrix();
  }
}
#endif

double sMouseRuler = 0.0;
void drawMouseRuler()
{
  shaderUse(false);
  const double xL = glFromSecond(sMouseRuler);
  const double xR = xL + 0.06;
  if (xR < 0.0 || xL > 1.0)
    return; // offscreen
  // (Abandoned purple horizontal fade to right of sharp line,
  // because multichannel might interleave waveforms and heatmap features.)

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
  void blink(const double t, const float r, const float g, const float b) {
    _t = t;
    _color[0] = _colorFaded[0] = r;
    _color[1] = _colorFaded[1] = g;
    _color[2] = _colorFaded[2] = b;
                _colorFaded[3] = 0.0;
    _color[3]                  = 1.0;
  };
  void draw() {
    shaderUse(false);
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
Flash flashLimits[2];

void limitBlink(const int i)
{
  assert(i==0 || i==1);
  flashLimits[i].blink(tShowBound[i], 1.0f, 0.1f, 0.1f);
}
void zoomBlink(const int i)
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

void aimCropY()
{
  if (yAim[0] < yShowBound[0]) {
    yAim[0] = yShowBound[0];
    //printf("limitBlinkY(0);\n");;;;
  }
  if (yAim[1] > yShowBound[1]) {
    yAim[1] = yShowBound[1];
    //printf("limitBlinkY(1);\n");;;;
  }
}

bool fHeld = false;			// Left  mouse button is held down.
bool fHeldRight = false;		// Right mouse button is held down.
bool fDrag = false;			// Mouse was left-dragged, not merely left-clicked.
double xyMouse[2] = {0.5, 0.15};	// Mouse position in GL coords.
double xMouseAim = 0.0, yMouseAim = 0.0;// Mouse position in world coords.  Setpoint for xyMouse.
bool fReshaped = false;

void aim()
{
  if (!fReshaped)
    return; // pixelSize[] not yet defined

  double lowpass = 0.0345 * exp(67.0 * secsPerFrame);
  // Disable aim/show at slow frame rates, to avoid flicker.
  if (secsPerFrame > 1.0/30.0)
    lowpass = 1.0;

  if (lowpass > 1.0)
    lowpass = 1.0;
  const double lowpass1 = 1.0 - lowpass;
  xyMouse[0] = xyMouse[0]*lowpass1 + xMouseAim*lowpass;
  xyMouse[1] = xyMouse[1]*lowpass1 + yMouseAim*lowpass;

  // Update tShow and tShowPrev from tAim.

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

  // Clamp zoomin, tweaked for 5 msec undersampling.
  // (This may interact with the constraint on tShowBound,
  // but any oscillation should converge.)

  const double samplesPerPixelMin = 1.0;// For EEG, 1.0.  For audio, 100.0 * (0.005 * SR) * 3.
  const double dtZoomMin = samplesPerPixelMin * pixelSize[0] / SR;
  const bool fZoominLimit = tAim[1] - tAim[0] < dtZoomMin;
  if (fZoominLimit) {
    const double tFixed = (tAim[0] + tAim[1]) / 2.0;
    tAim[0] = tFixed - dtZoomMin * 0.50001;
    tAim[1] = tFixed + dtZoomMin * 0.50001;
  }
  aimCrop();

  for (int i=0; i<2; ++i) {
    tShow[i] = tShow[i]*lowpass1 + tAim[i]*lowpass;
    if (fZoominLimit)
      zoomBlink(i);
  }
  tShowPrev[0] = tShow[0];
  tShowPrev[1] = tShow[1];

  // Update yShow and yShowPrev from yAim.

  assert(yShowBound[0] < yShowBound[1]);
  assert(yAim[0] < yAim[1]);

  // If yAim is disjoint with yShowBound (!!), pan it back into range.
  if (yAim[0] >= yShowBound[1]) {
    yAim[0] -= yAim[1] - yShowBound[1];
    yAim[1] = yShowBound[1];
  }
  else if (yAim[1] <= yShowBound[0]) {
    yAim[1] += yShowBound[0] - yAim[0];
    yAim[0] = yShowBound[0];
  }
  assert(yAim[0] < yShowBound[1]);
  assert(yAim[1] > yShowBound[0]);
  aimCropY();

  const double dyZoomMin = 1.0 / channels;
  const bool fZoominLimitY = yAim[1] - yAim[0] < dyZoomMin;
  if (fZoominLimitY) {
    // printf("Hit fZoominLimitY %.2f.\n", dyZoomMin); // aimCropY will also warn.
    const double yFixed = (yAim[0] + yAim[1]) / 2.0;
    yAim[0] = yFixed - dyZoomMin * 0.50001;
    yAim[1] = yFixed + dyZoomMin * 0.50001;
  }
  aimCropY();

  assert(yShowBound[0] <= yAim[0]);  assert(yAim[1]  <= yShowBound[1]);
  assert(yShowBound[0] <= yShow[0]); assert(yShow[1] <= yShowBound[1]);

  for (int i=0; i<2; ++i) {
    yShow[i] = yShow[i]*lowpass1 + yAim[i]*lowpass;
    /*if (fZoominLimitY)
      zoomBlinkY(i);*/
  }
  //printf("\t\t\t\t\tyShow %.2f .. %.2f\n", yShow[0], yShow[1]);;;;
  assert(yShowBound[0] - 1e-5 <= yShow[0]); assert(yShow[1] <= yShowBound[1] + 1e-5);
  yShowPrev[0] = yShow[0];
  yShowPrev[1] = yShow[1];

#ifdef WAVEDRAW
  // todo: in Windows, try PPL's parallel_for_each.  Good for an EEG's 90+ WaveDraw's.
  std::for_each(wavedrawers.begin(), wavedrawers.end(), std::mem_fun_ref(&WaveDraw::update));
#endif
}

#if 0
void debugYCoords()
{
    for (double y = -1.0; y < 2.0; y += 0.01) {
      glRasterPos2d(0.5, y);
      char sz[80];
      sprintf(sz, "%.2f", y);
      putsGlut(sz);
    }
}
#endif

void scrollwheelY(double y, const bool fIn)
{
  // Undo yTimeline transformation: map 0..1 to yTimeline..1.
  y = (y - yTimeline) / (1.0-yTimeline);
  const double yFixed = yFromGL(y);
  assert(yShow[0] <= yFixed);
  assert(yFixed <= yShow[1]);

  //printf("scrollY at %.3f, yFixed %.3f\n", y, yFixed);;;;
  const double zoom = fIn ? 1.0/1.08 : 1.08; // Smaller than for x aka t, because range is also smaller.
  yAim[0] = yFixed + (yAim[0]-yFixed) * zoom;
  yAim[1] = yFixed + (yAim[1]-yFixed) * zoom;
}

// later, increase step for successive steps in same direction, resetting this after 0.5 seconds elapse.
void scrollwheel(const double x, const bool fIn, const bool fFast)
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
  shaderUse(false);
  glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texNoise);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glPushMatrix();
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
inline double xFromMouse(const double xM) { return       xM/pixelSize[0]; }
inline double yFromMouse(const double yM) { return 1.0 - yM/pixelSize[1]; }

void warnIgnoreKeystroke(const unsigned char key)
{
  printf("Ignoring keystroke ");
  if (!isascii(key))
    printf("<0x%02x>", key & 0xFF);  // outside ASCII range 
  else if (isprint(key))
    printf("%c", key);
  else if (key < ' ')
    printf("<CTRL+%c>", key+'@');
  else if (key == ' ')
    printf("<SPACE>");
  else
    printf("<DEL>");
  printf("\n");
}

void keyboard(const unsigned char key, const int x, int /*y*/)
{
  if (!fReshaped)
    return; // pixelSize[] not yet defined

  // Fraction of horizontal screen to pan, per frame.
  // Slow enough to let user react to what they see, when holding key down.
  // Fast enough to not be tedious, requiring multiple keystrokes when investigating in detail.
  const double panspeed = 0.25;

  switch(key) {
    case 3: // ctrl+C
    case 23: // ctrl+W
      vfQuit = true;
      snooze(0.4); // let other threads notice vfQuit
      exit(0);
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
	const double dsec = std::min(dsecondFromGL(panspeed), tShowBound[1]-tAim[1] + 1e-4);
	if (dsec > 0.0) {
	  tAim[0] += dsec;
	  tAim[1] += dsec;
	}
      }
      break;
    case 'a':
      {
	const double dsec = std::min(dsecondFromGL(panspeed), tAim[0]-tShowBound[0] + 1e-4);
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
      warnIgnoreKeystroke(key);
      break;
  }
}

bool vfFakeDrag = false;
void drag(const int x, const int y)
{
  if (!fReshaped)
    return; // pixelSize[] not yet defined

  // Mouse may not have actually moved, if called from button callback.

  if (fHeld) {
    xMouseAim = xFromMouse(x);
    const double xpan = xMouseAim;
    const double dsec = dsecondFromGL(xyPanPrev[0] - xpan);
    tAim[0] += dsec;
    tAim[1] += dsec;
    xyPanPrev[0] = xpan;

    yMouseAim = yFromMouse(y);
    const double ypan = yMouseAim;
    const double dy = dyFromGL(xyPanPrev[1] - ypan);
    yAim[0] += dy;
    yAim[1] += dy;
    xyPanPrev[1] = ypan;

    fDrag |= !vfFakeDrag; // distinguish left click from left drag
  }
  if (fHeldRight)
    sMouseRuler = secondFromGL(xFromMouse(x));

  // If you're dextrous enough to drag while holding both buttons, feel free to do so.
}

void mouse(const int button, const int state, const int x, const int y)
{
  if (!fReshaped)
    return; // pixelSize[] not yet defined

  const bool fShift = (glutGetModifiers() & GLUT_ACTIVE_SHIFT) != 0;

  const int _ = 100000; // switch() on button AND state
  switch (button*_ + state) {
    case GLUT_WHEEL_UP*_ + GLUT_DOWN:
      if (fShift)
	scrollwheelY(yFromMouse(y), true);
      else
	scrollwheel(xFromMouse(x), true, false);
      break;
    case GLUT_WHEEL_DOWN*_ + GLUT_DOWN:
      if (fShift)
	scrollwheelY(yFromMouse(y), false);
      else
	scrollwheel(xFromMouse(x), false, false);
      break;
    case GLUT_LEFT_BUTTON*_ + GLUT_DOWN:
      fHeld = true;
      fDrag = false;
      xyMouse[0] = xyPanPrev[0] = xFromMouse(x);
      xyMouse[1] = xyPanPrev[1] = yFromMouse(y);
      break;
    case GLUT_RIGHT_BUTTON*_ + GLUT_DOWN:
      fHeldRight = true;
      fDrag = false;
      xyPanPrev[0] = xFromMouse(x);
      xyPanPrev[1] = yFromMouse(y);
      break;
    case GLUT_LEFT_BUTTON*_ + GLUT_UP:
      if (!fDrag)
	sMouseRuler = secondFromGL(xFromMouse(x));
        // click not drag
      fHeld = false;
      fDrag = false;
      break;
    case GLUT_RIGHT_BUTTON*_ + GLUT_UP:
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
void tick(const double x, const char* label)
{
  if (ctick >= 1000)
    return; // silently
  const double width2 = strlen(label) * dxChar / 2.0;
  ticked[ctick][0] = x - width2;
  ticked[ctick][1] = x + width2;
  ++ctick;
}

bool fTickIncluded(const double x, const char* label)
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

void doTick(const double x, const char* label)
{
  if (fTickIncluded(x, label))
    return; // covered by another tick
  tick(x, label);
  const double width2 = strlen(label) * dxChar / 2.0;
  glRasterPos2d(x-width2, 0.5); // 0.5 is midway along the timeline's [0,1] y-extent.
  putsGlut(label);
}

void drawTicks()
{
  const int cUnit = 11;
  const char* szTick[cUnit] = { "week", "day", "h", "10m", "m", "10s", "s", "s/10", "s/100", "ms", "us" };
  const double secTick[cUnit] = { 7 * 86400, 86400, 3600, 600, 60, 10, 1, .1, .01, .001, 1e-6 };
  // Sadly, no standard units fill the gaps between h and m, m and s, ms and us.
  const double gl0 = glFromSecond(0);

  const double dxMin = 3.0/65.0;
  tickreset();
  shaderUse(false);
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

void reshape(const int w, const int h)
{
  glViewport(0, 0, pixelSize[0]=w, pixelSize[1]=h);
  // Assume monospace.
  dxChar = glutBitmapWidth(font, 'a') / double(pixelSize[0]);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluOrtho2D(0,1, 0,1);
  glMatrixMode(GL_MODELVIEW);
  fReshaped = true;
}

void drawThumb()
{
  const double dt = tShowBound[1] - tShowBound[0];
  double x0 = std::max(0.0, (tShow[0] - tShowBound[0]) / dt);
  assert(pixelSize[0] > 0);
  const double dxMin = 2.0/pixelSize[0];
  const double x1Min = x0 + dxMin;
  double x1 = std::max(x1Min, (tShow[1] - tShowBound[0]) / dt);
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
  shaderUse(false);

  // fade out top edge, so thumb doesn't look manipulable.
  glBegin(GL_QUADS);
    const double yTop = 0.45; // [0,1] is the timeline's y-extent.
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

void drawAll()
{
  if (!fReshaped) {
    // Still starting up.  Window manager hasn't set the window size yet.
    return;
  }

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

  // Convert yShow[0], yShow[1] to dy, yZoom.
  const double dy = yShow[0];
  yZoom = 1.0 / (yShow[1] - yShow[0]); // used by drawWaveform().  Better would be for drawWaveform() to call dyFromOpengl().

  glPushMatrix();
    // Map y-range 0..1 to yTimeline..1.
    glTranslated(0.0, yTimeline, 0.0);
    glScaled(1.0, 1.0-yTimeline, 1.0);

    // Show yShow[0]..yShow[1].
    glScaled(1.0, yZoom, 1.0);
    glTranslated(0.0, -dy, 0.0);

    // Draw in unit square.  x and y both from 0 to 1.
    drawFeatures();
    drawMouseRuler();
#ifdef WAVEDRAW
    drawWaveform();
#endif
    // debugYCoords();
  glPopMatrix();

  glPushMatrix();
    // Map y-range 0..1 to 0..yTimeline.
    glScaled(1.0, yTimeline, 1.0);

    // Draw in unit square.  x and y both from 0 to 1.
    drawTimeline();
    drawTicks();
    drawThumb();
  glPopMatrix();

  flashLimits[0].draw();
  flashLimits[1].draw();
  drawCursors();
  glutSwapBuffers();
  aim();
}


void makeTextureNoise()
{
  const int w = 4096; // Wider than widest window.
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
void kickProducer(int /*is*/) {
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
		  rtaudioTick(vpsSamples, visSamples);
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
  extern void alsaInit(unsigned), alsaTick(const short*, ssize_t), alsaTerm();
  extern int alsaBuf();
  alsaInit(SR);

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

#ifdef _MSC_VER
int mainCore(int argc, char** const argv)
#else
int main(int argc, char** argv)
#endif
{
  appname = argv[0];
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
  if (argc > 2)
    quit("Usage: timeliner_run dirMarshal");
  if (argc == 2)
    dirMarshal = argv[1];

  const std::string wav(dirMarshal + std::string("/mixed.wav"));
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
  SR = sfinfo.samplerate;
  if (sfinfo.format != (SF_FORMAT_WAV|SF_FORMAT_PCM_16))
    warn("wavfile has unexpected format");
  wavcsamp = long(sfinfo.frames);
  channels = sfinfo.channels;
  wavS16 = new short[wavcsamp * channels];
  const long cs = long(sf_readf_short(pf, wavS16, wavcsamp));
  if (cs != wavcsamp)
    quit("failed to read wav");
  if (0 != sf_close(pf))
    warn("failed to close wav");

  // Clever fast memory-conserving shortcut for monophonic.
  short* channelS16 = channels==1 ? wavS16 : new short[wavcsamp];
#ifdef WAVEDRAW
  const double msec_resolution = 0.3;
  const double undersample = std::max(1.0, msec_resolution * 1e-3 * SR);
#endif
  for (unsigned i=0; i<channels; ++i) {
    if (channels != 1)
      for (long j=0; j<wavcsamp; ++j)
	channelS16[j] = wavS16[channels*j+i];
#ifdef WAVEDRAW
    wavedrawers.push_back(WaveDraw(new CHello(channelS16, wavcsamp, float(SR), int(undersample), 1)));
#endif
  }

  // todo: do this mixdown in timeliner_pre.cpp
  if (channels != 1) {
    // For now, just average all channels into monophonic audio playback.
    // todo: instead, a weighted sum that attenuates very weak channels.
    for (long j=0; j<wavcsamp; ++j) {
      int z = 0;
      for (unsigned i=0; i<channels; ++i) {
	z += wavS16[channels*j+i];
      }
      channelS16[j] = short(z/channels);
    }
    // Swap pointers.  Delete the no-longer-needed longer multichannel array,
    // and keep just the mixed-down monophonic array.
    delete [] wavS16;
    wavS16 = channelS16;
  }

  tAim[0] = tShow[0] = tShowBound[0] = 0.0;
  tAim[1] = tShow[1] = tShowBound[1] = wavcsamp / double(SR);

  glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
  glutInitWindowSize(pixelSize[0], pixelSize[1]);
  glutCreateWindow("Timeliner");
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
  shaderInit();

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
			  : (shift-) scrollwheel\n\
\n\
	Set playback cursor: right click or drag\n\
	Play/stop         : space\n\
\n\
	Quit              : ctrl+C\n\n");

  glutMainLoop();
  return 0;
}
