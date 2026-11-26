/*
 * Copyright (c) 2026 roleo.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * PCM File Sink
 */

#ifndef _PCM_FILE_SINK_HH
#define _PCM_FILE_SINK_HH

#include "FileSink.hh"

#define ULAW 0
#define ALAW 1

// Output gain applied by the 16 kHz oversample path (fork fix,
// 2026-11-18): linear interpolation + x2, saturating at int16 limits.
#define PCM_OUT_GAIN 2

class PCMFileSink: public FileSink {
public:
    static PCMFileSink* createNew(UsageEnvironment& env, char const* fileName,
                                  int destSampleRate, int srcLaw,
                                  unsigned bufferSize = 8192);
    // "bufferSize" should be at least as large as the largest expected
    //   input frame.

    virtual void addData(unsigned char const* data, unsigned dataSize,
                         struct timeval presentationTime);
    // NOTE (yi-hack-v5 port): Live555 2023.01.19 FileSink::addData() takes
    // "unsigned char const*"; MStar's live.2026.01.12 version takes a
    // non-const pointer. Adapted to match the 2023 base class.
    // (Available in case a client wants to add extra data to the output file)

protected:
    PCMFileSink(UsageEnvironment& env, FILE* fid, int destSampleRate, int srcLaw, unsigned bufferSize);
    // called only by createNew()
    virtual ~PCMFileSink();

protected: // redefined virtual functions:
    virtual Boolean continuePlaying();

protected:
    static void afterGettingFrame(void* clientData, unsigned frameSize,
                                  unsigned numTruncatedBytes,
                                  struct timeval presentationTime,
                                  unsigned durationInMicroseconds);
    virtual void afterGettingFrame(unsigned frameSize,
                                   unsigned numTruncatedBytes,
                                   struct timeval presentationTime);
    void writeNonBlocking(void const* buf, unsigned nbytes);

    int fDestSampleRate;
    int fSrcLaw;
    int fPacketCounter;
    int16_t *fPCMBuffer;
    int16_t fPrevSample; // previous decoded 8 kHz input sample (linear
                         // interpolation state, persists across frames)
    Boolean fOutputClosed;
};

#endif
