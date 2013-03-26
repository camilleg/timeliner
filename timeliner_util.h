#ifdef _MSC_VER
#define NOMINMAX // Don't #define min and max in windows.h, so std::min works.
#include <windows.h>
#endif

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
