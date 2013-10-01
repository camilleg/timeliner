#define GLEW_BUILD
#include <GL/glew.h> // before gl.h
#include <GL/glut.h>

#include <glm/glm.hpp>
#include <glm/ext.hpp>
// apt-get install libglm-dev
// www.opengl-tutorial.org

#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#if !defined(GLUT_WHEEL_UP)
#define GLUT_WHEEL_UP   (3)
#define GLUT_WHEEL_DOWN (4)
#endif

const double secsPerFrame = 1/40.0;

const double yShowBound[2] = { 0.0, 1.0 };
double yShowPrev[2] = {-2.0, -1.0}; // deliberately bogus
double yShow[2] = {yShowBound[0], yShowBound[1]}; // displayed interval of y, updated each frame
double yAim[2] = {yShow[0], yShow[1]}; // setpoint for yShow, updated more slowly

double tShowBound[2] = {0.0, 90000.0};
double tShowPrev[2] = {-2.0, -1.0};	// deliberately bogus
double tShow[2] = {tShowBound[0], tShowBound[1]};	// displayed interval of time, updated each frame
double tAim[2] = {tShow[0], tShow[1]};	// setpoint for tShow, updated more slowly

inline double dsecondFromGL(const double dx) { return dx * (tShow[1] - tShow[0]); }
inline double secondFromGL(const double x) { return dsecondFromGL(x) + tShow[0]; }
inline double glFromSecond(const double s) { return (s - tShow[0]) / (tShow[1] - tShow[0]); }

inline double dyFromGL(const double dy) { return dy * (yShow[1] - yShow[0]); }
inline double yFromGL(const double y) { return dyFromGL(y) + yShow[0]; }
inline double glFromY(const double y) { return  (y - yShow[0]) / (yShow[1] - yShow[0]); }

double linearlerp(const double z, const double min, const double max) { return z*max + (1.0-z)*min; }

double dxChar = 0.01;
#define font GLUT_BITMAP_9_BY_15
char sprintfbuf[10000];
void putsGlut(const char* pch = sprintfbuf, bool fMask = true)
{
  while (*pch) glutBitmapCharacter(font, *pch++);
}
// Alternative: http://stackoverflow.com/questions/5262951/what-is-state-of-the-art-for-text-rendering-in-opengl-as-of-version-4-1 ,
// which cites: http://bytewrangler.blogspot.co.uk/2011/10/signed-distance-fields.html GLSL'd: replace clip() with if (text.a<0.5) {discard;}

void prepTextureMipmap(const GLuint t)
{
  glBindTexture(GL_TEXTURE_1D, t);
  assert(glIsTexture(t) == GL_TRUE);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glTexParameterf(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
}

const int SR = 8000;
const long wavcsamp = SR*10; // duration of test pattern

#include "timeliner_cache.h"
class Feature {
  enum { vecLim = CQuartet_widthMax+1 };
  class Slartibartfast { public: GLuint tex[vecLim]; };
public:
  int cchunk;
  std::vector<Slartibartfast> rgTex;

  Feature() {
    _vectorsize = 5; // pixel height
    _period = 1.0/SR;	// waveform's sample rate
    const int slices = wavcsamp;
    _cz = slices*_vectorsize;
    float* pz = new float[_cz];
    for (int t=0; t < slices; ++t)
      for (int s=0; s < _vectorsize; ++s) {
	// test pattern
	const float x = t / (slices-1.0);
	const float y = s / (_vectorsize-1.0);
	const float xPulse = (t % 1000) * 0.001;
//	pz[t*_vectorsize+s] = 0.5*x + 0.5*y;
	pz[t*_vectorsize+s] = 0.5*x + 0.1*xPulse + 0.38*y; // + 0.11*drand48();
      }
    _pz = pz;
    makeMipmaps();
  }

  int vectorsize() const { return _vectorsize; }
  int samples() const { return _cz / _vectorsize; }

  void makeMipmaps() {
    const char* pch = getenv("timeliner_zoom");
    unsigned subsample = pch ? atoi(pch) : 1;
    if (subsample < 1)
      subsample = 1;

    const int csample = samples();
    unsigned width = 1;
    while (width < csample/subsample)
      width *= 2;

    // Minimize cchunk
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
      glGenTextures(_vectorsize, rgTex[ichunk].tex);
      for (int j=0; j < _vectorsize; ++j)
	prepTextureMipmap(rgTex[ichunk].tex[j]);
    }

    const CHello cacheHTK(_pz, _cz, 1.0f/_period, subsample, _vectorsize);
    // www.opengl.org/archives/resources/features/KilgardTechniques/oglpitfall/ #5 Not Setting All Mipmap Levels.
    unsigned level;
    for (level=0; (width/cchunk)>>level >= 1; ++level) {
      makeTextureMipmap(cacheHTK, level, width >> level);
    }
  }

  const void makeTextureMipmap(const CHello& cacheHTK, const int mipmaplevel, int width) const {
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
	  linearlerp(chunkL, tShowBound[0], tShowBound[1]),
	  linearlerp(chunkR, tShowBound[0], tShowBound[1]),
	  vectorsize(), width, 1);
      for (int j=0; j<vectorsize(); ++j) {
	assert(glIsTexture(rgTex[ichunk].tex[j]) == GL_TRUE);
	glBindTexture(GL_TEXTURE_1D, rgTex[ichunk].tex[j]);
	glTexImage1D(GL_TEXTURE_1D, mipmaplevel, GL_INTENSITY8, width, 0, GL_RED, GL_UNSIGNED_BYTE, bufByte + width*j);
      }
    }
    delete [] bufByte;
  };

private:
  float _period;	// seconds per sample
  int _vectorsize;	// e.g., how many frequency bins in a spectrogram
  const float* _pz;	// _pz[0.._cz] is the (vectors of) raw data
  long _cz;
};
std::vector<Feature*> features;

// For test pattern.
// v=1,s=0 yields white.
// v=0,s=* yields black.
// v=1,s=1 yields pure h.
static inline void RgbFromHsv(float* a)
{
  const float v = a[2];
  const float s = a[1];
  if (s == 0.0) {
    // pure grey
    a[0] = a[1] /* = a[2] */ = v;
    return;
  }
  float h = a[0];
  const float hue = h * 6.0F;
  h = hue - floor(hue);
  const float p = v * (1.0F - s);
  const float q = v * (1.0F - s * h);
  const float t = v  *(1.0F - s * (1.0F - h));
  switch (int(floor(hue))) {
    case 0: a[0]=v; a[1]=t; a[2]=p; return;
    case 1: a[0]=q; a[1]=v; a[2]=p; return;
    case 2: a[0]=p; a[1]=v; a[2]=t; return;
    case 3: a[0]=p; a[1]=q; a[2]=v; return;
    case 4: a[0]=t; a[1]=p; a[2]=v; return;
    case 5: a[0]=v; a[1]=p; a[2]=q; return;
  }
  a[0]=0.0; a[1]=0.0; a[2]=0.0;
}

GLuint myPrg = -1;
void shaderInit()
{
  glewInit();
  assert(glewIsSupported("GL_VERSION_2_0"));
  myPrg = glCreateProgram();
  assert(myPrg > 0);
  const GLuint myVS = glCreateShader(GL_VERTEX_SHADER);
  const GLuint myFS = glCreateShader(GL_FRAGMENT_SHADER);

  // TODO: replace deprecated gl_MultiTexCoord0 with something.  "Define my own AttrMultiTexCoord0"?
  const GLchar* prgV = "#version 130\nout float u; in vec4 vVertex; uniform mat4 mvp; void main() { gl_Position = mvp * vVertex; u = gl_MultiTexCoord0.s; }";

  // u traverses 0..1 within each "chunk" of texture, as it should.  Set by prgV.  Used by prgF.
  // i tracks the heatmap texture, 0 to 1.  (Then, 0.0 to 126.0.  127 is a sentinel value, avoiding array overflow without a slow conditional.)
  // i's resolution is float not byte, because u's (rgTex's) GL_TEXTURE_MAG_FILTER is GL_LINEAR not GL_NEAREST.
  // lerp goes from 0.0 to 1.0.  (GLSL's mod() isn't HLSL's.  http://stackoverflow.com/questions/7610631/glsl-mod-vs-hlsl-fmod)
  // (If another "out" is added to prgF, specify output destinations with glBindFragDataLocation,
  // glDrawBuffers GL_COLOR_ATTACHMENT0, etc.  With only one output, it defaults to "output 0, index 0.")
#ifdef DISABLE_LERPED_PALETTE_LOOKUP
  const GLchar* prgF = "#version 130\n\
    in float u; uniform sampler1D heatmap; uniform float palette[3*128]; out vec4 fragColor; \n void main() {\n\
    float i = texture(heatmap, u).r * 126.0;\n\
    int j = int(i) * 3;\n\
    fragColor = vec4(palette[j], palette[j+1], palette[j+2], 1.0);\n\
  }";
#else
  const GLchar* prgF = "#version 130\n\
    in float u; uniform sampler1D heatmap; uniform float palette[3*128]; out vec4 fragColor; \n void main() {\n\
    float i = texture(heatmap, u).r * 126.0;\n\
    float lerp1; float lerp = modf(i, lerp1);\n\
    /*lerp1==floor(lerp1)*/ int j = int(lerp1)*3;\n\
    lerp1 = 1.0 - lerp;\n\
    fragColor = vec4(\n\
      palette[j+3]*lerp + palette[j  ]*lerp1,\n\
      palette[j+4]*lerp + palette[j+1]*lerp1,\n\
      palette[j+5]*lerp + palette[j+2]*lerp1,\n\
      1.0) /* rgba */;\n\
  }";
#endif

  glShaderSource(myVS, 1, &prgV, NULL);
  glShaderSource(myFS, 1, &prgF, NULL);
  int r=0, cch=0, cch2=0; char sz[10000] = "dummy";

  glCompileShader(myVS); glGetShaderiv(myVS, GL_COMPILE_STATUS, &r);
  glGetShaderiv(myVS, GL_INFO_LOG_LENGTH, &cch);
  glGetShaderInfoLog(myVS, cch, &cch2, sz);
  if (cch2>0) printf("vert shader compile:\n%s\n", sz);
  assert(r != GL_FALSE);

  glCompileShader(myFS); glGetShaderiv(myFS, GL_COMPILE_STATUS, &r);
  cch = cch2 = 0;
  glGetShaderiv(myFS, GL_INFO_LOG_LENGTH, &cch);
  glGetShaderInfoLog(myFS, cch, &cch2, sz);
  if (cch2>0) printf("frag shader compile:\n%s\n", sz);
  assert(r != GL_FALSE);

  glAttachShader(myPrg, myVS);
  glAttachShader(myPrg, myFS);

  glLinkProgram(myPrg); glGetProgramiv(myPrg, GL_LINK_STATUS,   &r);
  cch = cch2 = 0;
  glGetProgramiv(myPrg, GL_INFO_LOG_LENGTH, &cch);
  glGetProgramInfoLog(myPrg, cch, &cch2, sz);		// segfaults if bufPalette is too large
  if (cch2>0) printf("glLinkProgram: %d, %s\n", r, sz);
  assert(r != GL_FALSE);

  glUseProgram(myPrg); // Before calling glUniform()s, so they know which program to refer to.

  // 128 works, but 256 fails to glLinkProgram (and segfaults in glGetProgramInfoLog).
  // 3000-ish bytes may be the maximum for a uniform array.
  //
  // Alternatives: UBO; texelFetch() a buffer texture (TBO), SSBO.
  // http://stackoverflow.com/questions/7954927/glsl-passing-a-list-of-values-to-fragment-shader
  // http://rastergrid.com/blog/2010/01/uniform-buffers-vs-texture-buffers/
  GLfloat bufPalette[3*128];
  // Test palette.  Red-green gradient, with slight blue noise.  Black halfway.
  // Blue noise and black region's boundaries flicker during pan-zoom.
  // Because palette's applied AFTER mipmapping?  No-flicker works only with continuous-gradient palettes?

  // test pattern
#if 0
  for (int i=0; i<127; ++i) {
    const float z = i/126.0;
    // rgb
    bufPalette[3*i+0] = z;
    bufPalette[3*i+1] = 1.0-z;
    bufPalette[3*i+2] = 0.7 * drand48();
  }
  for (int i=50; i<53; ++i) {
    bufPalette[3*i+0] = 0.0;
    bufPalette[3*i+1] = 0.0;
    bufPalette[3*i+2] = 0.0;
  }
#else
  // rainbow
  for (int i=0; i<127; ++i) {
    const float z = i/126.0;
    // hsv
    bufPalette[3*i+0] = z;
    bufPalette[3*i+1] = 0.93;
    bufPalette[3*i+2] = 0.89 - 0.3*z;
    RgbFromHsv(bufPalette + 3*i);
  }
#endif

  // sentinel
  bufPalette[3*127+0] = bufPalette[3*126+0];
  bufPalette[3*127+1] = bufPalette[3*126+1];
  bufPalette[3*127+2] = bufPalette[3*126+2];

  assert(glGetUniformLocation(myPrg, "palette") >= 0);
  assert(glGetUniformLocation(myPrg, "heatmap") >= 0);
  assert(glGetUniformLocation(myPrg, "mvp") >= 0);

  glUniform1fv(glGetUniformLocation(myPrg, "palette"), 3*128, bufPalette);

#if 0
  // These have no effect.
  glActiveTexture(GL_TEXTURE0); // use texture unit 0
  glUniform1i(glGetUniformLocation(myPrg, "heatmap"), 0); // Bind sampler to texture unit 0.  http://www.opengl.org/wiki/Texture#Texture_image_units
  // glEnable(GL_TEXTURE_1D), GL_TEXTURE_ENV_MODE, etc are no-ops for shaders.
#endif
}

void drawFeatures()
{
  std::vector<Feature*>::const_iterator f;

  // Compute y's: amortize among vectorsizes.
  double rgdy[100];
  assert(features.size() < 100);
  int i=0;
  double sum = 0.0;
  for (f=features.begin(); f!=features.end(); ++f) {
    sum += rgdy[i++] = sqrt(double((*f)->vectorsize()));
    assert(i < 100);
  }
  // rgdy[0 .. i-1] are heights.

  double rgy[100];
  rgy[0] = 0.0;
  rgy[i] = 1.0;
  {
    const double rescale = sum / (rgy[i] - rgy[0]);
    for (int j=1; j<i; ++j)
      rgy[j] = rgy[j-1] + rgdy[j-1] / rescale;
    // rgy[0 .. i] are boundaries between features.
  }
  // TODO: compute rgy only once, and then save it.

  // glActiveTexture(GL_TEXTURE0); // use texture unit 0		// Has no effect.
  for (f=features.begin(),i=0; f!=features.end(); ++f,++i) {
    const double* p = rgy + i;
      const int jMax = (*f)->vectorsize();
      for (int ichunk=0; ichunk < (*f)->cchunk; ++ichunk) {
	for (int j=0; j<jMax; ++j) {
	  const double chunkL =  ichunk    / double((*f)->cchunk); // e.g., 5/8
	  const double chunkR = (ichunk+1) / double((*f)->cchunk); // e.g., 6/8
	  const double tBoundL = linearlerp(chunkL, tShowBound[0], tShowBound[1]);
	  const double tBoundR = linearlerp(chunkR, tShowBound[0], tShowBound[1]);
	  const double xL = (tBoundL - tShow[0]) / (tShow[1] - tShow[0]);
	  const double xR = (tBoundR - tShow[0]) / (tShow[1] - tShow[0]);
	  if (xR < 0.0 || 1.0 < xL)
	    continue; // offscreen
	  assert(glIsTexture((*f)->rgTex[ichunk].tex[j]) == GL_TRUE);
	  glBindTexture(GL_TEXTURE_1D, (*f)->rgTex[ichunk].tex[j]);
	  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	  const double yMin = linearlerp(double(j  )/jMax, p[0], p[1]);
	  const double yMax = linearlerp(double(j+1)/jMax, p[0], p[1]);
	  assert(p[0]<=yMin && yMin<yMax && yMax<=p[1]);
	  glBegin(GL_QUADS);
	    glTexCoord1d(0.0); glVertex2d(xL, yMin); glVertex2d(xL, yMax);
	    glTexCoord1d(1.0); glVertex2d(xR, yMax); glVertex2d(xR, yMin);
	  glEnd();
	}
      }
  }

  glUseProgram(0);
  glColor4f(0,0,1, 1);
  for (f=features.begin(),i=0; f!=features.end(); ++f,++i) {
    const double* p = rgy + i;
    glRasterPos2d(0.01, p[0] + 0.005);
    putsGlut("Label");
  }
  glUseProgram(myPrg);
}

void aimCrop()
{
  if (tAim[0] < tShowBound[0]) { tAim[0] = tShowBound[0]; }
  if (tAim[1] > tShowBound[1]) { tAim[1] = tShowBound[1]; }
}

void aimCropY()
{
  if (yAim[0] < yShowBound[0]) { yAim[0] = yShowBound[0]; }
  if (yAim[1] > yShowBound[1]) { yAim[1] = yShowBound[1]; }
}

bool fHeld = false;			// Left  mouse button is held down.
bool fDrag = false;			// Mouse was left-dragged, not merely left-clicked.
double xyMouse[2] = {0.5, 0.15};	// Mouse position in GL coords.
double xMouseAim = 0.0, yMouseAim = 0.0;// Mouse position in world coords.  Setpoint for xyMouse.
int pixelSize[2] = {1000,1000};

void aim()
{
  const double lowpass = 0.0345 * exp(67.0 * secsPerFrame);
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
  }
  else if (tAim[1] <= tShowBound[0]) {
    tAim[1] += tShowBound[0] - tAim[0];
    tAim[0] = tShowBound[0];
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

  const double dyZoomMin = 1.0;
  const bool fZoominLimitY = yAim[1] - yAim[0] < dyZoomMin;
  if (fZoominLimitY) {
    const double yFixed = (yAim[0] + yAim[1]) / 2.0;
    yAim[0] = yFixed - dyZoomMin * 0.50001;
    yAim[1] = yFixed + dyZoomMin * 0.50001;
  }
  aimCropY();

  assert(yShowBound[0] <= yAim[0]);  assert(yAim[1]  <= yShowBound[1]);
  assert(yShowBound[0] <= yShow[0]); assert(yShow[1] <= yShowBound[1]);

  for (int i=0; i<2; ++i) {
    yShow[i] = yShow[i]*lowpass1 + yAim[i]*lowpass;
  }
  assert(yShowBound[0] - 1e-5 <= yShow[0]); assert(yShow[1] <= yShowBound[1] + 1e-5);
  yShowPrev[0] = yShow[0];
  yShowPrev[1] = yShow[1];
}

void scrollwheelY(double y, const bool fIn)
{
  const double yFixed = yFromGL(y);
  assert(yShow[0] <= yFixed);
  assert(yFixed <= yShow[1]);

  const double zoom = fIn ? 1.0/1.08 : 1.08; // Smaller than for x aka t, because range is also smaller.
  yAim[0] = yFixed + (yAim[0]-yFixed) * zoom;
  yAim[1] = yFixed + (yAim[1]-yFixed) * zoom;
}

void scrollwheel(const double x, const bool fIn, const bool fFast)
{
  const double tFixed = secondFromGL(x);
  double zoom = fFast ? 1.14 : 1.19;
  if (fIn)
    zoom = 1.0 / zoom;
  tAim[0] = tFixed + (tAim[0]-tFixed) * zoom;
  tAim[1] = tFixed + (tAim[1]-tFixed) * zoom;
}

double xyPanPrev[2] = {0.0, 0.0};
inline double xFromMouse(const double xM) { return       xM/pixelSize[0]; }
inline double yFromMouse(const double yM) { return 1.0 - yM/pixelSize[1]; }

void keyboard(const unsigned char key, const int x, int /*y*/)
{
  // Fraction of horizontal screen to pan, per frame.
  // Slow enough to let user react to what they see, when holding key down.
  // Fast enough to not be tedious, requiring multiple keystrokes when investigating in detail.
  const double panspeed = 0.25;

  switch(key) {
    case 3: // ctrl+C
      exit(0);
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
  }
}

bool vfFakeDrag = false;
void drag(const int x, const int y)
{
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
}

void mouse(const int button, const int state, const int x, const int y)
{
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
    case GLUT_LEFT_BUTTON*_ + GLUT_UP:
      fHeld = false;
      fDrag = false;
      break;
  }
  vfFakeDrag = true; // Ugly global, because drag()'s signature can't change.
  drag(x,y);
  vfFakeDrag = false;
}

void reshape(const int w, const int h)
{
  glViewport(0, 0, pixelSize[0]=w, pixelSize[1]=h);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluOrtho2D(0,1, 0,1);
  glMatrixMode(GL_MODELVIEW);
}

void drawAll()
{
  glClear(GL_COLOR_BUFFER_BIT);
  glPushMatrix();
    const double dy = yShow[0];
    const double yZoom = 1.0 / (yShow[1] - yShow[0]);
    // Draw in unit square.  x and y both from 0 to 1.

    static const glm::mat4 matProj = glm::ortho(0.0f,1.0f,0.0f,1.0f, -1.0f,1.0f);		// like gluOrtho2D(0,1, 0,1)
    const glm::mat4 matView  = glm::translate(glm::mat4(1.0f), glm::vec3(0.0, -dy, 0.0));	// like glTranslated(0.0, -dy, 0.0)
    const glm::mat4 matScale = glm::scale    (glm::mat4(1.0f), glm::vec3(1.0, yZoom, 1.0));	// like glScaled(1.0, yZoom, 1.0)
    const glm::mat4 MVPmatrix = matProj * matView * matScale;
    glUniformMatrix4fv(glGetUniformLocation(myPrg, "mvp"), 1, GL_FALSE, glm::value_ptr(MVPmatrix));

#ifdef putsGlut_still_misplaced_for_nondefault_yShow
    glUseProgram(0);
    gluOrtho2D(0,1, 0,1);
    glTranslated(0.0, -dy, 0.0);
    glScaled(1.0, yZoom, 1.0);
    glUseProgram(myPrg);
#endif
    drawFeatures();
  glPopMatrix();
  glutSwapBuffers();
  aim();
}

int main(int argc, char** argv)
{
  glutInit(&argc, argv);

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
  const int cFeature = 3;
  for (int i=cFeature; i>=1; --i) features.push_back(new Feature());
  shaderInit();
  glutMainLoop();
  return 0;
}
