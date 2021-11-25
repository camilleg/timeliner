#include "timeliner_diagnostics.h"
#include "timeliner_cache.h"
#include "timeliner_util_threads.h"
#include "timeliner_util.h"

#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

// Pool of worker threads, e.g. for computing mipmaps.

int WorkerPool::cores()
{
  //return 1; // for debugging
  // POSIX:
#if defined(_SC_NPROC_ONLN)
  return sysconf(_SC_NPROC_ONLN);
#elif defined(_SC_NPROCESSORS_ONLN)
  return sysconf(_SC_NPROCESSORS_ONLN);
#else
  return 1;
#endif
  // Alternatives:
  // gcc's get_nprocs(),
  // http://stackoverflow.com/questions/12483399/getting-number-of-cores-not-ht-threads
}

bool WorkerPool::empty() const {
  if (!queueArgs.empty())
    return false;
  arGuard _(_lock);
  for (int i=0; i<_cores; ++i)
    if (_busy[i])
      return false;
  return true;
}

const int usecSleepMax = 50000;

void WorkerArgs::work() const {
  _feature.makeTextureMipmapChunk(_cacheHTK, _mipmaplevel, _width, _ichunk);
}

void* WorkerPool::workerThread(void* pv) {
  const int i = *(int*)pv;
  bool& fBusy = _busy[i];
  { arGuard _(_lock); fBusy = false; } // Report myself as available.

  while (!_fQuit) {
    usleep(usecSleepMax);
    bool f; { arGuard _(_lock); f = fBusy; }
    if (f) {
      // placeholder to run the task
      const WorkerArgs* args = _args[i];
      printf("worker %d starting task %d %d\n", i, args->_feature.vectorsize(), args->_width);
      args->work();
      //printf("worker %d finished task %d %d\n", i, args->_vectorsize, args->_width);
      delete args;
      { arGuard _(_lock); fBusy = false; } // Report myself as available.
    }
    if (fBusy)
      printf("worker %d just became busy\n", i);
    else
      printf("worker %d idle\n", i);
  }
  return NULL;
}

int WorkerPool::getWorker(const WorkerArgs* args) {
  arGuard _(_lock);
  for (int i=0; i<_cores; ++i) {
    if (!_busy[i]) {
      _busy[i] = true;
      // Give the worker this task.
      _args[i] = args;
      printf("recruited worker %d\n", i);
      return i;
    }
  }
  return -1;
}

void* WorkerPool::taskWorker(void*) {
  while (!_fQuit) {
    if (queueArgs.empty()) {
      usleep(usecSleepMax/5);
    } else {
      const WorkerArgs* args = queueArgs.front();
      queueArgs.pop();
      printf("\t\t\t\tsize %lu;\t\tawaiting idle worker for %d %d\n", queueArgs.size(), args->_feature.vectorsize(), args->_width);
      // Wait for a worker for task "args".
      while (getWorker(args) < 0)
	usleep(usecSleepMax/5);
    }
  }
  return NULL;
}

// Assign tasks to workers in order received, for better memory locality.
void WorkerPool::task(WorkerArgs* args) {
  queueArgs.push(args);
  printf("\t\t\t\tsize %lu;\t\tqueued task %d %d\n", queueArgs.size(), args->_feature.vectorsize(), args->_width);
}

WorkerPool::WorkerPool()
{
  // Create _cores threads.
  _fQuit = false;
  _cores = cores();
  _rgworker = new pthread_t[_cores];
  _rgiWorker = new int[_cores];
  _args = new const WorkerArgs*[_cores];
  _busy = new bool[_cores];
  for (int i=0; i<_cores; ++i) {
    _rgiWorker[i] = i;
    if (0 != pthread_create(&_rgworker[i] , NULL, &workerThread, &_rgiWorker[i]))
      quit("failed to create pool of worker threads");
  }
  if (0 != pthread_create(&_queueHandler, NULL, &taskWorker, NULL))
    quit("failed to init worker thread queue-handler");
}

WorkerPool::~WorkerPool()
{
  while (!empty())
    usleep(usecSleepMax/2);
  // Workers are idle and queue is empty.
  _fQuit = true;
  for (int i=0; i<_cores; ++i)
    (void)pthread_cancel(_rgworker[i]);
  (void)pthread_cancel(_queueHandler);
  usleep(usecSleepMax*2); // give threads time to notice _fQuit, in case pthread_cancel() failed
  delete [] _rgworker;
  delete [] _rgiWorker;
  delete [] _args;
  delete [] _busy;
}

arLock WorkerPool::_lock;
int WorkerPool::_cores = -1;
bool WorkerPool::_fQuit = false;
bool* WorkerPool::_busy = NULL;
const WorkerArgs** WorkerPool::_args = NULL;
std::queue<const WorkerArgs*> WorkerPool::queueArgs;
