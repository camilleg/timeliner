#pragma once
#include "timeliner_cache.h"
#include <string>

#include <GL/glew.h> // before gl.h
#include <GL/glut.h> // only for GLuint

class QueueElement {
public:
  unsigned char* bufByte;
  int ichunk;
  int width;
  int mipmaplevel;
  QueueElement( unsigned char* a, int b, int c, int d) :
    bufByte(a), ichunk(b), width(c), mipmaplevel(d) {}
};

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
  std::vector<Slartibartfast> rgTex;

  Feature(int /*iColormap*/, const std::string& filename, const std::string& dirname);

  void makeMipmaps();
  const void makeTextureMipmap     (const CHello& cacheHTK, int mipmaplevel, int width)             const;
  const void makeTextureMipmapChunk(const CHello& cacheHTK, int mipmaplevel, int width, int ichunk) const;
  void finishMipmap(const QueueElement&);

  bool hasGraphicsRAM() const { return mb == mbPositive; }

  void binaryload(const char* pch, long cch);

  bool fValid()      const { return m_fValid; }
  int vectorsize()   const { return m_vectorsize; }
  int samples()      const { return m_cz / m_vectorsize; }
  const char* name() const { return m_name; }

private:
  bool m_fValid;
  int m_iColormap;
  float m_period;	// seconds per sample
  int m_vectorsize;	// e.g., how many frequency bins in a spectrogram
  const float* m_pz;	// m_pz[0..m_cz] is the (vectors of) raw data
  long m_cz;
  char m_name[1000];
};
