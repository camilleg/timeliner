#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS // for strcpy, getenv, _snprintf, fopen, fscanf
#endif

#undef DEBUG
#include "timeliner_diagnostics.h"
#include "timeliner_cache.h"
#include "timeliner_util.h" // #includes <windows.h>
#include "timeliner_util_threads.h"

// Linux:   apt-get install libsndfile1-dev
// Windows: www.mega-nerd.com/libsndfile/ libsndfile-1.0.25-w64-setup.exe
#include <sndfile.h>
#include <vector>

// Linux:   apt-get install libglew-dev
// Windows: http://glew.sourceforge.net/install.html
#include <GL/glew.h> // before gl.h
#include <GL/freeglut.h> // Instead of glut.h, to get glutLeaveMainLoop().

// Linux:   apt-get install libpng12-dev
// Windows: http://gnuwin32.sourceforge.net/packages/libpng.htm and http://zlib.net/
#include <png.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>

#ifdef _MSC_VER
#include <time.h>
#include <iostream>
#include <sstream>
#include <functional>
#else
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h> // mkdir
#include <sys/time.h>
#include <sys/types.h>
#include <linux/soundcard.h>
#include <unistd.h>
#endif

#if !defined(GLUT_WHEEL_UP)
#define GLUT_WHEEL_UP   (3)
#define GLUT_WHEEL_DOWN (4)
#endif

#undef making_movie
#ifdef making_movie
double sMovieRecordingStart = -1.0;
bool fMovieRecording = false;
bool fMoviePlaying = false;
const int izPlaybackMaxMax = 30 * 60; // 30 FPS, max 60 seconds
double rgzPlayback[izPlaybackMaxMax][5];
int izPlaybackMax = 0; // last frame
int izPlayback = 0; // current frame, at 30 FPS
FILE* fpMovie = NULL;
#define yscroll_disable
#endif

void snooze(const double sec)
#ifdef _MSC_VER
{ Sleep(DWORD(sec * 1e3)); }
#else
{ (void)usleep(sec * 1e6); }
#endif

#ifdef _MSC_VER
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

int SR = -1;

arLock vlockAudio; // guards next three
int vnumSamples = 0; // high-low-water-mark between samplewriter() and samplereader()
const short* vpsSamples = NULL; // vpsSamples and visSamples are set by samplewriter() via emit(), and cleared by samplereader().
int visSamples = 0;

class S2Splay {
public:
  S2Splay() : _sPlay(-1.0), _sPlayPrev(-1.0), _tPlay(appnow()), _fPlaying(false), _sampBgn(10000) {}
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
    if (!_fPlaying
#ifdef making_movie
	&& !fMoviePlaying
#endif
	)
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
#ifdef making_movie
	if (fMovieRecording)
	  fprintf(fpMovie, "# audio playback stop at offset = %f s\n", sPlayCursor1Nolock());
#endif
      } else {
	_sPlayPrev = s;
	soundplayNolock(s);
#ifdef making_movie
	if (fMovieRecording)
	  fprintf(fpMovie, "# audio playback start from wavfile offset = %f s, at screenshot-recording offset = %f s\n", s, appnow() - sMovieRecordingStart);
#endif
      }
    }
  }

#ifdef making_movie
  void moviePlayback(const double s, const double t) {
    _sPlay = s;
    _tPlay = s + appnow() - t; // inverse of sPlayCursor1Nolock()
  }
#endif

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

#ifdef _MSC_VER
inline float drand48() { return float(rand()) / float(RAND_MAX); }
#endif

WorkerPool* pool = NULL;

int Feature::mb = mbUnknown;
std::vector<Feature*> features;

std::vector<GLuint> myPrgs;
bool fShaderValid(const int i)
{
  return 0<=i && i<int(myPrgs.size()) && myPrgs[i]!=0;
}
void shaderUse(const int i = -1)
{
  glUseProgram(fShaderValid(i) ? myPrgs[i] : 0);
}

GLfloat paletteBrightness = 1.0;
void setPalette(const int iShader, const GLfloat r, const GLfloat g, const GLfloat b) {
  static GLfloat bufPalette[3*128]; // One bufPalette is enough for multiple shaders.
  for (int i=0; i<128; ++i) {
    const GLfloat z(paletteBrightness * sq(i/127.0f));
    switch (iShader) {
    default:
      bufPalette[3*i+0] = z * r;
      bufPalette[3*i+1] = z * g;
      bufPalette[3*i+2] = z * b;
      break;
    case 0:
      // waveform
      bufPalette[3*i+0] = i<127 ? 0.0f : r;
      bufPalette[3*i+1] = i<127 ? 0.0f : g;
      bufPalette[3*i+2] = i<127 ? 0.0f : b;
      break;
    case 1:
      // blackbody: black red yellow white.
      double hsv[3] = { cb(z) * 0.27, 1.0 - fi(z), z };
      extern void RgbFromHsv(double*);
      RgbFromHsv(hsv);
      bufPalette[3*i+0] = GLfloat(hsv[0]);
      bufPalette[3*i+1] = GLfloat(hsv[1]);
      bufPalette[3*i+2] = GLfloat(hsv[2]);
      break;
    }
  }
  assert(fShaderValid(iShader));
  assert(      glGetUniformLocation(myPrgs[iShader], "palette") >= 0);
  glUniform1fv(glGetUniformLocation(myPrgs[iShader], "palette"), 3*128, bufPalette);
}

void shaderRestart(const int iShader, const double r, const double g, const double b) {
  // Recompiling the shaders may be overkill for just redoing setPalette().
  shaderUse();
  if (fShaderValid(iShader))
    glDeleteProgram(myPrgs[iShader]);
  if (int(myPrgs.size()) < iShader+1) myPrgs.resize(iShader+1, -1);
  myPrgs[iShader] = glCreateProgram();
  const GLuint& myPrg = myPrgs[iShader];
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

  int ret = 0;
  //int cch=0, cch2=0; char sz[10000] = "";

  glCompileShader(myVS); glGetShaderiv(myVS, GL_COMPILE_STATUS, &ret); assert(ret != GL_FALSE);
  //glGetShaderiv(myFS, GL_INFO_LOG_LENGTH, &cch);
  //glGetShaderInfoLog(myFS, cch, &cch2, sz);
  //if (cch2>0) printf("vert compile: %s\n", sz);

  glCompileShader(myFS); glGetShaderiv(myFS, GL_COMPILE_STATUS, &ret); assert(ret != GL_FALSE);
  //cch = cch2 = 0;
  //glGetShaderiv(myFS, GL_INFO_LOG_LENGTH, &cch);
  //glGetShaderInfoLog(myFS, cch, &cch2, sz);
  //if (cch2>0) printf("frag compile: %s\n", sz);

  glAttachShader(myPrg, myVS);
  glAttachShader(myPrg, myFS);

  glLinkProgram(myPrg); glGetProgramiv(myPrg, GL_LINK_STATUS,   &ret); assert(ret != GL_FALSE);
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

  shaderUse(iShader); // before calling any glUniform()s, so they know which program to refer to.
  setPalette(iShader, GLfloat(r),GLfloat(g),GLfloat(b));

  glActiveTexture(GL_TEXTURE0); // use texture unit 0
  assert(     glGetUniformLocation(myPrg, "heatmap") >= 0);
  glUniform1i(glGetUniformLocation(myPrg, "heatmap"), 0); // Bind sampler to texture unit 0.  www.opengl.org/wiki/Texture#Texture_image_units
}

void kickShaders()
{
  shaderRestart(0,  0.2, 1.0, 0.2); // waveform is green
  for (unsigned i=1; i<features.size(); ++i)
    shaderRestart(i,  0.9-0.2*i, 0.7, 0.4+0.1*i);
}

void shaderInit()
{
  glewInit();
  assert(glewIsSupported("GL_VERSION_2_0"));
  kickShaders();
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

  for (f=features.begin(),i=0; f!=features.end(); ++f,++i) {
    shaderUse(i);
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
  shaderUse();
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
  shaderUse();
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
    memset(_colorFaded, 0, sizeof(_colorFaded));
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
    shaderUse();
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

// Update tShow and yShow from tAim and yAim, from xyMouse.
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
#ifndef yscroll_disable
  // Undo yTimeline transformation: map 0..1 to yTimeline..1.
  y = (y - yTimeline) / (1.0-yTimeline);
  const double yFixed = yFromGL(y);
  assert(yShow[0] <= yFixed);
  assert(yFixed <= yShow[1]);

  //printf("scrollY at %.3f, yFixed %.3f\n", y, yFixed);;;;
  const double zoom = fIn ? 1.0/1.08 : 1.08; // Smaller than for x aka t, because range is also smaller.
  yAim[0] = yFixed + (yAim[0]-yFixed) * zoom;
  yAim[1] = yFixed + (yAim[1]-yFixed) * zoom;
#endif
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
  shaderUse();
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

bool vfQuit = false;
void appQuit()
{
  vfQuit = true;
  snooze(0.2); // let other threads notice vfQuit
  glutLeaveMainLoop();
}

void keyboard(const unsigned char key, const int x, int /*y*/)
{
  if (vfQuit)
    return;
  if (!fReshaped)
    return; // pixelSize[] not yet defined

  // Fraction of horizontal screen to pan, per frame.
  // Slow enough to let user react to what they see, when holding key down.
  // Fast enough to not be tedious, requiring multiple keystrokes when investigating in detail.
  const double panspeed = 0.25;

  switch(key) {
    case 'c'-'a'+1: // ctrl+C
    case 'w'-'a'+1: // ctrl+W
      appQuit();
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

    case 'b':
      paletteBrightness *= 1.03f;
      goto LReshade;
    case 'B':
      paletteBrightness /= 1.03f;
      goto LReshade;
    case 'b'-'a'+1: // ctrl+B
      paletteBrightness = 1.0f;
LReshade:
      kickShaders();
      break;

#ifdef making_movie
    case 'p':
      fMoviePlaying = true;
      info("playing movie: ensure window is entirely visible"); // glReadPixels() returns noise for offscreen or cropped pixels
      break;
    case 'P':
      fMoviePlaying = false;
      info("aborted playing movie");
      izPlayback = 0;
      izPlaybackMax = 0;
      break;
    case 'r':
      fMovieRecording = true;
      sMovieRecordingStart = appnow();
      info("recording movie");
      break;
    case 'R':
      fMovieRecording = false;
      info("recorded movie");
      break;
#endif

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

#ifndef yscroll_disable
    yMouseAim = yFromMouse(y);
    const double ypan = yMouseAim;
    const double dy = dyFromGL(xyPanPrev[1] - ypan);
    yAim[0] += dy;
    yAim[1] += dy;
    xyPanPrev[1] = ypan;
#endif

    fDrag |= !vfFakeDrag; // distinguish left click from left drag
  }
  if (fHeldRight)
    sMouseRuler = secondFromGL(xFromMouse(x));

  // If you're dextrous enough to drag while holding both buttons, feel free to do so.
}

void mouse(const int button, const int state, const int x, const int y)
{
  if (vfQuit)
    return;
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
  shaderUse();
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
  if (vfQuit)
    return;
#ifdef making_movie
  w = 1280;
  h = 720;
#endif
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
  shaderUse();

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

// Save a screenshot (excluding the mouse cursor) as a .png file.
// From http://zarb.org/~gc/html/libpng.html.
bool screenshot(const char* filename)
{
  FILE* fp = fopen(filename, "wb");
  if (!fp)
    return false;
  const int width = pixelSize[0];
  const int height = pixelSize[1];

  // Read pixels.  With libpng, RGBA works better than RGB.
  unsigned char* pRGB = new unsigned char[width * height * 4];
  glReadPixels(0,0, width,height,  GL_RGBA,  GL_UNSIGNED_BYTE, pRGB);

  png_structp pPNG = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!pPNG) {
    warn("screenshot problem 1");
    fclose(fp);
    return false;
  }
  png_infop pInfoPNG = png_create_info_struct(pPNG);
  if (!pInfoPNG) {
    warn("screenshot problem 2");
    fclose(fp);
    return false;
  }
  png_init_io(pPNG, fp);
  png_set_IHDR(pPNG, pInfoPNG, width,height, 8, PNG_COLOR_TYPE_RGBA,
    PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(pPNG, pInfoPNG);

  // Copy pixels from pRGB to PNG.
  png_byte** ppRowPNG = new png_byte* [height];
  for (int y=0; y<height; ++y) {
    ppRowPNG[y] = new png_byte[png_get_rowbytes(pPNG, pInfoPNG)];
    for (int x=0; x<width; ++x) {
      const unsigned char* pbSrc = pRGB + 4*((height-1-y)*width + x);
      std::copy(pbSrc, pbSrc+4, &ppRowPNG[y][x*4]);
    }
  }
  png_write_image(pPNG, ppRowPNG);
  png_write_end(pPNG, NULL);

  for (int y=0; y<height; ++y)
    delete [] ppRowPNG[y];
  delete [] ppRowPNG;
  png_destroy_write_struct(&pPNG, &pInfoPNG);
  fclose(fp);
  delete [] pRGB;
  return true;
}

#ifdef making_movie

void movieRecord()
{
  static double tRecordStart = -1.0;
  static int iFrame = 0;
  if (tRecordStart < 0.0) {
    tRecordStart = appnow();
    fpMovie = fopen("timeliner-recording.txt", "w");
  }
  // 30 FPS for video
  const double t = tRecordStart + iFrame/30.0;
  if (appnow() >= t) {
    double rgs[2] = {-1.0,-1.0};
    (void)s2s.sPlayCursors(rgs);
    fprintf(fpMovie, "%f %f %f %f %f\n", tShow[0], tShow[1], sMouseRuler/*purple*/, rgs[0],rgs[1]/*green*/);
    ++iFrame;
  }
}

const char* movieDir =
#ifdef _MSC_VER
  ".";
#else
  "/run/shm/timeliner/";
#endif
void moviePlayback()
{
  if (izPlaybackMax == 0) {
#ifndef _MSC_VER
    if (0 != access(movieDir, F_OK)) {
      switch (errno) {
      case ENOENT:
	if (mkdir(movieDir, 0644) != 0)
	  // fallthrough
      case ENOTDIR:
	  quit("failed to store screenshots");
	break;
      }
      // movieDir exists.
    }
    (void)!system("/bin/rm -f /run/shm/timeliner/*.png");
    // Removing either the nonempty dir or the wildcard *.png, without system(),
    // is necessarily elaborate: http://stackoverflow.com/questions/1149764/delete-folder-with-items
#endif
    // Parse timeliner-recording.txt into an array of floats.
    FILE* fp = fopen("timeliner-recording.txt", "r");
    while (fp && izPlaybackMax < izPlaybackMaxMax) {
      char line[200];
      if (1 != fscanf(fp, "%[^\n]\n", line))
	break;
      if (line[0] == '#')
	continue;		// comment, e.g. seconds-offset of audio playback
      double* pz = rgzPlayback[izPlaybackMax++];
      if (5 != sscanf(line, "%lf %lf %lf %lf %lf", pz, pz+1, pz+2, pz+3, pz+4))
	break;
    }
    fclose(fp);
    printf("Parsed %d playback frames = %.1f seconds.\n", izPlaybackMax-1, (izPlaybackMax-1)/30.0);
  }
  if (!vfQuit) {
    const double* pz = rgzPlayback[izPlayback];
    tShow[0] = pz[0];
    tShow[1] = pz[1];
    sMouseRuler = pz[2];
    s2s.moviePlayback(pz[3], pz[4]);
    //Cursors
    if (++izPlayback >= izPlaybackMax) {
      printf("Playback complete.\n");
      appQuit();
    }
  }
}

void movieSaveFrame()
{
  char filename[80];
  sprintf(filename, "%s%05d.png", movieDir, izPlayback);
  screenshot(filename);
  // Afterwards: cd /run/shm/timeliner; ..../movie-soundtrack.rb; ffmpeg -y -r 30 -i %05d.png -i out.wav -map 0:0 -map 1:0 -ar 44100 -c:v h264 out.mov
  // Then optionally crop: ffmpeg -i out.mov -t 60 -c:a copy -c:v copy outShorter.mov
}
#endif

void drawAll()
{
  if (vfQuit)
    return;
  if (!fReshaped) {
    // Still starting up.  Window manager hasn't set the window size yet.
    return;
  }

#ifdef making_movie
  if (fMoviePlaying)
    moviePlayback();
#endif

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

#ifdef making_movie
  if (fMovieRecording)
    movieRecord();
  if (fMoviePlaying)
    movieSaveFrame();
#endif

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

  glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_CONTINUE_EXECUTION);
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
    else
      delete f;
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
  (void)_beginthread(samplereader, 0, NULL);
  (void)_beginthread(samplewriter, 0, NULL);
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
  for (const auto f: features) delete f;
  return 0;
}
