#ifdef _MSC_VER
#define NOMINMAX // Don't #define min and max in windows.h, so std::min works.
#include <windows.h>
#endif

// T is float or Float or double
template <class T> T sq(const T _) { return _*_; }
template <class T> T cb(const T _) { return _*_*_; }
template <class T> T fi(const T _) { return _*_*_*_; }

// As key goes from 0 to 1, return from a to b.
template <class T> T lerp(const T& key, const T& a, const T& b) { return key*b + (1-key)*a; }

// Memory-mapped read-only files.

class Mmap {
  char* _pch;
#ifdef _MSC_VER
  LARGE_INTEGER _cch;
  HANDLE h, h2;
#else
  off_t _cch;
  int _fd;
#endif

public:
  Mmap(const std::string& szFilename, bool fOptional=true);
  ~Mmap();
  const char*  pch() const { return _pch; }
  const bool valid() const { return _pch != NULL; } // Better would be C++11 safe-bool explicit operator bool() const;
#ifdef _MSC_VER
  const off_t cch() const { return off_t(_cch.QuadPart); }
#else
  const off_t cch() const { return _cch; }
#endif
};
