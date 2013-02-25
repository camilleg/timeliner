#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS		// for strcpy, getenv, _snprintf, fopen, fscanf
#endif

#undef DEBUG

#include "timeliner_common.h"
#include "timeliner_diagnostics.h"

#include <algorithm> 
#include <arpa/inet.h> // ntohl(), etc
#include <cassert>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <gsl/gsl_wavelet.h>
#include <iostream>
#include <sstream>
#include <vector>

#ifdef _MSC_VER
#include <windows.h>
#include <time.h>
#else
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Linux:   aptitude install libsndfile1-dev
// Windows: www.mega-nerd.com/libsndfile/ libsndfile-1.0.25-w64-setup.exe
#include <sndfile.h>

std::string dirMarshal = ".timeliner_marshal";
std::string configfile = "timeliner_config.txt";

#include "timeliner_cache.h"
#include "timeliner_util.h"

// nonstandard.
std::string itoa(const int x) {
  static char s[100];
  sprintf(s, "%d", x);
  return s;
}

void fileFromString(const std::string& outfilename, const std::string& indata) {
  std::ofstream t(outfilename.c_str());
  t.write(indata.c_str(), indata.size());
}
void fileFromBuf(const std::string& outfilename, const char* pb, const long cb) {
  std::ofstream t(outfilename.c_str());
  t.write(pb, cb);
}
void fileFromBufs(const std::string& outfilename, const char* pb, const long cb, const float* pz, const long cz) {
  std::ofstream t(outfilename.c_str());
  // Header of bytes.  Body of floats.
  t.write(pb, cb);
  t.write((const char*)pz, cz*sizeof(float));
}

short** rawS16 = NULL;
long wavcsamp = -1;

double* hamming(int w) {
  double* a = new double[w];
  for (int n=0; n<w; ++n)
    a[n] = 0.54 - 0.46 * cos(2.0*M_PI*n / (w-1.0));
  return a;
}

// Modifies rgz in-place (with htonl()).
std::string writeHTK(const std::string& outfile, float* rgz, const long cz, const unsigned floatsPerSlice, const float sampPeriodHNSU) {
  if (cz <= 0)
    quit("refused to write HTK file with no data");
  for (unsigned i=0; i<cz; ++i) {
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

std::string writeHTKWavelet(const int channel, const std::string& fileOut, const float sampPeriodHNSU) {
  const unsigned cSampWindow = 32;
  gsl_wavelet* w = gsl_wavelet_alloc(gsl_wavelet_daubechies_centered, 4);
  gsl_wavelet_workspace* workspace = gsl_wavelet_workspace_alloc(cSampWindow);
  const float sSamp = sampPeriodHNSU / 1e7;
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
}

int channels = 1;  // mono, stereo, etc
int numchans = 62; // frequency bands per filterbank, mfcc, or wavelet
const float sampPeriodF = 100000.0;
const std::string sampPeriod = "100000.0"; // hnsu, hundreds of nanoseconds, i.e. 1e-7 seconds, or decimicroseconds.
const std::string sampPeriodTimes8 = "800000.0";

const std::string Hcfg = "/tmp/timeliner_hcopy.cfg";
const std::string Outfile = "/tmp/timeliner_htk";
std::string htkFromWav(const int channel, const int iKind, const std::string& infile) {
  if (iKind < 0 || iKind >= 4)
    return infile;
  if (iKind == 3)
    return writeHTKWavelet(channel, Outfile, sampPeriodF);
  if (iKind == 2)
    warn("Quicknet (qnsfws, feacat) support nyi.");

  // Use current value of numchans, not the compile-time default.
  const std::string ConfigCommon = "\
    USEHAMMING = T\n\
    ZMEANSOURCE = T\n\
    WINDOWSIZE = " + sampPeriodTimes8 + "\n\
    NUMCHANS = " + itoa(numchans) + "\n\
    HIFREQ = " + itoa(SR/2) + "\n\
    LOFREQ = 0\n\
    SOURCEFORMAT = WAV\n\
    USEPOWER = T\n\
    TARGETRATE = " + sampPeriod + "\n\
    PREEMCOEF = 0.97\n\
    ";
  const std::string Config[3] = {
    "TARGETKIND = FBANK_Z\n",
    "TARGETKIND = MFCC_Z\nNUMCEPS = " + itoa(numchans) + "\n",
    "TARGETKIND = FBANK_D_A_Z\n"
  };
  fileFromString(Hcfg, ConfigCommon + Config[iKind]);
  (void)unlink(Outfile.c_str()); // Mere paranoia.  The file really shouldn't be there.

  const std::string channelfile = "/tmp/channel.wav";

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
  if (wavcsamp != sf_writef_short(pf, rawS16[channel], wavcsamp))
    quit("failed to write " + channelfile);
  if (0 != sf_close(pf))
    warn("failed to close wav");

  if (-1 == system(("HCopy -A -T 1 -C " + Hcfg + " " + channelfile + " " + Outfile).c_str()))
    quit("system() failed");
  if (0 != unlink(channelfile.c_str()))
    quit("failed to remove " + channelfile);
  if (0 != unlink(Hcfg.c_str()))
    quit("failed to remove " + Hcfg);

  return Outfile;
}

void readHTK(const std::string& caption, const std::string& filename,
    double& period, long& vectorsize, float*& data, size_t& cz)
{
  const Mmap* foo = new Mmap(filename);
  const char* pch = foo->pch();
  const unsigned cch = foo->cch();
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

  const long secs = nsamps*period_hnsu / 1e7;
  const unsigned di = bytesPerSamp/4;
  info("sample kind " + parmkind_name[parmkind_base] + parmkind_qualifiers);
  info(itoa(nsamps) + " samples == " + itoa(secs) + " sec, " + itoa(period_hnsu) + " hnsu, " + itoa(di) + " floats/sample.");
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
  const float dz = zMax - zMin;
  for (i=0; i<cz; ++i)
    data[i] = (data[i] - zMin) / dz;

  // Spectrograms (.fb filterbanks) are conventionally black on white, not white on black.
  if (filename.find("fb") != std::string::npos) {
    for (i=0; i<cz; ++i)
      data[i] = 1.0 - data[i];
  }

  period = period_hnsu/1e7;
  vectorsize = di;
}

class Feature {
  const int m_iColormap;
  const std::string m_name;
  double m_period;
  long m_vectorsize;
  float* m_data;
  size_t m_cz;
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
    if (0 != unlink(htkFilename.c_str()))
      warn("failed to remove " + htkFilename);
  }
  // Caller must delete[] pb and pz.
  void binarydump(char*& pb, long& cb, float*& pz, long& cz)
  {
    if (!m_data || m_cz==0)
      quit("no data for feature '" + m_name);
    cb = m_name.size() + 1;
    pb = new char[cb];
    (void)strncpy(pb, m_name.c_str(), cb);
    cz = 4 + m_cz;
    pz = new float[cz];
    pz[0] = m_iColormap;
    pz[1] = m_period;
    pz[2] = float(m_cz) / m_vectorsize;
    pz[3] = m_vectorsize;
    memcpy(pz+4, m_data, m_cz * sizeof(float));
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

int main(int argc, char** argv)
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

  const std::string dirOriginal(get_current_dir_name());
  if (chdir(dirMarshal.c_str()) != 0) {
    if (mkdir(dirMarshal.c_str(), S_IRWXU|S_IRGRP|S_IROTH) != 0) {
      quit("failed to make subdirectory " + dirMarshal + " in directory " + get_current_dir_name());
    }
    if (chdir(dirMarshal.c_str()) != 0)
      quit("failed to chdir to marshal dir, moments after making it");
  }
  dirMarshal = get_current_dir_name(); // Fully qualify.
  if (chdir(dirOriginal.c_str()) != 0) {
    quit("failed to chdir to starting directory " + dirOriginal);
  }
  //info("looking for " + configfile + " in directory " + get_current_dir_name());

  // Fast for files under 50KB.  Otherwise, see http://stackoverflow.com/questions/2602013/read-whole-ascii-file-into-c-stdstring .
  std::ifstream t(configfile.c_str());
  if (!t.good())
    quit("No HTK feature config file " + configfile);
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
      info("Source audio is " + wavSrc);
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
  //printf("%ld frames, %d SR, %d channels, %x format, %d sections, %d seekable\n",
  //  long(sfinfo.frames), sfinfo.samplerate, sfinfo.channels, sfinfo.format, sfinfo.sections, sfinfo.seekable);

  if (sfinfo.samplerate != SR)
    quit(wavSrc + " doesn't have 16 kHz sampling rate.  Sorry.");
  if (sfinfo.format != (SF_FORMAT_WAV | SF_FORMAT_PCM_16))
    quit(wavSrc + " doesn't have format .wav PCM 16-bit.  Sorry.");
  // todo: /usr/include/sndfile.h actually supports dozens of other file formats.  So allow those.
  // todo: convert to 16-bit 16khz, before storing in wavS16.  Without calling system("sox ..."), so it works in win32.
  if (-1 == system(("cp " + wavSrc + " " + dirMarshal + "/mixed.wav").c_str()))
    quit("system() failed");
  info("reading " + wavSrc);
  wavcsamp = sfinfo.frames;
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
    for (int i=0; i<channels; ++i) {
      short* ps = new short[wavcsamp];
      for (long j=0; j<wavcsamp; ++j)
	ps[j] = wavS16[j*channels + i];
      rawS16[i] = ps;
    }
    info("de-interleaved channels");
    delete [] wavS16;
  }

  int i=0;
  if (chdir(dirMarshal.c_str()) != 0)
    quit("failed to chdir to marshal dir " + dirMarshal);
  if (-1 == system("rm -rf features*")) // will be deprecated, when timeliner_pre generates mipmaps directly
    quit("system() failed");
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
    for (int chan=0; chan<channels; ++chan) {
      std::cout << "Constructing a feature from channel " << chan << " of kind " << iColormap << " from source " << wavSrc << " with caption " << caption << "\n"; // << " and " << tokens.size() << " more args\n";
      const Feature feat(chan, iColormap, wavSrc, caption);
      filename[8] = '0' + i;
      marshal(filename, feat);
      ++i;
      assert(i<10); // will be deprecated, when timeliner_pre generates mipmaps directly
    }
  }

  for (int i=0; i<channels; ++i) delete [] rawS16[i];
  return 0;
}
