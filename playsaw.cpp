#include "RtAudio.h"
#include <iostream>
#include <cassert>
#include <cstdlib>

#include <Windows.h>
#include <WinBase.h> // for CRITICAL_SECTION
CRITICAL_SECTION critsecStore;
LPCRITICAL_SECTION critsec = &critsecStore;
bool fCritsec = false;

typedef signed short MY_TYPE;
#define FORMAT RTAUDIO_SINT16
#define SCALE  (32767.0)

#if 0
#if defined( __WINDOWS_ASIO__ ) || defined( __WINDOWS_DS__ )
  #include <windows.h>
  #define SLEEP( milliseconds ) Sleep( (DWORD) milliseconds ) 
#else // Unix variants
  #include <unistd.h>
  #define SLEEP( milliseconds ) usleep( (unsigned long) (milliseconds * 1000.0) )
#endif
#endif

bool checkCount = false;
const unsigned int channels = 1;
int actualSize = 256; // probably close to 256

// critsec guards these three.
static short vps[16000]; // One full second's worth of buffer.  Normally filled to at most 0.1 seconds.
static unsigned vis = 0;
static unsigned visMax = 0;

// Producer of vps, called by samplereader.
// Grows visMax (the "high water mark" inside vps[]).
void rtaudioTick(const short* ps, int cb)
{
	assert(cb % 2 == 0);
	if (fCritsec) EnterCriticalSection(critsec);
	assert(vis <= visMax);
    if (vis < visMax) {
      // Shove partial buffer back to start of array (slower than circular buffer, oh well).
      memmove(vps, vps+vis, (visMax-vis)*2);
      visMax -= vis;
      // Append fresh samples to not-yet-played samples.
    }
    else
    {
	  // Buffer was already drained.
      visMax = 0;
    }
	vis = 0;
	memcpy(vps+visMax, ps, cb);
  //printf("producing %d\n", cb/2);;;;
	visMax += cb/2;
	if (visMax >= sizeof(vps)/sizeof(vps[0]))
	{
		printf("\n\toverflow averted, but timeliner will crash!\n\n");
		visMax = sizeof(vps)/sizeof(vps[0]) - 1;
	}
    assert(visMax < sizeof(vps)/sizeof(vps[0]));
	if (fCritsec) LeaveCriticalSection(critsec);
  Sleep(14); // emulate blocking write of 16 msec, that rtaudio abandoned some time after v3.0.1 in 2004 (as used by Audacity).
}

// Called only by saw().
inline short nextSample()
{
	if (vis < visMax)
		return vps[vis++];
	// buffer drained
	vis = 0;
	visMax = 0;
	return 0;
}

extern void kickProducer(int isRequest);

// Consumer of vps, via nextSample.  Increments vis by actualSize, towards visMax.
// Data stored in buffer is interleaved between channels
int rtaudioPlayCallback( void *outputBuffer, void * /*inputBuffer*/,
		 unsigned int nBufferFrames,
         double /*streamTime*/,
		 RtAudioStreamStatus status,
		 void * /*data*/ )
{
  actualSize = nBufferFrames;
  if ( status )
    std::cout << "RtAudio underflow.\n";
  MY_TYPE *buffer = (MY_TYPE *) outputBuffer;

  if (fCritsec) EnterCriticalSection(critsec);
//  if (visMax-vis < nBufferFrames)
//  {
//	printf("\t\t\t\t\t\tWithin one buf of underflow\n");;;;  // at "low water mark" (?)
//	kickProducer(nBufferFrames);
//  }
  //printf("\t\t\t\tconsuming %d\n", nBufferFrames);;;;
  for ( unsigned i=0; i<nBufferFrames; ++i ) {
#ifndef testsignal
	  assert(channels == 1);
	  *buffer++ = nextSample();
#else
    #define lastValues ((double *) data)
	for ( unsigned j=0; j<channels; ++j ) {
      *buffer++ = (MY_TYPE) (lastValues[j] * SCALE * 0.05);
	  // update sawtooth test-signal
	  const double BASE_RATE = 0.04;
      lastValues[j] += BASE_RATE * (j+1+(j*0.1));
      if ( lastValues[j] >= 1.0 ) lastValues[j] -= 2.0;
    }
#endif
  }
  if (fCritsec) LeaveCriticalSection(critsec);
  return 0; // 1 would make RtAudio terminate.
}

int rtaudioBuf() { return actualSize; }

RtAudio dac;
double *data = NULL;

void rtaudioInit()
{
	printf("Includes RtAudio software, copyright 2012 Gary P. Scavone.\n");

  if ( dac.getDeviceCount() < 1 ) {
    std::cout << "\nNo audio devices found!\n";
    exit( 1 );
  }

  const unsigned int fs = 16000/*sampling rate*/;
  data = (double *) calloc( channels, sizeof( double ) );

  // print messages to stderr.
  dac.showWarnings( true );

  RtAudio::StreamParameters oParams;
  oParams.deviceId = 0;
  oParams.nChannels = channels;
  oParams.firstChannel = 0;
  unsigned int bufferFrames = 256;

  RtAudio::StreamOptions options;
  options.flags = 0; //RTAUDIO_HOG_DEVICE;
  //options.flags |= RTAUDIO_SCHEDULE_REALTIME;
  try {
    dac.openStream( &oParams, NULL, FORMAT, fs, &bufferFrames, &rtaudioPlayCallback, (void *)data, &options );
    dac.startStream();
  }
  catch ( RtError& e ) {
    e.printMessage();
  }
	fCritsec = InitializeCriticalSectionAndSpinCount(critsec, 0x00000400) == TRUE;
	if (!fCritsec)
		printf("failed to init critsec.  Misbehavior likely.\n");
}

void rtaudioTerm()
{
  if ( dac.isStreamOpen() ) dac.closeStream();
  free( data );
}

#if 0
void rtaudioPause(bool f)
{
	std::cout << "rtaudioPause " << f << "\n";
	if (f /*&& dac.isStreamRunning()*/)
		dac.stopStream(); // drain buffer (instead of abortStream, I think)
	if (!f /*&& !dac.isStreamRunning()*/)
		dac.startStream();
}
#endif
