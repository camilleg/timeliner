#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "timeliner_cache.h"

#undef VERBOSE

// Computing thousands of these per frame is faster than gigabytes of trivial lookup tables (see "Shrunk").
inline Float TFromIleaf(unsigned long is, const Float hz)
{
  return (Float(is) - 0.5F) / hz;
  // Roundoff error possible, when "is" so big that is+0.5 == (is+1)+0.5.
  // Around 2e7 for Float=float, 3e26 for Float=double.
}

CHello::~CHello() {
  // Deallocate members of layers, then layers itself.
  assert(layers);
  assert(!layers->empty());
  for (std::vector<VD*>::iterator i = layers->begin(); i != layers->end(); ++i)
    delete *i;
  delete layers;
}

// Exact 200-line copypaste between float* aSrc and int* aSrc.
// Yes, float* not Float*.
CHello::CHello(const float* const aSrc, const long cs, const Float hzArg, const unsigned SUB, const int widthArg) :
  fShrunkleaves(SUB == 1),
  hz(hzArg),
  width(widthArg),
  czNode(3 + 3*width), // tMin tMax numEls, width* { zMin zMean zMax }.
  cb(0)
{
  if (SUB < 1) {
    std::cout << "error: nonpositive cache undersampler.\n";
    exit(1);
  }
  if (hz <= 0.0) {
    std::cout << "error: cache got nonpositive sample rate " << hz << ".\n";
    exit(1);
  }
  if (width < 1) {
    std::cout << "error: cache got nonpositive vector width " << width << ".\n";
    exit(1);
  }
  if (width > int(CQuartet_widthMax)) {
    std::cout << "error: recompile timeliner's timeliner_cache.h with CQuartet_widthMax >= " << width << ", not " << CQuartet_widthMax << ".\n";
    exit(1);
  }

  // If not undersampling, then omit explicit min,mean,max,numels,tMin,tMax.
  // MMM are just the value.
  // numels is 1.
  // tMin,tMax are derived from offset into array of leaves.
  layers = new std::vector<VD*>;
  assert(cs % width == 0);
  const unsigned cLeaves = cs / width / SUB;
#ifdef VERBOSE
  if (cLeaves > 500000) {
    std::cout << "Alloc " << cLeaves << " leaves";
    if (SUB > 1)
      std::cout << ", undersampled " << SUB << "x down to " << hz/SUB << " Hz.";
    std::cout << "\n";
  }
#endif
  const unsigned cz = fShrunkleaves ? cs : cLeaves*czNode;
  layers->push_back(new VD(cz, 0.0));
  cb += cz * sizeof(Float);
#ifdef VERBOSE
  if (cLeaves > 500000)
    std::cout << "Stuff " << cLeaves << " leaves.\n";
#endif

        VD::iterator pz = layers->back()->begin();
  const VD::iterator pzMax = layers->back()->end();
  // float not Float
  const float* iSrc = aSrc;
  const float* const iSrcMax = aSrc + cs;

  if (fShrunkleaves) {
    assert(long(layers->back()->size()) == cs);
    // Both tests are needed, lest pz overflow.  (Compiler bug??)
    unsigned c;
    for (c=0; iSrc != iSrcMax && pz != pzMax; ++c) {
#ifndef NDEBUG
#ifndef _MSC_VER
	    // VS2013 only got std::isnormal in July 2013:
	    // http://blogs.msdn.com/b/vcblog/archive/2013/07/19/c99-library-support-in-visual-studio-2013.aspx
		if (!std::isnormal(*iSrc) && *iSrc != 0.0) {
	printf("Warning: invalid floating-point value %f.\n", *iSrc);
      }
#endif
#endif
      *pz++ = Float(*iSrc++);
    }
    assert(c == cLeaves*width);
  } else {
    unsigned long is = 0;
    int iLeaf = 0;
    int percentPrev = 0;
    while (iSrc != iSrcMax && pz != pzMax) {
      // Compute tMin and tMax with the same expression,
      // so they're exactly binary == from one node to the next.
      *pz++ = TFromIleaf(is, hz);
      is += SUB;
      *pz++ = TFromIleaf(is, hz);

      static Float zMin[CQuartet_widthMax];
      static double zMean[CQuartet_widthMax]; // avoid roundoff error
      static Float zMax[CQuartet_widthMax];
      for (unsigned _=0; _<width; _++) {
	zMin[_] = 1e9;
	zMean[_] = 0.0;
	zMax[_] = -1e9;
      }
      unsigned j = 0;
      // Stuff zMin[], zMean[], zMax[], reading one slice at a time.
      for (; j<SUB && iSrc != iSrcMax; ++j) {
	for (unsigned _=0; _<width; ++_,++iSrc) {
	  const Float f = Float(*iSrc);
	  if (f < zMin[_]) zMin[_] = f;
	  zMean[_] += double(f);
	  if (f > zMax[_]) zMax[_] = f;
	}
      }

      // numels; width * { min, mean, max }.
      *pz++ = Float(j);
      for (unsigned _=0; _<width; ++_) {
	*pz++ = zMin[_];
	*pz++ = Float(zMean[_] / j);
	*pz++ = zMax[_];
      }

      if (cLeaves > 300000) {
	const int percent = int((100.0 * ++iLeaf) / cLeaves);
	if (percent != percentPrev || percent >= 100.0) {
	  fprintf(stderr, "Caching wav data: %2d%%...\r", percent);
	  fflush(stderr);
	  percentPrev = percent;
	}
      }
    }
    printf("                              \r");
  }

  while (layers->back()->size() > czNode) {
    const VD& L = *layers->back(); // Read previous layer.

    // A Twig is a non-leaf node.  Perhaps cNode is more readable than cTwig?

    if (fShrunkleaves && layers->size() == 1) {
      // Build first nontrivial layer.
      assert(width >= 1);
      // width==1 is possible, albeit an inefficiently small payload.
      assert(L.size() % width == 0);
      const int cTwigPrev = L.size() / width;
      const bool odd = cTwigPrev % 2 != 0;
      const int cParentOfTwoKids = (odd ? cTwigPrev-1 : cTwigPrev) / 2;
      const int cTwig = (odd ? cTwigPrev+1 : cTwigPrev) / 2;
#ifdef VERBOSE
      if (cTwig > 250000)
	std::cout << "Stuff " << cTwig << " nodes from shrunk leaves.\n";
#endif

      // Write new layer.
      const unsigned cz = cTwig*czNode;
      layers->push_back(new VD(cz, 0.0));
      cb += cz * sizeof(Float);
      VD::iterator pz = layers->back()->begin();
      unsigned long is = 0;
      unsigned j=0;
      for (int _=0; _<cParentOfTwoKids; _++, j+=2*width) {
	const unsigned k = j+width;
	// j and k index two adjacent "nodes",
	// really just two sequences of Floats in L, each of length "width".
	// Create the payload of their parent pz.

	// time interval
	*pz++ = TFromIleaf(is, hz);
	is += 2;
	*pz++ = TFromIleaf(is, hz);

	// numels; width * { min, mean, max }.
	*pz++ = 2.0;
	assert(j % width == 0);
	assert(k % width == 0);
	assert(j<k);
	for (unsigned iVector=0; iVector<width; ++iVector) {
	  assert(k+iVector < L.size());

	  const Float& z1 = L[j+iVector];
	  const Float& z2 = L[k+iVector];

	  const Float _min = *pz++ = std::min(z1,  z2);
	  const Float _mean = *pz++ = (z1 + z2) * 0.5F;
	  const Float _max = *pz++ = std::max(z1,  z2);
	  assert(0.0 <= _min); // for htk
	  assert(_min <= _mean);
	  assert(_mean <= _max);
	  assert(_max <= 1.0); // for htk
#ifdef NDEBUG
	  _unused(_min);
	  _unused(_mean);
	  _unused(_max);
#endif
	}
      }

      if (!odd) {
	assert(j==L.size());
	assert(is == cLeaves);
      } else {
	// Copy the final "node" to its parent, which gets only that single child instead of two.
	// j += 2 already happened when leaving the for-loop.
	assert(j+width == L.size());

	// time interval
	*pz++ = TFromIleaf(is, hz);
	is += 1;
	assert(is == cLeaves);
	*pz++ = TFromIleaf(is, hz);

	// numels; width * { min, mean, max }.
	*pz++ = 1.0;
	for (unsigned iVector=0; iVector<width; ++iVector) {
	  *pz++ = *pz++ = *pz++ = L[j+iVector];
	}
      }

    } else {

      assert(L.size() >= czNode);
      assert(L.size() % czNode == 0);
      const int cTwigPrev = L.size() / czNode;
      const bool odd = cTwigPrev % 2 != 0;
      const int cParentOfTwoKids = (odd ? cTwigPrev-1 : cTwigPrev) / 2;
      const int cTwig = (odd ? cTwigPrev+1 : cTwigPrev) / 2;
      if (cTwig > 2000000)
	std::cout << "Cache: stuff " << cTwig << " float nodes == " << int(cTwig*czNode*8/1.0e6) << "MB.\n";

      // Write new layer.
      layers->push_back(new VD(cTwig*czNode, 0.0));

      VD::iterator pz = layers->back()->begin();
      unsigned j=0;
      for (int _=0; _<cParentOfTwoKids; _++, j+=2*czNode) {
	const unsigned k = j+czNode;
	// j and k index two nodes at layer L.
	// Propagate their payloads to their parent.

#ifndef NDEBUG
	// +0 +1 +2, 3*{ +3 +4 +5 } is tMin, tMax, numels, min, mean, max.

	assert(k+3+3*width <= L.size());

	assert(L[j+0] < L[j+1]); // Positive-length time interval.
	assert(L[j+1] == L[k+0]); // Yes, binary exactness for floating-point.
	assert(L[k+0] < L[k+1]); // Positive-length time interval.

	assert(L[j+2] >= 1.0);    // at least 1 element
	assert(L[k+2] >= 1.0);    // at least 1 element

	const Float epsilon = 1e-5;
	{
	  for (unsigned _=0; _<width; ++_) {
	    const int __ = _*3;

#ifndef _MSC_VER
	    // VS2013 only got std::isnan in July 2013:
	    // http://blogs.msdn.com/b/vcblog/archive/2013/07/19/c99-library-support-in-visual-studio-2013.aspx
		assert(!isnan(L[j+__+3]));
	    assert(!isnan(L[j+__+4]));
	    assert(!isnan(L[j+__+5]));
#endif
	    assert(L[j+__+3] <= L[j+__+4] + epsilon); // min <= mean
	    assert(L[k+__+3] <= L[k+__+4] + epsilon); // min <= mean
	    assert(L[j+__+4] <= L[j+__+5] + epsilon); // mean <= max
	    assert(L[k+__+4] <= L[k+__+5] + epsilon); // mean <= max
	  }
	}
#endif

	// tMin, tMax
	*pz++ = L[j+0];
	*pz++ = L[k+1];

	// +2 +3 +4 +5 is numels, min, mean, max.
	// numels; width * { min, mean, max }.
	const Float _numEls = *pz++ = L[j+2] + L[k+2];
	assert(_numEls >= 1.9); // For division by zero, but even stronger than != 0.
	for (unsigned _=0; _<width; ++_) {
	  const int __ = _*3;
	  const Float _min  = *pz++ = std::min(L[j+__+3], L[k+__+3]);
	  const Float _mean = *pz++ = Float((L[j+2]*double(L[j+__+4]) + L[k+2]*double(L[k+__+4])) / _numEls);
	  const Float _max  = *pz++ = std::max(L[j+__+5], L[k+__+5]);
	  assert(_min <= _mean + epsilon);
	  assert(_mean <= _max + epsilon);
#ifdef NDEBUG
	  _unused(_min);
	  _unused(_mean);
	  _unused(_max);
#endif
	}
      }
      if (odd) {
	// Copy the final node to its parent, which gets only that single child instead of two.
	// j += 2*czNode already happened when leaving the for-loop.
	assert(j+czNode == L.size());
	for (unsigned l=0; l<czNode; ++l)
	  *pz++ = L[j+l];
      }

    }
  }
#ifndef NDEBUG
  printf("Cache of floats uses %.0f MB mobo RAM.\n", cb / float(1e6));
#endif
}

// Exact 200-line copypaste between float* aSrc and short* aSrc.
CHello::CHello(const short* const aSrc, const long cs, const Float hzArg, const unsigned SUB, const int widthArg) :
  fShrunkleaves(SUB == 1),
  hz(hzArg),
  width(widthArg),
  czNode(3 + 3*width), // tMin tMax numEls, width* { zMin zMean zMax }.
  cb(0)
{
  if (SUB < 1) {
    std::cout << "error: nonpositive cache undersampler.\n";
    exit(1);
  }
  if (hz <= 0.0) {
    std::cout << "error: cache got nonpositive sample rate " << hz << ".\n";
    exit(1);
  }
  if (width < 1) {
    std::cout << "error: cache got nonpositive vector width " << width << ".\n";
    exit(1);
  }
  if (width > int(CQuartet_widthMax)) {
    std::cout << "error: recompile timeliner's ruby extension main.cpp with CQuartet_widthMax >= " << width << ", not " << CQuartet_widthMax << ".\n";
    exit(1);
  }

  // If not undersampling, then omit explicit min,mean,max,numels,tMin,tMax.
  // MMM are just the value.
  // numels is 1.
  // tMin,tMax are derived from offset into array of leaves.
  layers = new std::vector<VD*>;
  assert(cs % width == 0);
  const unsigned cLeaves = cs / width / SUB;
#ifdef VERBOSE
  if (cLeaves > 500000) {
    std::cout << "Alloc " << cLeaves << " leaves";
    if (SUB > 1)
      std::cout << ", undersampled " << SUB << "x down to " << hz/SUB << " Hz.";
    std::cout << "\n";
  }
#endif
  const unsigned cz = fShrunkleaves ? cs : cLeaves*czNode;
  layers->push_back(new VD(cz, 0.0));
  cb += cz * sizeof(Float);
#ifdef VERBOSE
  if (cLeaves > 500000)
    std::cout << "Stuff " << cLeaves << " leaves.\n";
#endif

        VD::iterator pz = layers->back()->begin();
  const VD::iterator pzMax = layers->back()->end();
  const short* iSrc = aSrc;
  const short* const iSrcMax = aSrc + cs;

  if (fShrunkleaves) {
    assert(long(layers->back()->size()) == cs);
    unsigned c = 0;
    // Both tests are needed, lest pz overflow.  (Compiler bug??)
    while (iSrc != iSrcMax && pz != pzMax) {
      *pz++ = Float(*iSrc++);
      // This float is likely outside [0,1], but within SHRT_MIN..SHRT_MAX.
      ++c;
    }
    assert(c == cLeaves*width);
  } else {
    unsigned long is = 0;
    int iLeaf = 0;
    int percentPrev = 0;
    while (iSrc != iSrcMax && pz != pzMax) {
      // Compute tMin and tMax with the same expression,
      // so they're exactly binary == from one node to the next.
      *pz++ = TFromIleaf(is, hz);
      is += SUB;
      *pz++ = TFromIleaf(is, hz);

      static Float zMin[CQuartet_widthMax];
      static double zMean[CQuartet_widthMax]; // avoid roundoff error
      static Float zMax[CQuartet_widthMax];
      for (unsigned _=0; _<width; _++) {
	zMin[_] = 1e9;
	zMean[_] = 0.0;
	zMax[_] = -1e9;
      }
      unsigned j = 0;
      // Stuff zMin[], zMean[], zMax[], reading one slice at a time.
      for (; j<SUB && iSrc != iSrcMax; ++j) {
	for (unsigned _=0; _<width; ++_,++iSrc) {
	  const Float f = Float(*iSrc);
	  if (f < zMin[_]) zMin[_] = f;
	  zMean[_] += double(f);
	  if (f > zMax[_]) zMax[_] = f;
	}
      }

      // numels; width * { min, mean, max }.
      *pz++ = Float(j);
      for (unsigned _=0; _<width; ++_) {
	*pz++ = zMin[_];
	*pz++ = Float(zMean[_] / SUB);
	*pz++ = zMax[_];
      }

      if (cLeaves > 300000) {
	const int percent = int((100.0 * ++iLeaf) / cLeaves);
	if (percent != percentPrev || percent >= 100.0) {
	  fprintf(stderr, "Caching wav data: %2d%%...\r", percent);
	  fflush(stderr);
	  percentPrev = percent;
	}
      }
    }
    printf("                              \r");
  }

  while (layers->back()->size() > czNode) {
    const VD& L = *layers->back(); // Read previous layer.

    // A Twig is a non-leaf node.  Perhaps cNode is more readable than cTwig?

    if (fShrunkleaves && layers->size() == 1) {
      // Build first nontrivial layer.
      assert(width >= 1);
      // width==1 is possible, albeit an inefficiently small payload.
      assert(L.size() % width == 0);
      const int cTwigPrev = L.size() / width;
      const bool odd = cTwigPrev % 2 != 0;
      const int cParentOfTwoKids = (odd ? cTwigPrev-1 : cTwigPrev) / 2;
      const int cTwig = (odd ? cTwigPrev+1 : cTwigPrev) / 2;
#ifdef VERBOSE
      if (cTwig > 250000)
	std::cout << "Stuff " << cTwig << " nodes from shrunk leaves.\n";
#endif

      // Write new layer.
      const unsigned cz = cTwig*czNode;
      layers->push_back(new VD(cz, 0.0));
      cb += cz * sizeof(Float);
      VD::iterator pz = layers->back()->begin();
      unsigned long is = 0;
      unsigned j=0;
      for (int _=0; _<cParentOfTwoKids; _++, j+=2*width) {
	const unsigned k = j+width;
	// j and k index two adjacent "nodes",
	// really just two sequences of Floats in L, each of length "width".
	// Create the payload of their parent pz.

	// time interval
	*pz++ = TFromIleaf(is, hz);
	is += 2;
	*pz++ = TFromIleaf(is, hz);

	// numels; width * { min, mean, max }.
	*pz++ = 2.0;
	assert(j % width == 0);
	assert(k % width == 0);
	assert(j<k);
	for (unsigned iVector=0; iVector<width; ++iVector) {
	  assert(k+iVector < L.size());

	  const Float& z1 = L[j+iVector];
	  const Float& z2 = L[k+iVector];

	  const Float _min = *pz++ = std::min(z1,  z2);
	  const Float _mean = *pz++ = (z1 + z2) * 0.5F;
	  const Float _max = *pz++ = std::max(z1,  z2);
	  // These lie in [0,1] if this is an HTK feature,
	  // but only in [SHRT_MIN,SHRT_MAX] if this is fShrunkLeaves .wav data.
	  assert(_min <= _mean);
	  assert(_mean <= _max);
#ifdef NDEBUG
	  _unused(_min);
	  _unused(_mean);
	  _unused(_max);
#endif
	}
      }

      if (!odd) {
	assert(j==L.size());
	assert(is == cLeaves);
      } else {
	// Copy the final "node" to its parent, which gets only that single child instead of two.
	// j += 2 already happened when leaving the for-loop.
	assert(j+width == L.size());

	// time interval
	*pz++ = TFromIleaf(is, hz);
	is += 1;
	assert(is == cLeaves);
	*pz++ = TFromIleaf(is, hz);

	// numels; width * { min, mean, max }.
	*pz++ = 1.0;
	for (unsigned iVector=0; iVector<width; ++iVector) {
	  *pz++ = *pz++ = *pz++ = L[j+iVector];
	}
      }

    } else {

      assert(L.size() >= czNode);
      assert(L.size() % czNode == 0);
      const int cTwigPrev = L.size() / czNode;
      const bool odd = cTwigPrev % 2 != 0;
      const int cParentOfTwoKids = (odd ? cTwigPrev-1 : cTwigPrev) / 2;
      const int cTwig = (odd ? cTwigPrev+1 : cTwigPrev) / 2;
      if (cTwig > 2000000)
	std::cout << "Cache: stuff " << cTwig << " int nodes == " << int(cTwig*czNode*8/1.0e6) << "MB.\n";

      // Write new layer.
      layers->push_back(new VD(cTwig*czNode, 0.0));

      VD::iterator pz = layers->back()->begin();
      unsigned j=0;
      for (int _=0; _<cParentOfTwoKids; _++, j+=2*czNode) {
	const unsigned k = j+czNode;
	// j and k index two nodes at layer L.
	// Propagate their payloads to their parent.

#ifndef NDEBUG
	// +0 +1 +2, 3*{ +3 +4 +5 } is tMin, tMax, numels, min, mean, max.

	assert(k+3+3*width <= L.size());

	assert(L[j+0] < L[j+1]); // Positive-length time interval.
	assert(L[j+1] == L[k+0]); // Yes, binary exactness for floating-point.
	assert(L[k+0] < L[k+1]); // Positive-length time interval.

	assert(L[j+2] >= 1.0);    // at least 1 element
	assert(L[k+2] >= 1.0);    // at least 1 element

	{
	  for (unsigned _=0; _<width; ++_) {
	    const int __ = _*3;
	    assert(L[j+__+3] <= L[j+__+4]); // min <= mean
	    assert(L[k+__+3] <= L[k+__+4]); // min <= mean
	    assert(L[j+__+4] <= L[j+__+5]); // mean <= max
	    assert(L[k+__+4] <= L[k+__+5]); // mean <= max
	  }
	}
#endif

	// tMin, tMax
	*pz++ = L[j+0];
	*pz++ = L[k+1];

	// +2 +3 +4 +5 is numels, min, mean, max.
	// numels; width * { min, mean, max }.
	const Float _numEls = *pz++ = L[j+2] + L[k+2];
	assert(_numEls >= 1.9); // For division by zero, but even stronger than != 0.
	for (unsigned _=0; _<width; ++_) {
	  const int __ = _*3;
	  const Float _min  = *pz++ = std::min(L[j+__+3], L[k+__+3]);
	  const Float _mean = *pz++ = Float((L[j+2]*double(L[j+__+4]) + L[k+2]*double(L[k+__+4])) / _numEls);
	  const Float _max  = *pz++ = std::max(L[j+__+5], L[k+__+5]);
	  assert(_min <= _mean);
	  assert(_mean <= _max);
#ifdef NDEBUG
	  _unused(_min);
	  _unused(_mean);
	  _unused(_max);
#endif
	}
      }
      if (odd) {
	// Copy the final node to its parent, which gets only that single child instead of two.
	// j += 2*czNode already happened when leaving the for-loop.
	assert(j+czNode == L.size());
	for (unsigned l=0; l<czNode; ++l)
	  *pz++ = L[j+l];
      }

    }
  }
#ifndef NDEBUG
  printf("Cache of shorts uses %.0f MB mobo RAM.\n", cb / float(1e6));
#endif
}

static inline const CQuartet merge_for_recurse(const CQuartet& a, const CQuartet& b)
{
  // return an index to a fixed buffer of CQuartets, to avoid constructing, for speed?
  // custom const-size malloc?
  // make this a member like a += b, instead of c=a+b ?  for speed?
  if (!a) return b;
  if (!b) return a;
  assert(a.width == b.width);
  const unsigned& w = a.width;
  assert(w > 0);
  assert(a[0] >= 1.0);
  assert(b[0] >= 1.0);
  const Float numEls = a[0] + b[0];
  if (w == 1)
    return CQuartet(
      numEls,
      std::min(a[1], b[1]),
      (a[0]*a[2] + b[0]*b[2]) / numEls,
      std::max(a[3], b[3]));

  static Float t[1+CQuartet_widthMax*3];
  t[0] = numEls;
  for (unsigned i=0; i<w; ++i) {
    const unsigned di = 1+3*i;
    assert(di+2 < 1+3*w);
    // min mean max
    const Float epsilon = 1e-5;
    assert(a[di+0] <= a[di+1] + epsilon);
    assert(a[di+1] <= a[di+2] + epsilon);
    assert(b[di+0] <= b[di+1] + epsilon);
    assert(b[di+1] <= b[di+2] + epsilon);
    t[di+0] = std::min(a[di+0], b[di+0]);
    t[di+1] = Float((a[0]*double(a[di+1]) + b[0]*double(b[di+1])) / numEls); // double avoids roundoff error
    t[di+2] = std::max(a[di+2], b[di+2]);
    assert(t[di+0] <= t[di+1] + epsilon);
    assert(t[di+1] <= t[di+2] + epsilon);
  }
  return CQuartet(w, t);
}

const CQuartet CHello::recurse(const std::vector<VD*>* const layers, const Float s, const Float t, const int iLayer, const unsigned iz) const
{
  assert(iLayer >= 0);
  const bool fSpecial = fShrunkleaves && iLayer == 0;
  const VD& L = *(*layers)[iLayer];
  assert(iz % czNode == 0);
  const Float tMin = fSpecial ? TFromIleaf(iz/czNode    , hz) : L[iz  ];
  const Float tMax = fSpecial ? TFromIleaf(iz/czNode + 1, hz) : L[iz+1];
  assert(tMin <= tMax);
  // tMin == tMax is almost ok (e.g. when Float is float not double, for more than 2e7 samples),
  // but then visual artifacts happen (blocky when zoomed in).

  if (t < tMin || tMax < s) {
    // disjoint
    static const CQuartet nil;
    return nil;
  }
  const unsigned w = width;
  if (iLayer == 0) {
    // leaf
    if (fSpecial) {
      if (w == 1) {
	const Float& z = L[iz/czNode];
	return CQuartet(1.0, z, z, z);          // leaf, shrunk, scalar.  Inefficient but possible.
      }
      // Careful.  iz usually indexes a flat list of czNode-tuple floats,
      // so iz/czNode is the "pure" index.  But now we index width-tuples,
      // so multiply by w.
	  if (iz/czNode*w >= L.size())
		  return CQuartet(L, L.size()-w, w, true);		// workaround for leaf just past end of vector (why?);;;;
	  assert(iz/czNode*w < L.size());
      return CQuartet(L, iz/czNode*w, w, true); // leaf, shrunk, vector.
    }
    return CQuartet(L, iz+2, w);                // leaf, nonshrunk.  e.g., wav.
  }
  // s<t. tMin<tMax. tMin<t. s<tMax.
  if (s < tMin && tMax < t) {
    // proper subset
    return CQuartet(L, iz+2, w);
  }
  // node too wide, or partial overlap

  if (fShrunkleaves && iLayer == 1) {
    if (iz*2+czNode >= czNode * (*layers)[iLayer-1]->size()) {
      // only child
      return recurse(layers, s, t, iLayer-1, iz*2);
    }
  } else {
    if (iz*2+czNode >= (*layers)[iLayer-1]->size()) {
      // only child
      return recurse(layers, s, t, iLayer-1, iz*2);
    }
  }

  return merge_for_recurse(
    recurse(layers, s, t, iLayer-1, iz*2),
    recurse(layers, s, t, iLayer-1, iz*2+czNode));
}

// Return interleaved min and max.
// Force each max to exceed its corresponding min by at least dyMin.
void CHello::getbatch(float* r, const double t0, const double t1, const unsigned cstep, const double dyMin) const
{
  assert(dyMin > 0.0);
  double t = t0;
#if 0
  double tFail = -1.0;
#endif
  // This tMin-tLim computation is cleverer than in the sister functions, avoiding multiplies.
  // Todo: measure if it's actually faster.
  const double dt = (t1-t0) / double(cstep);
  Float tLimPrev = Float(t - 0.5 * dt);
  Float tLim = Float(t + 0.5 * dt);
  for (unsigned i=0; i<cstep; ++i, t+=dt, tLim+=dt) {
    const Float tMin = tLimPrev;
    tLimPrev = tLim;
    const CQuartet q(recurse(layers, tMin, tLim, layers->size() - 1, 0));
    Float yMin, yMax;
    if (!q) {
      // Probably [t0,t1] isn't a subinterval of the cached interval.
      // todo: test explicitly for this.
      yMin = 0.0;
      yMax = 1.0;
#if 0
      tFail = t;
#endif
    }
    else {
      yMin = q[1];
      yMax = q[3];
    }
    const Float dy = yMax - yMin;
    if (dy < dyMin) {
      // This also handles yMax < yMin.
      const Float yMid = (yMin + yMax) * 0.5F;
      yMin = Float(yMid - dyMin);
      yMax = Float(yMid + dyMin);
    }
    assert(yMax - yMin >= dyMin);
    *r++ = float(yMin);
    *r++ = float(yMax);
  }
#if 0
  if (tFail > 0.0)
    cout << "getbatch failed for cache " << iCache << " at time " << t << " in [" << t0 << ", " << t1 << "].\n";
#endif
}

// Convert rgb triple in-place to hsv.
static inline void RgbFromHsv(Float* a)
{
  const Float v = a[2];
  const Float s = a[1];
  if (s == 0.0) {
    // pure grey
    a[0] = a[1] /* = a[2] */ = v;
    return;
  }
  Float h = a[0];
  const Float hue = h * 6.0F;
  h = hue - floor(hue);
  const Float p = v * (1.0F - s);
  const Float q = v * (1.0F - s * h);
  const Float t = v  *(1.0F - s * (1.0F - h));
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

static inline Float sq(Float _)
  { return _*_; }

static inline Float lerp(Float key, Float a, Float b)
{
  // As key goes from 0 to 1, return from a to b.
  return key*b + (1.0F-key)*a;
}

// Convert minmeanmax in-place to rgb.
// Assumes min,mean,max all in [0,1].
static void RgbFromMMM(Float* a, int iColormap)
{
  // Convert min,mean,max to HSV.  Then, to RGB.
  switch (iColormap) {
    case 4: // oracle
      // max -> value.  yellow.
      a[1] = 1.0F;
      a[0] = 0.16F;
      break;
    case 3: // wavelet
      // a[2] = a[2];
      a[1] = sq(1.0F - a[1]); // pale
      a[0] = a[2];
    case 0: // feaFB filterbank
      // max -> value, darkness
      // /*a[0] = 0.0*/;
      // a[1] = 0.0/*1.0-a[2]*/;
      a[2] = (a[1] + a[2]) * 0.5F;
      a[1] = sq(1.0F - a[1]); // pale
      a[0] = 0.33F; // green
      break;
    case 1: // feaMFCC
      a[1] = sq(a[1]); // saturate only when mean is quite high
      a[0] = 0.5; // cyan
      break;
    default: // Saliency
      // Show min AND max.
	  // If max is large, show that.
	  // If min is small, show that.
	  // Value is max or min, whichever is farther from mean.
      // Value is max or min, whichever is more extreme i.e. farther from 0.5.  min, reversed.
      // "soft max" log sum exp x_i would be smoother but slower, and perhaps outside [0.5,1].
#undef BOTH_ENDS
#ifdef BOTH_ENDS
      Float value = a[2] > 1.0-a[0] ? a[2] : 1.0-a[0]; // 0.5 to 1
      a[0] = value*2.0 - 1.0;
#else
      a[0] = a[2];
#endif
      a[1] = 0.0; //;;a[0]; // sq(sq(a[0]));
      a[2] = a[0];
      a[0] = 0.0; // 0.334 - (a[0]/3.0); // monotonic gamut for hue, green to red
#if 0
      // todo: hsv = _, 1/(max-min) normalized, mean.

#if 0
      // min -> value, max-min -> saturation, max -> hue
      a[1] = a[2] - a[0];
      Float t=a[0];
      a[0]=a[2];
      a[2]=t;
      // Boost hue: when saturated, moderate value towards 0.5.
      a[2] = lerp(a[1], a[2], 0.5);
#else
      // max -> value, saturation.
    //a[0] = 1.0/6.0; // yellow
      a[0] = a[2];
      a[2] = (a[1] + a[2]) * 0.5;
      a[1] = sq(a[1]); // saturate only when mean is quite high
#endif
#endif
      
      break;
  }
  RgbFromHsv(a);
}

static Float ByteFromMMM(const Float* a, int iColormap)
{
  switch (iColormap) {
    case 4: // oracle
      return a[2]; // max
      break;
    case 3: // wavelet
      // max, and somewhat mean
      return lerp(0.8f, a[2], a[1]);
    case 0: // feaFB filterbank
      // max, and somewhat mean
      return lerp(0.6f, a[2], a[1]);
    case 1: // feaMFCC
      // max, and somewhat mean
      return lerp(0.6f, a[2], sq(a[1]));
    default: // Saliency
      return a[2];
      break;
  }
}

// Return conflated min mean max as a byte, jMax copies concatenated.
void CHello::getbatchByte(unsigned char* r, const double t0, const double t1, const int jMax, const unsigned cstep, const int iColormap) const
{
  const CQuartet* dummyCur = CQuartet::dummy(width);
  double t = t0;
  const double dt = (t1-t0) / double(cstep);
  for (unsigned i=0; i<cstep; ++i,t+=dt) {
    const Float tMin = Float(t - 0.5 * dt);
    const Float tLim = Float(t + 0.5 * dt);
    const CQuartet q(recurse(layers, tMin, tLim, layers->size() - 1, 0));
    const CQuartet& rq = q ? q : *dummyCur;
    for (int j=0; j<jMax; ++j) {
      const Float yMMM[3] = {rq[3*j+1], rq[3*j+2], rq[3*j+3]};
      r[(j*cstep+i)] = (unsigned char)( ByteFromMMM(yMMM, iColormap) *255.0);
    }
  }
}

// Return interleaved min mean max as RGB, jMax copies concatenated.
void CHello::getbatchMMM(unsigned char* r, const double t0, const double t1, const int jMax, const unsigned cstep, const int iColormap) const
{
  const CQuartet* dummyCur = CQuartet::dummy(width);
  double t = t0;
  const double dt = (t1-t0) / double(cstep);
  for (unsigned i=0; i<cstep; ++i,t+=dt) {
    const Float tMin = Float(t - 0.5 * dt);
    const Float tLim = Float(t + 0.5 * dt);
    const CQuartet q(recurse(layers, tMin, tLim, layers->size() - 1, 0));
    const CQuartet& rq = q ? q : *dummyCur;
    for (int j=0; j<jMax; ++j) {
      Float yMMM[3] = {rq[3*j+1], rq[3*j+2], rq[3*j+3]};
      RgbFromMMM(yMMM, iColormap);
      r[(j*cstep+i)*3+0] = (unsigned char)(yMMM[0]*255.0);
      r[(j*cstep+i)*3+1] = (unsigned char)(yMMM[1]*255.0);
      r[(j*cstep+i)*3+2] = (unsigned char)(yMMM[2]*255.0);
    }
  }
}

// Return interleaved min mean max, packed into a texturemap.
const unsigned char* const CHello::getbatchTextureFromVector(const double t0, const double t1, const unsigned cstep, const int iColormap, const unsigned oversample) const
{
  const unsigned cstepMax = 4096; // 1048576 /* for mipmaps - hack */; // 2048;
  const unsigned oversampleMax = 8;
  assert(cstep <= cstepMax);
  assert(oversample <= oversampleMax);
  static unsigned char scanline[CQuartet_widthMax * oversampleMax * cstepMax * 3];
    // Dimension order:     y, oversample,     x, rgb.
    // Strides are:     width, oversample, cstep,   3.

#define ITexel(y,over,x) ((((y) *oversample +(over)) *cstep +(x)) *3 +(0 /* index into rgb, unused */ ))

  const CQuartet* dummyCur = CQuartet::dummy(width);
#ifndef NDEBUG
  const unsigned cbScanline = cstep * 3 * width * oversample;
#endif

  double t = t0;
  const double dt = (t1-t0) / double(cstep); // roundoff error, causing flicker?  do it directly without accumulating?
  for (unsigned i=0; i<cstep; ++i, t+=dt) {
    const Float tMin = Float(t - 0.5 * dt);
    const Float tLim = Float(t + 0.5 * dt);
    const CQuartet q(recurse(layers, tMin, tLim, layers->size() - 1, 0));
    const CQuartet& rq = q ? q : *dummyCur;
    for (unsigned j=0; j<width; ++j) {
      Float yMMM[3] = {rq[3*j+1], rq[3*j+2], rq[3*j+3]};
      RgbFromMMM(yMMM, iColormap);
      // Bottom is j==0.  Top is j==width-1.
      // Left is i==0.  Right is i==cstep-1.
      const unsigned iTexel = ITexel(j,0,i);
      assert(iTexel + 2 <= cbScanline);
      scanline[iTexel+0] = (unsigned char)(yMMM[0]*255.0);
      scanline[iTexel+1] = (unsigned char)(yMMM[1]*255.0);
      scanline[iTexel+2] = (unsigned char)(yMMM[2]*255.0);
    }
  }
  delete dummyCur;
  const unsigned cbSlice = cstep * 3;
  for (unsigned j=0; j<width; ++j)
    for (unsigned k=0; k<oversample; ++k) {
      assert(ITexel(j,k,0) + cbSlice <= cbScanline);
      memcpy(scanline + ITexel(j,k,0), scanline + ITexel(j,0,0), cbSlice);
    }
  return scanline;
}
