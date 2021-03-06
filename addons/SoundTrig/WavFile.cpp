
#include "WavFile.h"
#include <fstream>
#include "IntTypes.h"
#include <string.h>
#include "libresample/libresample.h"

namespace 
{
  bool isBigEndian();
  int16 LE(int16);
  int32 LE(int32);
  uint32 LE(uint32 x) { return LE(*reinterpret_cast<int32 *>(&x)); }
  uint16 LE(uint16 x) { return LE(*reinterpret_cast<int16 *>(&x)); }
  int16 fromLE(int16);
  int32 fromLE(int32);
  uint32 fromLE(uint32 x) { return fromLE(*reinterpret_cast<int32 *>(&x)); }
  uint16 fromLE(uint16 x) { return fromLE(*reinterpret_cast<int16 *>(&x)); }
}

struct OWavFile::Impl
{
  Impl() : nframes(0), bitspersample(0), srate(0), nchans(0) {}
  unsigned nframes, bitspersample, srate, nchans;
  static const unsigned dataSizeOffset = 40, fileSizeOffset = 4;
  std::ofstream f;
};

// some properties passed in at creation time
unsigned OWavFile::srate() const { return p->srate; }
unsigned OWavFile::nchans() const { return p->nchans; }
unsigned OWavFile::bits() const { return p->bitspersample; }
unsigned OWavFile::sizeBytes() const { return sampSizeBytes()*sizeSamps(); }
unsigned OWavFile::sizeSamps() const { return p->nframes; }
unsigned OWavFile::sampSizeBytes() const { return nchans()*(bits()/8); }


struct Header {
  char riff[4]; // 'RIFF'
  uint32 fileLength; // size of rest of file not including first 8 bytes
  char wave[4]; // 'WAVE'

  Header() { memcpy(riff, "RIFF", 4); memcpy(wave, "WAVE", 4); fileLength = LE(0); }
};

struct FormatChunk {
  char           chunkID[4]; // not zero terminated!, should be "fmt "
  int32          chunkSize; // does not include first 8 bytes used by first 2 fields.. refers to size of remaining fields and should be set to 16 for wFormatTag = 1

  int16          wFormatTag; // set it to 1
  uint16         wChannels; 
  uint32         dwSamplesPerSec; /* samplerate in Hz */
  uint32         dwAvgBytesPerSec; /* = wBlockAlign * dwSamplesPerSec */
  uint16         wBlockAlign; // = wChannels * (wBitsPerSample / 8)
  uint16         wBitsPerSample; // standard rates are 11025 22050 and 44100
/* Note: there may be additional fields here, depending upon wFormatTag. */

  FormatChunk() 
  { 
    memcpy(chunkID, "fmt ", 4); 
    chunkSize = LE(16); 
    wFormatTag = LE(1);   
    dwSamplesPerSec = dwAvgBytesPerSec = wBlockAlign = wBitsPerSample = wChannels = 0;
  }
};

struct DataChunk
{
  char          chunkID[4];
  int32         chunkSize; // does not include the first 8 bytes
  // data goes here...
  DataChunk() { memcpy(chunkID, "data", 4); chunkSize = LE(0); }
};

OWavFile::OWavFile()
{
  p = new Impl;
}

OWavFile::~OWavFile()
{
  close();
  delete p;
  p = 0;
}

bool OWavFile::isOpen() const
{
  return p->f.is_open();
}

bool OWavFile::isOk() const
{
  return isOpen() && p->f.good();
}

void OWavFile::close()
{
  if (isOpen()) {
    // finalize -- commit the file size and data size to the proper offsets
    uint32 dataBytes = sizeBytes(), tmp;
    p->f.seekp(Impl::dataSizeOffset);
    tmp = LE(dataBytes);
    p->f.write((const char *)&tmp, sizeof(tmp));
    p->f.seekp(Impl::fileSizeOffset);
    tmp = LE(uint32(dataBytes + sizeof(DataChunk) + sizeof(FormatChunk) + sizeof(Header) - sizeof(int32)*2));
    p->f.write((const char *)&tmp, sizeof(tmp));    
    p->f.close();
  }
}

bool OWavFile::create(const char *filename, unsigned n_chans, unsigned bits_per_sample, unsigned srate)
{
  if (srate != 44100 && srate != 22050 && srate != 11025) return false; // invalid sample rate
  if (bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) return false;
  if (n_chans < 1 || n_chans > 2) return false;
  p->f.open(filename, std::ios_base::out|std::ios_base::trunc|std::ios_base::binary);
  if (!p->f.is_open()) return false;
  p->srate = srate;
  p->nchans = n_chans;
  p->bitspersample = bits_per_sample;
  Header h;
  FormatChunk f;
  DataChunk d;
  f.wChannels = LE(n_chans);
  f.dwSamplesPerSec = LE(srate);
  f.wBlockAlign = LE(n_chans * bits_per_sample/8);
  f.dwAvgBytesPerSec = LE(srate * n_chans * bits_per_sample/8);
  f.wBitsPerSample = LE(bits_per_sample);
  p->f.write((const char *)&h, sizeof(Header));
  p->f.write((const char *)&f, sizeof(FormatChunk));
  p->f.write((const char *)&d, sizeof(DataChunk));
  return isOk();
}

namespace 
{
  inline double Round(double d) 
  { 
      if (d < 0.) 
          return double(unsigned(d-0.5)); // round away from 0 for negative
      return double(unsigned(d+0.5)); // round away from 0 for positive
  }

  inline double Ceil(double d) 
  { 
      double c = static_cast<double>(static_cast<long int>(d));
      if (c < d) c += 1.0;
      return c;
  }

  inline double Floor(double d) 
  { 
      double c = static_cast<double>(static_cast<long int>(d));
      if (c > d) c -= 1.0;
      return c;
  }
}

// NB: when writing data, channels should be interleaved as in sample0{chan0,chan1},sample1{chan0,chan1} etc..
bool OWavFile::write(const double *data, unsigned size, unsigned srate, double scale_min, double scale_max)
{
    unsigned nwrit = 0, nframes = size/p->nchans; // nb: nframes gets changed below if we resample
    double *data_ptr = 0; // if non-null, points to resampled interleaved data -- gets deleted

  // NB: need to upsample/downsample here.. use libresample and resample to another memory buffer
  if (srate != p->srate) {
    int min_frames = -1;
    double factor = double(p->srate)/double(srate);
    int n_in = nframes, space_out = int(Ceil(nframes*factor + factor) + 1.);
    double *ddata = data_ptr = new double[space_out * p->nchans];
    memset(ddata, 0, sizeof(double)*(space_out * p->nchans));
    float *data_in = new float[n_in];
    float *data_out = new float[space_out];

    for (unsigned ch = 0; ch < p->nchans; ++ch) {
        for (unsigned frame = 0; frame < nframes; ++frame) {
            // deinterleave data and convert to float
            data_in[frame] = data[frame*p->nchans + ch];
        }
        int in_used = 0;
        void *h = resample_open(1, 1/25., 25.);
        int ret = resample_process(h, factor, data_in, n_in, 1, &in_used, data_out, space_out);
        resample_close(h);
        if (ret > -1 && ( ret < min_frames || min_frames < 0 ) ) 
            min_frames = ret;
        double mind = 2e9, maxd = -2e9;  
        for (int frame = 0; frame < ret; ++frame) {
            double d = data_out[frame];
            ddata[frame*p->nchans + ch] = d;
            // figure out data max and min for normalization phase below..
            if (mind > d) mind = d;
            if (maxd < d) maxd = d;
        }
        // now normalize data
        for (int frame = 0; frame < ret; ++frame) {
            double & d = ddata[frame*p->nchans + ch];
            d = ((d-mind)/(maxd-mind)) * (scale_max-scale_min) + scale_min;
        }
    }
    delete [] data_in;
    delete [] data_out;
    data = ddata;
    nframes = min_frames > -1 ? min_frames : 0;
  }

  for (unsigned frame = 0; frame < nframes; ++frame, ++nwrit) {
    for (unsigned chan = 0; chan < p->nchans; ++chan) {
      unsigned idx = frame*p->nchans+chan;
      double datum = data[idx];
      uint32 samp = uint32((datum-scale_min)/(scale_max-scale_min) * double(0xfffffffaU) + 1); // 32-bit unsigned sample -- note we never use full 32-bit range because we were getting clipping errors sometimes probably due to floating point inaccuracies?
      if (p->bitspersample != 8) {
        // argh, deal with signed PCM
        samp -= 0x7fffffff;
      }
      samp = samp >> (32-p->bitspersample); // take the upper BitsPerSample bits
      samp = LE(samp); // make sure it's little endian so that the write command below 1. succeeds on all platforms (we write the first bitspersaple bits) and 2. wave files are anyway little endian
      p->f.write((const char *)&samp, p->bitspersample/8);
    }
    if (!p->f.good()) break;
  }
  p->nframes += nwrit;
  if (data_ptr) delete [] data_ptr;
  return isOk();
}

bool OWavFile::write(const void *v,
                     unsigned size, 
                     unsigned bits,
                     unsigned srate)
{
  const uint8 *d = static_cast<const uint8 *>(v);  
  double scale_min, scale_range;

  switch (bits) {
    case 8: scale_min = 0., scale_range = 256.; break;
    case 16: scale_min = -32768., scale_range = 65536.; break;
    case 24: scale_min = -8388608., scale_range = 16777216.; break;
    case 32: scale_min = -2147483648., scale_range = 4294967296.; break;
    default: return false;
  }
  // convert to doubles in memory, then pass to the other write function
  double *dbls = new double[size];
  for (unsigned i = 0; i < size; ++i) {
      double & datum = dbls[i];
      switch (bits) {
      case 8:  datum = d[i]; break;
      case 16: datum = ((int16 *)d)[i]; break;
      case 24: {
          uint32 samp = 0;
          memcpy(&samp, d+i*3, 3);
          if (isBigEndian()) samp >>= 8;
          datum = samp;
      }
          break;
      case 32: datum = ((int32 *)d)[i];  break;
      }
      datum = (datum-scale_min)/scale_range;
  }
  bool ret = write(dbls, size, srate, 0., 1.0);
  delete [] dbls;
  return ret;
}

namespace 
{
  bool isBigEndian()
  {
    static const uint32 dword = 0x12345678;
    static const uint8 * dp = reinterpret_cast<const uint8 *>(&dword);
    // little endian machines store the dword as 0x78563412
    // big endian store it as written by humans MSB first
    return dp[0] == 0x12;
  }
  int16 LE(int16 x) 
  {   
    if (isBigEndian()) {
      int8 * p = (int8 *)&x;
      p[0] ^= p[1]; // swap the two bytes
      p[1] ^= p[0];
      p[0] ^= p[1];      
    }
    return x;
  }
  int32 LE(int32 x)
  {
    if (isBigEndian()) {
      int8 * p = (int8 *)&x;
      p[0] ^= p[3]; // swap the two end bytes
      p[3] ^= p[0];
      p[0] ^= p[3];      

      p[1] ^= p[2]; // swap the two middle bytes
      p[2] ^= p[1];
      p[1] ^= p[2];      
    }
    return x;    
  }

  int16 fromLE(int16 x) { return LE(x); }
  int32 fromLE(int32 x) { return LE(x); }
}
