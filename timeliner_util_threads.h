#pragma once
#ifdef _MSC_VER
#define NOMINMAX // Don't #define min and max in windows.h, so std::min works.
#include <windows.h>
#endif

#include <cerrno>
#include <iostream>
#include <queue>

#ifdef _MSC_VER
#include <process.h>
#else
#include <pthread.h>
#endif

// Windows version and extra features are in syzygy's language/arThread.h,
// from which this is excerpted.
class arLock {
#ifdef _MSC_VER
  HANDLE _mutex;
  bool _fOwned; // Owned by this app.  Does NOT mean "locked."
  bool _fLocked;
#else
  pthread_mutex_t _mutex;
#endif
public:

#ifdef _MSC_VER
  arLock() : _fOwned(true) {
    _mutex = CreateMutex(NULL, FALSE, NULL);
    const DWORD e = GetLastError();
    if (e == ERROR_ALREADY_EXISTS && _mutex) {
      // Another app has this.  (Only do this with global things like arLogStream.)
      _fOwned = false;
    }
	if (_mutex != NULL)
	  // success
      return;
    if (e == ERROR_ALREADY_EXISTS) {
      std::cerr << "arLock warning: CreateMutex failed (already exists).\n";
      return;
    }
    if (e == ERROR_ACCESS_DENIED) {
      std::cerr << "arLock warning: CreateMutex failed (access denied); backing off.\n";
LBackoff:
      // _mutex = OpenMutex(SYNCHRONIZE, FALSE, name);
      // Fall back to a mutex of scope "app" not "the entire PC".
      _mutex = CreateMutex(NULL, FALSE, NULL);
      if (!_mutex) {
        std::cerr << "arLock warning: failed to create mutex.\n";
      }
    }
    else if (e == ERROR_PATH_NOT_FOUND) {
      std::cerr << "arLock warning: CreateMutex failed (backslash?); backing off.\n";
      goto LBackoff;
    }
    else {
      std::cerr << "arLock warning: CreateMutex failed; backing off.\n";
      goto LBackoff;
    }
  }

  bool valid() const { return _mutex != NULL; }

  ~arLock() {
    if (_fOwned && _mutex) {
      (void)ReleaseMutex(_mutex); // paranoid
      CloseHandle(_mutex);
    }
  }

  void lock() {
    if (!valid()) return;
    const DWORD msecTimeout = 3000;
    for (;;) {
      const DWORD r = WaitForSingleObject( _mutex, msecTimeout );
      switch (r) {
      case WAIT_OBJECT_0:
	_fLocked = true;
	return;
      default:
      case WAIT_ABANDONED:
	// Another thread terminated without releasing _mutex.
	// << "arLock acquired abandoned lock.\n";
	return;
      case WAIT_TIMEOUT:
	break;
      case WAIT_FAILED:
	const DWORD e = GetLastError();
	if (e == ERROR_INVALID_HANDLE) {
	  // << "arLock warning: invalid handle.\n";
	  // _mutex is bad, so stop using it.
	  _mutex = NULL;
	  // Desperate fallback: create a fresh (unnamed) mutex.
	  _mutex = CreateMutex(NULL, FALSE, NULL);
	  if (_mutex)
	    continue;
	  // << "arLock unrecoverably failed to recreate handle.\n";
	}
	else {
	  // << "arLock internal error: GetLastError()==" << e << ".\n";
	}
	return;
      }
    }
  }
  void unlock() {
    if (!ReleaseMutex(_mutex)) {
      // << "arLock warning: failed to unlock.\n";
      CloseHandle(_mutex);
      _mutex = NULL;
    };
  }

#else

  arLock() {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);
    pthread_mutex_init(&_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
  }
  ~arLock() {
    pthread_mutex_destroy(&_mutex);
  }

  void lock() {
    // A relative time (a duration) requires pthread_mutex_timedlock_np
    // or pthread_mutex_reltimedlock_np, but Ubuntu doesn't define those.
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += 1; // Overkill.  Slower than that really shouldn't happen with only 3 threads.
    const int r = pthread_mutex_timedlock(&_mutex, &t);
    if (r != 0) {
      switch(r) {
	
	case EINVAL:    std::cerr << "EINVAL\n"; break;
	case ETIMEDOUT: std::cerr << "ETIMEDOUT\n"; break;
	case EAGAIN:    std::cerr << "EAGAIN\n"; break;
	case EDEADLK:   std::cerr << "EDEADLK\n"; break;
	default:        std::cerr << "unknown\n"; break;
      }
      quit("internal mutex error");
    }
  }
  void unlock() {
    pthread_mutex_unlock(&_mutex);
  }

#endif
};

// Implicitly unlocks when out of scope.
class arGuard {
  arLock& _l;
public:
  arGuard(arLock& l): _l(l) { _l.lock(); }
  ~arGuard() { _l.unlock(); }
};

class CHello; // timeliner_cache.h
#include "timeliner_feature.h" // class Feature

class WorkerArgs {
public:
  const Feature& _feature;
  const CHello& _cacheHTK;
  const int _mipmaplevel;
  const int _width;
  const int _ichunk;
  WorkerArgs( const Feature& feature, const CHello& cacheHTK, int mipmaplevel, int width, int ichunk ):
    _feature(feature),
    _cacheHTK(cacheHTK),
    _mipmaplevel(mipmaplevel),
    _width(width),
    _ichunk(ichunk)
    {}
  void work() const;
};

class WorkerPool {
public:
  WorkerPool();
  ~WorkerPool();
  void task(WorkerArgs*);
  bool empty() const;

private:
  // Methods are static, to be usable from pthreads.
  // Members are static for access from these static methods.
  pthread_t _queueHandler;
  pthread_t* _rgworker;
  int* _rgiWorker;
  static int _cores;
  static bool _fQuit;
  static bool* _busy;
  static const WorkerArgs** _args;
  static arLock _lock; // guards _busy and _args
  static std::queue<const WorkerArgs*> queueArgs;

  int cores();

  static int getWorker(const WorkerArgs* args);
  static void* taskWorker(void*);
  static void* workerThread(void*);
};
