#include <vector>

#include <cassert>
#ifdef NDEBUG
#define _unused(x) ((void)x)
#endif

typedef double Float;
typedef std::vector<Float> VD;
// todo: Only tMin,tMax need double.  The rest of czNode can be float.
// So split that apart, to save memory.  Pointer aliasing, probably.
// Upgrade VD to a class, with a double tMin, double tMax, and plain old array not vector of floats.

// todo: choose this at runtime from the HTK features loaded.
const unsigned CQuartet_widthMax = 200; // 52 for filterbank, 62 for externally built saliency.  250 once made glTexImage1D fail with error 0x502.

class CQuartet {
  Float a[1+CQuartet_widthMax*3]; // numels, width*{min, mean, max}.  Not const, because C++ initializers can't handle arrays.
 public:
  const unsigned width;

  operator bool() const { return width > 0; }

  Float operator[](unsigned i) const {
    assert(width > 0);
    assert(i<1+width*3);
    return a[i];
  }

  // Construct a width-1 node explicitly.
  CQuartet(Float numels, Float _min, Float _mean, Float _max) : width(1) {
    assert(_min <= _mean);
    assert(_mean <= _max);
    a[0] = numels;
    a[1] = _min;
    a[2] = _mean;
    a[3] = _max;
  }

  Float clamp(const Float x, const Float xMin, const Float xMax) const {
    return x<xMin ? xMin : x>xMax ? xMax : x;
  }

  // Construct a node from an a[].
  CQuartet(const VD& z, unsigned i, unsigned w=1, bool fLeaf=false) : width(w) {
    assert(width <= CQuartet_widthMax);

    if (fLeaf) {
      // Construct a leaf from scratch.
      // Numels==1. Each min mean max triplet has constant value.
      a[0] = 1.0;
      for (unsigned j=0; j<width; ++j) {
	const Float mmm = clamp(z[i+j], 0.0, 1.0); // in case htk is buggered up, 120619 tenminutes.wav
	a[3*j+1] = 
	a[3*j+2] = 
	a[3*j+3] = mmm;

	//assert(0.0 <= a[3*j+1]); // for htk
	//assert(a[3*j+1] <= 1.0); // for htk
      }
      return;
    }

    // wav uses this.  Nonshrunk, width=1.
    a[0] = z[i];
    for (unsigned j=0; j<width; ++j) {
      a[3*j+1] = z[i+ 3*j+1];
      a[3*j+2] = z[i+ 3*j+2];
      a[3*j+3] = z[i+ 3*j+3];

      assert(-32769.0 <= a[3*j+1]); // for wav
      assert(a[3*j+1] <= a[3*j+2]);
      assert(a[3*j+2] <= a[3*j+3]);
      assert(a[3*j+3] <= 32769.0); // for wav
    }

  }

  CQuartet(unsigned w, const Float* rhs, bool fLeaf=false) : width(w) {
    assert(width <= CQuartet_widthMax);
    if (fLeaf) {
      // This is only for the dummy node?  rhs as Float* rather than VD& is dangerous.
      // Construct a leaf from scratch.
      // Numels==1. Each min mean max triplet has constant value.
      a[0] = 1.0;
      for (unsigned j=0; j<width; ++j) {
	a[3*j+1] =
	a[3*j+2] =
	a[3*j+3] = rhs[j];
	assert(0.0 <= a[3*j+1]); // for htk
	assert(a[3*j+1] <= 1.0); // for htk
      }
    } else {
      // Construct a node from an a[].
      memcpy(a, rhs, (1 + width*3) * sizeof(Float));
      for (unsigned j=0; j<width; ++j) {
	assert(0.0 <= a[3*j+1]); // for htk
	const Float epsilon = 1e-5;
	assert(a[3*j+1] <= a[3*j+2] + epsilon);
	assert(a[3*j+2] <= a[3*j+3] + epsilon);
	assert(a[3*j+3] <= 1.0); // for htk
      }
    }
  }

  // Nil node.
  CQuartet() : width(0) {}

  // Dummy node.
  static const CQuartet* dummy(unsigned w) {
    Float rhs[CQuartet_widthMax];
    for (unsigned i=0; i<w; ++i)
      rhs[i] = 0.5;
    return new CQuartet(w, rhs, true);
  }

};

class CHello
{
public:
  CHello(const short* const aSrc, const long cs, const Float hz, const unsigned SUB, const int width);
  CHello(const float* const aSrc, const long cs, const Float hz, const unsigned SUB, const int width);
  ~CHello();

  void getbatch(float* dst, const double t0, const double t1, const unsigned cstep, const double dyMin) const;
  void getbatchMMM(unsigned char* r, double t0, double t1, int jMax, unsigned cstep, int iColormap) const;
  void getbatchByte(unsigned char* r, double t0, double t1, int jMax, unsigned cstep, int iColormap) const;
  const unsigned char* const getbatchTextureFromVector(const double t0, const double t1, const unsigned cstep, const int iColormap, const unsigned oversample) const;

private:
  std::vector<VD*>* layers;
  const bool fShrunkleaves;
  const Float hz;
  const unsigned width;
  const unsigned czNode;
  long cb;

  const CQuartet recurse(const std::vector<VD*>* const layers, const Float s, const Float t, const int iLayer, const unsigned iz) const;
};
