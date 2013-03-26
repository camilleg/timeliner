#include "timeliner_diagnostics.h"
#include "timeliner_util.h"

#include <cassert>
#include <string>
#include <fcntl.h>
#ifndef _MSC_VER
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#ifdef _MSC_VER

Mmap::Mmap(const std::string& szFilename, const bool fOptional) : _pch(NULL) {
    h = CreateFile( std::wstring(szFilename.begin(), szFilename.end()).c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
      if (!fOptional)
	    warn("mmap: problem opening file" + szFilename);
      return;
    }
    h2 = CreateFileMapping(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (h2 == NULL)
      warn("mmap: problem #2 opening file" + szFilename);
    _pch = (char*)MapViewOfFile(h2, FILE_MAP_READ, 0, 0, 0);

    _cch.QuadPart = 0;
    if (GetFileSizeEx(h, &_cch) == 0)
      warn("mmap: problem measuring file" + szFilename);
}
Mmap::~Mmap() {
	if (h != INVALID_HANDLE_VALUE) {
      UnmapViewOfFile(_pch);
	  if (h2 != NULL)
		CloseHandle(h2);
	  CloseHandle(h);
	}
}

#else

Mmap::Mmap(const std::string& szFilename, bool fOptional) : _pch(NULL), _cch(0), _fd(-1) {
    _fd = open(szFilename.c_str(), O_RDONLY);
    if (_fd < 0) {
      if (!fOptional)
	warn("mmap: problem opening file" + szFilename);
      return;
    }
    struct stat s;
    const int err = fstat(_fd, &s); // EOVERFLOW possible if compiled on 32-bit
    if (err != 0) {
      warn("mmap: fstat failed");
      return;
    }
    assert(sizeof(_cch) >= 8 && sizeof(s.st_size) >= 8); // Otherwise files over 2GB will fail.
    _cch = s.st_size; // possibly 0, for an empty file
    _pch = (char*)mmap(NULL, _cch, PROT_READ, MAP_SHARED, _fd, 0);
    if (_pch == MAP_FAILED) {
      _pch = NULL;
      warn("mmap failed");
    }
}
Mmap::~Mmap() {
    if (_pch && munmap(_pch, _cch) == -1)
      warn("munmap failed");
    if (_fd >= 0)
      close(_fd);
}

#endif
