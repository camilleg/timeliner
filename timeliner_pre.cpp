#ifdef _MSC_VER
#include "timeliner/stdafx.h"
#define _CRT_SECURE_NO_WARNINGS		// for strcpy, getenv, _snprintf, fopen, fscanf
#endif

#undef DEBUG

#include "timeliner_diagnostics.h"

#include <algorithm> 
#include <cassert>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#ifdef _MSC_VER
#define NOMINMAX // Don't #define min and max in windows.h, so std::min works.
#include <windows.h>
#include <time.h>
#include <functional>			// not1
#include <cctype>				// isspace (NOT <locale>, so it works with std::ptr_fun)
#include <direct.h>				// _getcwd
#define getcwd(a,b) _getcwd(a,b)
#define mkdir(a,b)  _mkdir(a)
#define chdir(a)    _chdir(a)
inline double round(const double x) { return floor(x + 0.5); }
#else
#include <arpa/inet.h> // ntohl(), etc
#include <gsl/gsl_wavelet.h> // apt install libgsl-dev
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Linux:   apt-get install libsndfile1-dev
// Windows: www.mega-nerd.com/libsndfile/ libsndfile-1.0.25-w64-setup.exe
#include <sndfile.h>

std::string dirMarshal = ".timeliner_marshal";
std::string configfile = "timeliner_config.txt";

#include "timeliner_cache.h"
#include "timeliner_util.h"

// C++-11 deprecates this with std::to_string().
template <typename T> std::string to_str(const T& t) { std::ostringstream os; os << t; return os.str(); }

void fileFromString(const std::string& outfilename, const std::string& indata) {
  std::ofstream t(outfilename.c_str(), std::ios_base::binary | std::ios_base::out);
  t.write(indata.c_str(), indata.size());
}
void fileFromBuf(const std::string& outfilename, const char* pb, const long cb) {
  std::ofstream t(outfilename.c_str(), std::ios_base::binary | std::ios_base::out);
  t.write(pb, cb);
}
void fileFromBufs(const std::string& outfilename, const char* pb, const long cb, const float* pz, const long cz) {
  std::ofstream t(outfilename.c_str(), std::ios_base::binary | std::ios_base::out);
  // Header of bytes.  Body of floats.
  t.write(pb, cb);
  t.write((const char*)pz, cz*sizeof(float));
}

short** rawS16 = NULL;
long wavcsamp = -1;
int SR = -1;

#ifndef M_PI
#define M_PI (3.1415926535898)
#endif
double* hamming(int w) {
  double* a = new double[w];
  for (int n=0; n<w; ++n)
    a[n] = 0.54 - 0.46 * cos(2.0*M_PI*n / (w-1.0));
  return a;
}

// Modifies rgz in-place (with htonl()).
std::string writeHTK(const std::string& outfile, float* rgz, const long cz, const unsigned floatsPerSlice, const unsigned long sampPeriodHNSU) {
  if (cz <= 0)
    quit("refused to write HTK file with no data");
  for (int i=0; i<cz; ++i) {
    unsigned* pw = reinterpret_cast<unsigned*>(rgz+i);
    *pw = htonl(*pw);
  }
  const size_t cb = 12;
  char rgb[cb];
  // Explicit pw avoids the g++ 4.6.3 warning "dereferencing type-punned pointer will break strict-aliasing rules."
  unsigned* pw = reinterpret_cast<unsigned*>(rgb+0); *pw = htonl(cz/floatsPerSlice);
  *reinterpret_cast<unsigned*>(rgb+4)                    = htonl(sampPeriodHNSU);
  *reinterpret_cast<unsigned short*>(rgb+ 8) = htons(sizeof(float)*floatsPerSlice);
  *reinterpret_cast<unsigned short*>(rgb+10) = htons(9);
  fileFromBufs(outfile, rgb, cb, rgz, cz);
  return outfile;
}

std::string writeHTKWavelet(const int channel, const std::string& fileOut, const unsigned long sampPeriodHNSU) {
#ifdef _MSC_VER
	warn("wavelets nyi in Windows, because the port of GNU GSL is ancient and unmaintained.");
	return fileOut;
#else
  const unsigned cSampWindow = 32;
  gsl_wavelet* w = gsl_wavelet_alloc(gsl_wavelet_daubechies_centered, 4);
  gsl_wavelet_workspace* workspace = gsl_wavelet_workspace_alloc(cSampWindow);
  const float sSamp = float(sampPeriodHNSU) / 1e7;
  const size_t stride = sSamp * SR;
  if (cSampWindow < stride)
    warn("wavelet window is shorter than stride, so samples between windows will be ignored");
  double* weights = hamming(cSampWindow);
  const float cSamp = wavcsamp;
  float iSamp = 0.0;
  double* data = new double[cSampWindow];
  float* r = new float[cSampWindow * unsigned(cSamp/stride + 1)];
  long iz = 0;
  while (iSamp+cSampWindow < cSamp) {
    const unsigned i = unsigned(iSamp);

    // Read data, and convolve it with Hamming window.
    unsigned j;
    for (j=0; j<cSampWindow; ++j)
      data[j] = double(rawS16[channel][i+j]) * weights[j];

    // Apply wavelet transform to data[], in place.
    (void)gsl_wavelet_transform_forward(w, data, 1, cSampWindow, workspace);

    // Append this vector of wavelet coeffs to r[].
    for (j=0; j<cSampWindow; ++j)
      r[iz + j] = float(data[j]);
    iz += cSampWindow;
    iSamp += stride;
  }
  delete [] data;
  delete [] weights;
  gsl_wavelet_workspace_free(workspace);
  gsl_wavelet_free(w);
  const std::string s = writeHTK(fileOut, r, iz, cSampWindow, sampPeriodHNSU);
  delete [] r;
  return s;
#endif
}

long wavcsamp_fake = -1;
int channels_fake = -1;
unsigned channels = 1;  // mono, stereo, etc
int numchans = 62; // frequency bands per filterbank, mfcc, or wavelet
const unsigned long sampPeriodF = 100000;
const std::string sampPeriod = "100000.0"; // hnsu, hundreds of nanoseconds, i.e. 1e-7 seconds, or decimicroseconds.
const std::string sampPeriodTimes8 = "800000.0";

#ifdef _MSC_VER
const std::string Hcfg = "c:\\temp\\timeliner_hcopy.cfg";
const std::string Outfile = "c:\\temp\\timeliner_htk";
// TODO: instead of c:\\temp use _dupenv_s(&pch, &cch, "TEMP"), and then free cch.  Ditto for channel.wav.
#else
const std::string Hcfg = "/tmp/timeliner_hcopy.cfg";
const std::string Outfile = "/tmp/timeliner_htk";
#endif
std::string htkFromWav(const int channel, int iKind, const std::string& infile) {
  if (iKind < 0 || iKind >= 4)
    return infile;
  if (iKind == 3) {
#ifdef _MSC_VER
    warn("wavelets nyi in Windows; using filterbank instead.");
    iKind = 0;
#else
    return writeHTKWavelet(channel, Outfile, sampPeriodF);
#endif
  }
  if (iKind == 2)
    warn("Quicknet (qnsfws, feacat) support nyi.");

  // Use current value of numchans, not the compile-time default.
  const std::string ConfigCommon = "\
    USEHAMMING = T\n\
    ZMEANSOURCE = T\n\
    WINDOWSIZE = " + sampPeriodTimes8 + "\n\
    NUMCHANS = " + to_str(numchans) + "\n\
    HIFREQ = " + to_str(SR/2) + "\n\
    LOFREQ = 0\n\
    SOURCEFORMAT = WAV\n\
    USEPOWER = T\n\
    TARGETRATE = " + sampPeriod + "\n\
    PREEMCOEF = 0.97\n\
    ";
  const std::string Config[3] = {
    "TARGETKIND = FBANK_Z\n",
    "TARGETKIND = MFCC_Z\nNUMCEPS = " + to_str(numchans) + "\n",
    "TARGETKIND = FBANK_D_A_Z\n"
  };
  fileFromString(Hcfg, ConfigCommon + Config[iKind]);
  (void)remove(Outfile.c_str()); // Mere paranoia.  The file really shouldn't be there.

#ifdef _MSC_VER
  const std::string channelfile = "c:\\temp\\channel.wav";
#else
  const std::string channelfile = "/tmp/channel.wav";
#endif

  // Create channelfile from rawS16[channel].
  SF_INFO sfinfo;
  sfinfo.samplerate = SR;
  sfinfo.channels = 1;
  sfinfo.format = SF_FORMAT_WAV|SF_FORMAT_PCM_16;
  SNDFILE* pf = sf_open(channelfile.c_str(), SFM_WRITE, &sfinfo);
  if (!pf) {
#ifdef _MSC_VER
    char buf[MAX_PATH];
    (void)GetFullPathNameA(channelfile.c_str(), MAX_PATH, buf, NULL);
    quit(std::string("no wav file ") + std::string(buf));
#else
    quit("no wav file");
#endif
  }
  if (channels_fake < 0) channels_fake = channels;
  if (wavcsamp_fake < 0) wavcsamp_fake = wavcsamp;
  printf("channel %d of %d\n", channel, channels_fake);;;;
  const sf_count_t c = sf_writef_short(pf, rawS16[channel], wavcsamp_fake);
  if (wavcsamp_fake != c) {
    quit("problem writing 1-channel .wav file " + channelfile);
  }
  if (0 != sf_close(pf))
    warn("failed to close wav");

  if (-1 == system(("HCopy -A -T 1 -C " + Hcfg + " " + channelfile + " " + Outfile).c_str()))
    quit("system(HCopy) failed");
  if (0 != remove(channelfile.c_str()))
    quit("failed to remove " + channelfile);
  if (0 != remove(Hcfg.c_str()))
    quit("failed to remove " + Hcfg);

  return Outfile;
}

void readHTK(const std::string& caption, const std::string& filename,
    double& period, int& vectorsize, float*& data, size_t& cz)
{
  const Mmap* foo = new Mmap(filename, false);
  const char* pch = foo->pch();
  const unsigned cch = foo->cch();
  if (!pch)
    quit("missing HTK file " + filename);
  if (cch == 0)
    quit("empty HTK file " + filename);
  if (cch < 12)
    quit("truncated header in HTK file " + filename);
  assert(4 == sizeof(float));
  assert(4 == sizeof(int));
  assert(4 == sizeof(unsigned));
  assert(2 == sizeof(short));
  // 12-byte header, 4-byte float data. www.ee.columbia.edu/ln/LabROSA/doc/HTKBook21/node58.html
  const unsigned nsamps = ntohl(*(unsigned*)pch);			pch += sizeof(unsigned);
  const unsigned period_hnsu = ntohl(*(unsigned*)pch);			pch += sizeof(unsigned);
  const unsigned short bytesPerSamp = ntohs(*(unsigned short*)pch);	pch += sizeof(unsigned short);
  const unsigned short parmkind = ntohs(*(unsigned short*)pch);		pch += sizeof(unsigned short);
  assert(pch - foo->pch() == 12);

  const short parmkind_base = parmkind & 077;
  const std::string parmkind_name[11] = {
    "WAVEFORM", "LPC", "LPREFC", "LPCEPSTRA", "LPDELCEP", "IREFC", "MFCC", "FBANK", "MELSPEC", "USER", "DISCRETE" };
  std::string parmkind_qualifiers;
  if ((parmkind &   0100) != 0) parmkind_qualifiers += "E";
  if ((parmkind &   0200) != 0) parmkind_qualifiers += "N";
  if ((parmkind &   0400) != 0) parmkind_qualifiers += "D";
  if ((parmkind &  01000) != 0) parmkind_qualifiers += "A";
  if ((parmkind &  02000) != 0) parmkind_qualifiers += "C";
  if ((parmkind & 010000) != 0) parmkind_qualifiers += "K";
  if ((parmkind &  04000) != 0) parmkind_qualifiers += "Z";
  if ((parmkind &  02000) != 0) parmkind_qualifiers += "O";
  info("parsed htk header");

  cz = (cch-12) / sizeof(float);
  data = new float[cz];
  unsigned i;
  for (i=0; i<cz; ++i)
    *(unsigned*)(data+i) = ntohl(((unsigned*)pch)[i]);
  delete foo;
  // ;; from start of function to here, move into a separate function so foo needn't be a pointer?

  const long secs = long(nsamps*period_hnsu / 1e7);
  const unsigned di = bytesPerSamp/4;
  info("sample kind " + parmkind_name[parmkind_base] + parmkind_qualifiers);
  info(to_str(nsamps) + " samples == " + to_str(secs) + " sec, " + to_str(period_hnsu) + " hnsu, " + to_str(di) + " floats/sample.");
  if (cz * sizeof(float) != nsamps*bytesPerSamp) {
    quit("header mismatched body in htk file " + filename);
  }
  if (bytesPerSamp % 4 != 0)
    quit("sample length not multiple of 4");
  if (cz != di * nsamps)
    quit("rgz size mismatch");

  info("normalizing " + caption);
  float zMin =  FLT_MAX;
  float zMax = -FLT_MAX;
  for (i=0; i<cz; ++i) {
    if (data[i] < zMin) zMin = data[i];
    if (data[i] > zMax) zMax = data[i];
  }
  float dz = zMax - zMin;
  if (dz <= 0.0) {
    // data[] is constant.
    dz = 1.0;
  }
  for (i=0; i<cz; ++i)
    data[i] = (data[i] - zMin) / dz;

  // Spectrograms (.fb filterbanks) are conventionally black on white, not white on black.
  if (filename.find("fb") != std::string::npos) {
    for (i=0; i<cz; ++i)
      data[i] = 1.0f - data[i];
  }

  period = period_hnsu/1e7;
  vectorsize = di;
}

class Feature {
  const int m_iColormap;
  const std::string m_name;
  double m_period;
  int m_vectorsize;
  float* m_data;
  size_t m_cz;

  template <class T> const T& min3(const T& a, const T& b, const T& c) const { return std::min(a, std::min(b, c)); }
  template <class T> const T& max3(const T& a, const T& b, const T& c) const { return std::max(a, std::max(b, c)); }
  template <class T> const T avg(const T& a, const T& b) { return (a+b)*0.5; }
  template <class T> const T clamp(const T& tMin, const T& t, const T& tMax) const { return std::min(std::max(tMin, t), tMax); }

public:
  Feature(int channel, int iColormap, const std::string& filename, const std::string& caption) :
    m_iColormap(iColormap),
    m_name(caption),
    m_data(NULL),
    m_cz(0)
  {
    std::cout << "Constructing feature from channel " << channel << " of kind " << iColormap << " with caption " << caption << "\n";
    const std::string htkFilename = htkFromWav(channel, m_iColormap, filename);
    readHTK(caption, htkFilename, m_period, m_vectorsize, m_data, m_cz);
    validate();
    if (0 != remove(htkFilename.c_str()))
      warn("failed to remove " + htkFilename);
  }

  Feature(int channel, const std::string& /*filename*/) :
    m_iColormap(5 /*shader*/ ),
    m_name("waveform-as-feature"),
    m_data(NULL),
    m_cz(0)
  {
    std::cout << "Constructing waveform-feature for channel " << channel << "\n";
    m_vectorsize = 50; // number of vertical texels, at most 40 to 50

    const double msec_resolution = 5.3; // 0.3 is useful, 10.0 computes mipmaps way faster during development
    const double undersample = std::max(msec_resolution*1e-3 * SR, 1.0);

    m_period = undersample/SR; // waveform's sample rate
    const int slices = int(wavcsamp/undersample);
    m_cz = slices*m_vectorsize;
    m_data = new float[m_cz](); // () means "value initialization" to all zeros, the background behind the waveform-line foreground.
    // Test pattern.  t is horizontal.  s is vertical.
    for (int t=0; t < slices; ++t) {
      double wavMin, wavMax;
      if (undersample == 1.0) {
	// When sampleFromWav() is steep, to fill in gaps in the curve,
	// set to 1.0 not just the texel for sampleFromWav(t),
	// but also texels above and below that (in the s dimension),
	// over the full span of { wav, avg(wav,wavPrev), avg(wav,wavNext) }.
	//
	// (Can't anti-alias conventionally, because shader's palette has only one value reserved for the waveform, 1.0 i.e. 127.)
	//
	// (Gaps still happen when vertically zoomed out, suppressing some rows of texels.  Avoiding that demands a 2D texturemap
	// instead of 1D.  But that would vertically smear the spectrogram behind the waveform.  Pick one defect or the other.
	// What's worse, gaps in curve or smeared spectrogram values?)
	const double wav = sampleFromWav(channel, long(t*undersample));
	const double wavPrev = sampleFromWav(channel, std::max(t-1, 0));
	const double wavNext = sampleFromWav(channel, std::min(t+1, slices-1));
	wavMin = min3(avg(wav, wavPrev), wav, avg(wav, wavNext));
	wavMax = max3(avg(wav, wavPrev), wav, avg(wav, wavNext));
      } else {
	// Keep the curve continuous by going one sample too far in each direction (the -1 and +1 in minmaxFromWav)
	minmaxFromWav(wavMin, wavMax, channel, long(t*undersample), long((t+1)*undersample));
      }
      const int sWavMin = clamp(0, int(round(wavMin*m_vectorsize)), m_vectorsize-1);
      const int sWavMax = clamp(0, int(round(wavMax*m_vectorsize)), m_vectorsize-1);

      float* pz = m_data+t*m_vectorsize;
      std::fill(pz+sWavMin, pz+sWavMax+1, 1.0f);
    }

  }

  inline void minmaxFromWav(double& zMin, double& zMax, const int channel, long tMin, long tMax) const {
    tMin = clamp(0L, tMin-1, wavcsamp);
    tMax = clamp(0L, tMax+1, wavcsamp);
    const short* ps = rawS16[channel];
    zMin = *std::min_element(ps+tMin, ps+tMax) / 65536.0 + 0.5;
    zMax = *std::max_element(ps+tMin, ps+tMax) / 65536.0 + 0.5;
    // C++11 std::minmax_element might be slightly faster, but tMax-tMin is usually tiny.
  }

  inline double sampleFromWav(const int channel, const long t) const { // return 0.0 to 1.0
    return rawS16[channel][clamp(0L, t, wavcsamp)] / 65536.0 + 0.5;
  }

  void validate() const
  {
#ifndef NDEBUG
#ifndef _MSC_VER
	  // VS2013 only got std::isnormal and std::fpclassify in July 2013:
	  // http://blogs.msdn.com/b/vcblog/archive/2013/07/19/c99-library-support-in-visual-studio-2013.aspx
    for (size_t i=0; i<m_cz; ++i) {
      if (!std::isnormal(m_data[i]) && m_data[i] != 0.0) {
	printf("Feature's float %lu of %lu is bogus: class %d, value %f\n", i, m_cz, std::fpclassify(m_data[i]), m_data[i]);
	quit("");
      }
    }
#endif
#endif
  }

  // Caller must delete[] pb and pz.
  void binarydump(char*& pb, long& cb, float*& pz, long& cz)
  {
    if (!m_data || m_cz==0)
      quit("no data for feature '" + m_name + "'");

	cb = m_name.size();
    pb = new char[cb+1];
    (void)std::copy(m_name.begin(), m_name.end(), pb);
	pb[cb] = '\0';

    cz = 4 + m_cz;
    pz = new float[cz];
    pz[0] = float(m_iColormap);
    pz[1] = float(m_period);
    pz[2] = float(m_cz) / m_vectorsize;
    pz[3] = float(m_vectorsize);
    std::copy(m_data, m_data+m_cz, pz+4);
  }
};

void marshal(const char* filename, Feature feat) {
  char* rgb; long cb; float* rgz; long cz;
  feat.binarydump(rgb, cb, rgz, cz);
  fileFromBufs(filename, rgb, cb, rgz, cz);
  delete [] rgb;
  delete [] rgz;
  // Worth making a class of rgb, cb, rgz, cz?  Then destructor could delete[] rgb and rgz.
}

#ifndef _MSC_VER
bool CopyFile(const char* filenameSrc, const char* filenameDst)
{
#if 1
  // Until mixed.wav can be converted from other formats.
  // Fully qualify path of filenameSrc, if needed.
  const std::string src = filenameSrc[0] == '/' ? filenameSrc : get_current_dir_name() + std::string("/") + filenameSrc ;
  // std::cout << "Making symlink from " << src << " to  " << filenameDst << "\n";
  (void)remove(filenameDst); // otherwise symlink() fails
  (void)!symlink(src.c_str(), filenameDst);
  return true;
#else
  const size_t cb = 4194304; // 4 MB
  char buf[cb];
  const int src = open(filenameSrc, O_RDONLY, 0);
  if (src < 0)
    return false;
  const int dst = open(filenameDst, O_WRONLY | O_CREAT, 0644);
  if (dst < 0) {
    close(src);
    return false;
  }
  size_t size;
  while ((size = read(src, buf, cb)) > 0) {
    if (write(dst, buf, size) < 0)
      break;
  }
  // Could do while (read()>0 && write()>=0), but then
  // && must be a "sequence point" so the write happens after the read.
  // Brittle and subtle, e.g. in C90 Standard section 6.3.
  close(src);
  close(dst);
  return true;
#endif
}
#endif

std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim))
    elems.push_back(item);
  return elems;
}
std::vector<std::string> split(const std::string &s, char delim = '\n') {
  std::vector<std::string> elems;
  return split(s, delim, elems);
}

// trim from start
inline std::string& ltrim(std::string &s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
  return s;
}
// trim from end
inline std::string& rtrim(std::string &s) {
  s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
  return s;
}
// trim from both ends
inline std::string& trim(std::string &s) {
  return ltrim(rtrim(s));
}

inline bool iscomment(char c) { return c == '#'; }
inline std::string& trimTrailingComment(std::string &s) {
  (void)s.erase(std::find_if(s.begin(), s.end(), iscomment), s.end());
  return s;
}

bool removeable(const std::string& s) { return s.empty() || s[0] == '#'; }
bool keyvalue(const std::string& s) { return s.find('=') != s.npos; } // lazy shortcut

int mainCore(int argc, char** argv)
{
  appname = argv[0];
  switch (argc) {
    case 3:
      configfile = argv[2];
    case 2:
      dirMarshal = argv[1];
    case 1:
      break;
    default:
      quit("Usage: " + std::string(argv[0]) + " [dirMarshal [configfile]]\n");
      return -1;
  }

  const std::string dirOriginal(getcwd(NULL, 0));
  if (chdir(dirMarshal.c_str()) != 0) {
    if (mkdir(dirMarshal.c_str(), S_IRWXU|S_IRGRP|S_IROTH) != 0) {
      quit("failed to make subdirectory " + dirMarshal + " in directory " + getcwd(NULL, 0));
    }
    if (chdir(dirMarshal.c_str()) != 0)
      quit("failed to chdir to marshal dir, moments after making it");
  }
  dirMarshal = getcwd(NULL, 0); // Fully qualify.
  if (chdir(dirOriginal.c_str()) != 0) {
    quit("failed to chdir to starting directory " + dirOriginal);
  }
#ifndef _MSC_VER
  //info("looking for " + configfile + " in directory " + get_current_dir_name());
#endif
  // Fast for files under 50KB.  Otherwise, see http://stackoverflow.com/questions/2602013/read-whole-ascii-file-into-c-stdstring .
  std::ifstream t(configfile.c_str());
  if (!t.good())
    quit("No HTK feature config file " + configfile + " in directory " + getcwd(NULL, 0));
  std::stringstream cfg;
  cfg << t.rdbuf();					// read file all at once into one string
  std::vector<std::string> lines = split(cfg.str());	// split string into lines

  for_each(lines.begin(), lines.end(), trim);		// strip whitespace
  lines.erase(std::remove_if(lines.begin(), lines.end(), removeable), lines.end()); // strip comments and blank lines
  for_each(lines.begin(), lines.end(), trimTrailingComment);

  // Parse and delete key-value pairs like "wav=foo.wav".
  std::string wavSrc;
  std::vector<std::string>::iterator it;
  for (it=lines.begin(); it!=lines.end(); ++it) {
    //info("parsing " + *it);
    std::vector<std::string> tokens = split(*it, '=');
    if (tokens.size() != 2)
      continue;
    const std::string key = trim(tokens[0]);
    const std::string value = trim(tokens[1]);
    //info("found a key = value");
    if (key == "wav") {
      if (!wavSrc.empty())
	warn("Config file " + configfile + ": duplicate wav");
      wavSrc = value;
      info("Source recording is " + wavSrc);
    }
    else if (key == "numchans") {
      numchans = atoi(value.c_str());
    }
    else
      warn("Config file " + configfile + ": key=value syntax: unrecognized key " + key);
  }
  if (wavSrc.empty())
    quit("Config file " + configfile + ": no line 'wav=/foo/bar.wav'");
  lines.erase(std::remove_if(lines.begin(), lines.end(), keyvalue), lines.end());

  if (chdir(dirMarshal.c_str()) != 0 || chdir("..") != 0)
    quit("failed to chdir to dir above marshal dir");

  const std::string suffix = wavSrc.substr(wavSrc.size()-3);
  if (channels_fake < 0) channels_fake = channels;
  if (suffix == "wav") {
    // www.mega-nerd.com/libsndfile/api.html
    SF_INFO sfinfo;
    sfinfo.format = 0;
    SNDFILE* pf = sf_open(wavSrc.c_str(), SFM_READ, &sfinfo);
    if (!pf) {
#ifdef _MSC_VER
      char buf[MAX_PATH];
      (void)GetFullPathNameA(wavSrc.c_str(), MAX_PATH, buf, NULL);
      quit(std::string("no wav file ") + std::string(buf));
#else
      quit("no wav file " + wavSrc  + " in directory " + get_current_dir_name());
#endif
    }
    SR = sfinfo.samplerate;
    //printf("%ld frames, %d SR, %d channels, %x format, %d sections, %d seekable\n",
    //  long(sfinfo.frames), sfinfo.samplerate, sfinfo.channels, sfinfo.format, sfinfo.sections, sfinfo.seekable);

    if (sfinfo.format != (SF_FORMAT_WAV | SF_FORMAT_PCM_16))
      quit(wavSrc + " doesn't have format .wav PCM 16-bit.  Sorry.");
    // todo: /usr/include/sndfile.h actually supports dozens of other file formats.  So allow those.
    // todo: convert to 16-bit 16khz, before storing in wavS16.  Without calling system("sox ..."), so it works in win32.


    // info("copying or symlinking " + wavSrc + " to " + dirMarshal + "/mixed.wav");
#ifdef too_slow_for_huge_files
    {
      std::ifstream src(wavSrc.c_str(), std::ios::binary);
      std::ofstream dst((dirMarshal + "/mixed.wav").c_str(), std::ios::binary | std::ios_base::out);
      dst << src.rdbuf();
    }
#else
#ifdef _MSC_VER
    (void)CopyFile(wavSrc.c_str(), (dirMarshal + "/mixed.wav").c_str(), false);
    // if returns zero, call GetLastError().
#else
    CopyFile(wavSrc.c_str(), (dirMarshal + "/mixed.wav").c_str());
#endif
#endif

    info("reading " + wavSrc);
    wavcsamp = long(sfinfo.frames);
    channels = sfinfo.channels;
    short* wavS16 = new short[wavcsamp * channels];
    const long cs = long(sf_readf_short(pf, wavS16, wavcsamp));
    if (cs != wavcsamp)
      quit("failed to read " + wavSrc);
    if (0 != sf_close(pf))
      warn("failed to close " + wavSrc);
    info("readed " + wavSrc);

    rawS16 = new short*[channels];
    if (channels == 1) {
      rawS16[0] = wavS16;
      // Cleverly, wavS16 gets delete[]'d eventually, as rawS16[0].
    } else {
      // De-interleave wavS16 into separate channels, 1 short at a time.
      // This is http://en.wikipedia.org/wiki/In-place_matrix_transposition .
      // Expensive and thrash-prone, thus better avoided by instead changing how memory is accessed.
      info("de-interleaving channels");
      for (unsigned i=0; i<channels; ++i) {
	short* ps = new short[wavcsamp];
	for (long j=0; j<wavcsamp; ++j)
	  ps[j] = wavS16[j*channels + i];
	rawS16[i] = ps;
      }
      info("de-interleaved channels");
      delete [] wavS16;
    }

  } else if (suffix == "rec") {
    // EDF (European Data Format, http://www.edfplus.info/specs/edf.html ) EEG recording.
    // Also http://code.google.com/p/telehealth/source/browse/lifelink/rxbox/trunk/rxbox-rc1/Local_EDFviewer/edfviewer.py?r=559
    const Mmap* foo = new Mmap(wavSrc); // might not need that pointer
    const char* pch = foo->pch();
    const off_t cch = foo->cch();
    if (cch == 0)
      quit("empty EDF file " + wavSrc);
    if (cch < 256)
      quit("truncated global header in EDF file " + wavSrc);
    //info("parsing EDF header");
    //info(std::string(pch, 256));

#ifdef _MSC_VER
#define strtof(a,b) strtol(a,b,10)
#pragma message("warning: Visual Studio doesn't support C99's strtof().")
#endif

    // Lazily ignore strtol's errno: http://stackoverflow.com/questions/194465/how-to-parse-a-string-to-an-int-in-c
    char* end;
    const long bytesInHeader        = strtol(std::string(pch+184, 8).c_str(), &end, 10); // 24320.
    long numRecords                 = strtol(std::string(pch+236, 8).c_str(), &end, 10);
    const double secondsPerRecord   = strtof(std::string(pch+244, 8).c_str(), &end);
    channels                        = strtol(std::string(pch+252, 4).c_str(), &end, 10);

    if (bytesInHeader != 256 + channels*256)
      quit("corrupt global header in EDF file " + wavSrc);
    // fseek past bytesInHeader + 1*bytesPerRecord.
    // for irec 1..nrec:
    // read shorts, from samplesPerRecord to channels.
    // save from   (irec-1)*samplesPerRecord+1   to   irec*samplesPerRecord

    // const double hours = numRecords * secondsPerRecord / 3600.0;
    // std::cerr << numRecords << " records, " << channels << " channels, " << secondsPerRecord << " sec/record, " << hours << " hours.\n";

    if (cch < bytesInHeader)
      quit("truncated per-channel header in EDF file " + wavSrc);
    pch += 256;

    pch += channels * (16+80+8+8+8+8+8+80); // For now, skip lbl trt pdq pmn pmx dmn dmx pft.
    // Samples per data record.
    // Rashly use only the first (326).   ;;;; Verify every "nsp"?
    const int samplesPerRecord = strtol(std::string(pch, 8).c_str(), &end, 10); // "nsp" in Kyle's parser.  shortsPerChannelInARecord.

    const double samplesPerSecond = samplesPerRecord / secondsPerRecord;
    const long shortsPerRecord = samplesPerRecord * channels;
    // const long bytesPerRecord = shortsPerRecord * 2; // "rsz" in Kyle's parser
    // std::cerr << "sr is " << samplesPerSecond << ".\n" << bytesPerRecord << " bytesPerRecord.\n";
    pch += channels * (8+32); // Skip "nsp".  Now pch points at the signed 16-bit data.

    info("parsing EDF body: creating .wav file");
    // off_t expectedEOF = bytesPerRecord * numRecords;
    // assert(pch - foo->pch() + expectedEOF == cch);
    // const off_t cs = expectedEOF/2;
    const short* ps = (const short*)pch; // yeah, yeah, pointer aliasing, shaddap already

    // std::cerr << "header was " << pch-foo->pch() << " bytes, body should be " << bytesPerRecord * numRecords << " bytes, cch is " << cch << "\n";

    // The specification says:
    //   Each data record contains 'duration' seconds of 'ns' signals.
    //   Each signal is represented by the header-specified number of samples.
    // In other words:
    //   After the header comes a sequence of blocks.
    //   Each block is a concatenation of "channels" sequences of shorts.

    wavcsamp = numRecords * samplesPerRecord;
    numRecords /= 1000; info("debug: reading a fraction of the data");
    wavcsamp_fake = numRecords * samplesPerRecord;

    channels_fake = 9; // channels;
    short* wavsamples = new short[wavcsamp_fake * channels_fake];

    // Interleave samples, from within each Record, into wavsamples[].
    for (long irecord=0; irecord<numRecords; ++irecord) {
      if (irecord % 20 == 0) printf("Record %6ld of %6ld expects %ld shorts as %u sequences of %d.\n", irecord, numRecords, shortsPerRecord, channels, samplesPerRecord);
      const short* psRecord = ps + irecord*shortsPerRecord;
      for (int chan = 0; chan < channels_fake; ++chan)
	std::copy(
	  psRecord + chan*wavcsamp,
	  psRecord + chan*wavcsamp + samplesPerRecord,
	  wavsamples + chan*wavcsamp_fake + irecord*samplesPerRecord);
    }

    // timeliner_run *playing* 3- or 20- or 94-channel audio is silly.
    // Must example/stereo/marshal/mixed.wav be 94-channel?
    // Can timeliner_pre *somehow* marshal 94 mipmaps and a merely mono mixed.mp3?
    // Build autoscaling into the mipmap?
    // Combine wav drawn on spectrogram into a single mipmap?

    //;;;; do this for win32 too, *constructing* mixed.wav.
    const std::string mixedfile = dirMarshal + "/mixed.wav";
    SF_INFO sfinfo;
    sfinfo.samplerate = SR = int(samplesPerSecond);
    sfinfo.channels = channels_fake;
    sfinfo.format = SF_FORMAT_WAV|SF_FORMAT_PCM_16;
    SNDFILE* pf = sf_open(mixedfile.c_str(), SFM_WRITE, &sfinfo);
    if (!pf) {
#ifdef _MSC_VER
      char buf[MAX_PATH];
      (void)GetFullPathNameA(mixedfile.c_str(), MAX_PATH, buf, NULL);
      quit(std::string("failed to create wav file ") + std::string(buf));
#else
      quit("failed to create wav file");
#endif
    }
    if (wavcsamp_fake != sf_writef_short(pf, wavsamples, wavcsamp_fake))
      quit("problem writing mixed-wav file " + mixedfile);
    if (0 != sf_close(pf))
      warn("failed to close wav file");

    info("parsing EDF body: splitting channels");
    rawS16 = new short*[channels];
    for (int j=0; j<channels_fake; ++j) {
      rawS16[j] = new short[wavcsamp_fake];
      std::copy(ps+j*wavcsamp_fake, ps+(j+1)*wavcsamp_fake, rawS16[j]);
    }

  } else {
    quit("unexpected suffix " + suffix + " of filename " + wavSrc);
  }

  int iFeature=0;
  if (chdir(dirMarshal.c_str()) != 0)
    quit("failed to chdir to marshal dir " + dirMarshal);
  if (-1 == system("rm -rf features*")) // will be deprecated, when timeliner_pre generates mipmaps directly
    info("system(rm features*) failed");
  char filename[20] = "features ";  // will be deprecated, when timeliner_pre generates mipmaps directly
  for (it=lines.begin(); it!=lines.end(); ++it) {
    std::vector<std::string> tokens = split(*it, ' ');
    if (tokens.size() < 2) {
      warn("Config file " + configfile + ": ignoring line: " + *it);
      continue;
    }
    // Parse leading index.
    const int iColormap = atoi(tokens[0].c_str());
    const std::string caption(tokens[1]);
    tokens.erase(tokens.begin(), tokens.begin()+2);
    if (iColormap < 0) {
      warn("Config file " + configfile + ": ignoring line starting with negative feature-type index: " + *it);
      continue;
    }
    for (int chan=0; chan<channels_fake; ++chan) {
      std::cout << "Constructing feature from channel " << chan << " of kind " << iColormap << " from source " << wavSrc << " with caption " << caption << "\n"; // << " and " << tokens.size() << " more args\n";
      const Feature feat(chan, iColormap, wavSrc, caption);
      filename[8] = '0' + iFeature;
      marshal(filename, feat);
      ++iFeature;
      assert(iFeature<10); // will be deprecated, when timeliner_pre generates mipmaps directly
    }
  }
  for (unsigned chan=0; chan<channels; ++chan) {
    const Feature feat(chan, wavSrc);
    filename[8] = '0' + iFeature;
    marshal(filename, feat);
    ++iFeature;
    assert(iFeature<10); // will be deprecated, when timeliner_pre generates mipmaps directly
  }

  for (unsigned i=0; i<channels; ++i) delete [] rawS16[i];
  return 0;
}

#ifdef _MSC_VER
// Entry point for the console application.
// For this to run, disable unicode: config properties, general, project defaults, character set, "not set".
int _tmain(int argc, _TCHAR* argv[])
#else
int main(int argc, char** argv)
#endif
{
  return mainCore(argc, argv);
}

