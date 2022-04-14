#include "timeliner_feature.h"
#include "timeliner_diagnostics.h"
#include "timeliner_util.h"
#include "timeliner_util_threads.h"
#include <cstring>
#include <cstdio>
#include <cmath>

float gpuMBavailable()
{
  // glerror GL_OUT_OF_MEMORY ?
  // kernel: [4391985.748987] NVRM: VM: nv_vm_malloc_pages: failed to allocate contiguous memory

  // http://developer.download.nvidia.com/opengl/specs/GL_NVX_gpu_memory_info.txt
  // AMD/ATI equivalent: WGL_AMD_gpu_association wglGetGPUIDsAMD wglGetGPUIDsAMD wglGetGPUInfoAMD
  // www.opengl.org/registry/specs/ATI/meminfo.txt
#if 0
  #define GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX 0x9047
  GLint total_mem_kb = 0;
  glGetIntegerv(GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &total_mem_kb);
#endif
  #define GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
  GLint cur_avail_mem_kb = 0;
  glGetIntegerv(GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &cur_avail_mem_kb);
  return cur_avail_mem_kb/1000.0F;

  // A feature of width 65536 (almost 11 minutes at 100 samples/sec)
  // uses ~145 MB texture RAM.
  // That's 8 chunks of width 8192.
  // 1 chunk is 8192x1 texels * 2 for mipmap * 3 for GL_RGB * 62 vectorsize = 3047424 B = 2.9MB;
  // 8 chunks is 23 MB.  But 145MB is used?!  (RGBA not just RGB?)
}

Feature::Feature(int /*iColormap*/, const std::string& filename, const std::string& dirname): m_fValid(false) {
  if (mb == mbUnknown) {
    mb = gpuMBavailable() > 0.0f ? mbPositive : mbZero;
    if (!hasGraphicsRAM())
      warn("Found no dedicated graphics RAM.  Might run slowly.");
  }
  const Mmap marshaled_file(dirname + "/" + filename);
  if (!marshaled_file.valid())
    return;
  binaryload(marshaled_file.pch(), marshaled_file.cch()); // stuff many member variables
  makeMipmaps();
  m_fValid = true;
  // ~Mmap closes file
}

void Feature::binaryload(const char* pch, long cch) {
  assert(4 == sizeof(float));
  strcpy(m_name, pch);				pch += strlen(m_name);
  m_iColormap = int(*(float*)pch);		pch += sizeof(float);
  m_period = *(float*)pch;			pch += sizeof(float);
  const int slices = int(*(float*)pch);	pch += sizeof(float);
  m_vectorsize = int(*(float*)pch);		pch += sizeof(float);
  m_pz = (const float*)pch; // m_cz floats.  Not doubles.
  m_cz = (cch - long(strlen(m_name) + 4*sizeof(float))) / 4;
#ifndef NDEBUG
  printf("debugging feature: name %s, colormap %d, period %f, slices %d, width %d, cz %lu.\n", m_name, m_iColormap, m_period, slices, m_vectorsize, m_cz);
#endif
  if (m_iColormap<0 || m_period<=0.0 || slices<=0 || m_vectorsize<=0) {
    quit("binaryload: feature '" + std::string(m_name) + "' has corrupt data");
  }
  assert(slices*m_vectorsize == m_cz);
#ifdef NDEBUG
  _unused(slices);
#else

#ifndef _MSC_VER
      // VS2013 only got std::isnormal and std::fpclassify in July 2013:
      // http://blogs.msdn.com/b/vcblog/archive/2013/07/19/c99-library-support-in-visual-studio-2013.aspx
      for (int i=0; i<m_cz; ++i) {
  if (!std::isnormal(m_pz[i]) && m_pz[i] != 0.0) {
      printf("binaryload: feature's float %d of %ld is bogus: class %d, value %f\n", i, m_cz, std::fpclassify(m_pz[i]), m_pz[i]);
      quit("");
    }
  }
#endif
#endif
}

void prepTextureMipmap(const GLuint t)
{
  glBindTexture(GL_TEXTURE_1D, t);
  assert(glIsTexture(t) == GL_TRUE);
  // Prevent bleeding onto opposite edges like a torus.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glTexParameterf(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
  //needed? glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_PRIORITY, 0.99);
}
	

arLock lockQueue;
std::vector<QueueElement> queueChunk; // ;;;; rename queue to vector

void Feature::makeMipmaps() {
  // Adaptive subsample is too tricky, until I can better predict GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX.
  // (Adapt the prediction itself??  Allocate a few textures of various sizes, and measure reported GL_GPU_MEM_INFO_CURRENT_AVAILABLE_MEM_NVX.  But implement this only after getting 2 or 3 different PCs to test it on.)
  // Subsampling to coarser than 100 Hz would be pretty limiting.
  const char* pch = getenv("timeliner_zoom");
  unsigned subsample = pch ? atoi(pch) : 1;
  if (subsample < 1)
    subsample = 1;
  if (subsample > 1)
    printf("Subsampling %ux from environment variable timeliner_zoom.\n", subsample);

  const int csample = samples();

  // Smallest power of two that exceeds features' # of samples.
  unsigned width = 1;
  while (width < csample/subsample)
    width *= 2;
  //printf("feature has %d samples, for tex-chunks' width %d.\n", csample, width);

  // Minimize cchunk to conserve RAM and increase FPS.
  {
    GLint widthLim; // often 2048..8192, rarely 16384, never greater.
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &widthLim);
    assert(widthLim >= 0); // because width is unsigned
    if (width > unsigned(widthLim)) assert(width%widthLim==0);	// everything is a power of two
    cchunk = width<unsigned(widthLim) ? 1 : width/widthLim;
    //printf("width = %u, cchunk = %d, widthLim = %d\n", width, cchunk, widthLim);
    if (width >= unsigned(widthLim))
      assert(GLint(width/cchunk) == widthLim);
  }

  rgTex.resize(cchunk);
  for (int ichunk=0; ichunk<cchunk; ++ichunk) {
    glGenTextures(m_vectorsize, rgTex[ichunk].tex);
    for (int j=0; j < m_vectorsize; ++j)
      prepTextureMipmap(rgTex[ichunk].tex[j]);
  }

  const CHello cacheHTK(m_pz, m_cz, 1.0f/m_period, subsample, m_vectorsize);
  glEnable(GL_TEXTURE_1D);
  const float mb0 = hasGraphicsRAM() ? gpuMBavailable() : 0.0f;
  // One pool for ALL Features would be slightly faster, but risks running out of memory.
  // However, any worker pool at all uses more memory.
  {
    WorkerPool pool;
    for (unsigned level=0; (width/cchunk)>>level >= 1; ++level) {
      //printf("  computing feature's mipmap level %d.\n", level);
      makeTextureMipmap(cacheHTK, level, width >> level);
    }
  }
  printf("finishing %lu chunks\n\n\n", queueChunk.size());
  for (std::vector<QueueElement>::iterator it = queueChunk.begin(); it != queueChunk.end(); ++it) {
    finishMipmap(*it);
  }

  if (hasGraphicsRAM()) {
    const float mb1 = gpuMBavailable();
    printf("Feature used %.1f graphics MB;  %.0f MB remaining.\n", mb0-mb1, mb1);
    if (mb1 < 50.0) {
#ifdef _MSC_VER
      warn("Running out of graphics RAM.  Try increasing the environment variable timeliner_zoom to 15 or so.");
#else
      // Less than 50 MB free may hang X.  Mouse responsive, Xorg 100% cpu, network up, console frozen.
      // Or, the next request may "go negative", mb0-mb1<0.
      warn("Running out of graphics RAM.  Try export timeliner_zoom=15.");
#endif
    }
  }
}

extern double tShowBound[2];

const void Feature::makeTextureMipmapChunk(const CHello& cacheHTK, const int mipmaplevel, const int width, const int ichunk) const {
  unsigned char* bufByte = new unsigned char[vectorsize()*width];
  const double chunkL = ichunk     / double(cchunk); // e.g., 5/8
  const double chunkR = (ichunk+1) / double(cchunk); // e.g., 6/8

  printf("makeTextureMipmapChunk 0, vectorsize %d\n", vectorsize());
  cacheHTK.getbatchByte(bufByte,
      lerp(chunkL, tShowBound[0], tShowBound[1]),
      lerp(chunkR, tShowBound[0], tShowBound[1]),
      vectorsize(), width, m_iColormap);

  arGuard _(lockQueue);
  queueChunk.push_back( QueueElement(bufByte, ichunk, width, mipmaplevel) );
}

#include <GL/glx.h>
#include <X11/Xlib.h>

// Multithreaded OpenGL is tricky, brittle, poorly documented.
// So we call OpenGL not from the worker pool but only afterwards.
void Feature::finishMipmap(const QueueElement& arg) {
  for (int j=0; j<vectorsize(); ++j) {
    printf("finishMipmap\n");
    //;;;;FAILS assert(glIsTexture(rgTex[arg.ichunk].tex[j]) == GL_TRUE);
    glBindTexture(GL_TEXTURE_1D, rgTex[arg.ichunk].tex[j]);
    glTexImage1D(GL_TEXTURE_1D, arg.mipmaplevel, GL_INTENSITY8, arg.width, 0, GL_RED, GL_UNSIGNED_BYTE, arg.bufByte + arg.width*j);
  }
  //;;;;?last one FAILS    delete [] arg.bufByte;
}

extern WorkerPool* pool;
const void Feature::makeTextureMipmap(const CHello& cacheHTK, const int mipmaplevel, int width) const {
  assert(vectorsize() <= vecLim);
  assert(width % cchunk == 0);
  width /= cchunk;

  //if (mipmaplevel<3) printf("    Computing %d mipmaps of width %d.\n", cchunk, width);
  //printf("vectorsize %d\n", vectorsize());
#if 1
  for (int ichunk=0; ichunk<cchunk; ++ichunk) {
    pool->task(new WorkerArgs(*this, cacheHTK, mipmaplevel, width, ichunk));
  }
#else
  unsigned char* bufByte = new unsigned char[vectorsize()*width];
  for (int ichunk=0; ichunk<cchunk; ++ichunk) {
    const double chunkL = ichunk     / double(cchunk); // e.g., 5/8
    const double chunkR = (ichunk+1) / double(cchunk); // e.g., 6/8
  printf("makeTextureMipmapChunk 0, vectorsize %d\n", vectorsize());
    cacheHTK.getbatchByte(bufByte,
	lerp(chunkL, tShowBound[0], tShowBound[1]),
	lerp(chunkR, tShowBound[0], tShowBound[1]),
	vectorsize(), width, m_iColormap);
    for (int j=0; j<vectorsize(); ++j) {
      assert(glIsTexture(rgTex[ichunk].tex[j]) == GL_TRUE);
      glBindTexture(GL_TEXTURE_1D, rgTex[ichunk].tex[j]);
      glTexImage1D(GL_TEXTURE_1D, mipmaplevel, GL_INTENSITY8, width, 0, GL_RED, GL_UNSIGNED_BYTE, bufByte + width*j);
    }
  }
  delete [] bufByte;
#endif
}
