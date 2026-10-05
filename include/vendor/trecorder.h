// Allwinner "TRecorder" (libtrecorder.so) API, as used by video_monitor.
// Reconstructed from call sites; the callback payload layout comes from CallbackFromTRecorder (0x493ad8).
#pragma once

#include <cstdint>

extern "C"
{

  typedef void* TrecoderHandle;

  // Callback message ids seen in CallbackFromTRecorder.
  enum TRecorderCallbackMsg : int
  {
    T_RECORD_ONE_FILE_COMPLETE = 0, // a segment rolled over; caller may change output path
    T_RECORD_STREAM_FRAME = 2,      // payload: TRecorderVideoFrame*
    T_RECORD_MUX_STATUS = 3,        // payload: int* (1 = mux finished)
  };

  // Payload of T_RECORD_STREAM_FRAME. Only the fields video_monitor reads are named.
  struct TRecorderVideoFrame
  {
    uint8_t* data;        // +0x00  Annex-B H.264 access unit
    int32_t size;         // +0x08
    uint8_t pad[0x20];    // +0x0c
    int32_t is_key_frame; // +0x2c
  };

  typedef int (*TRecorderCallback)(void* user, int msg, void* payload);

  TrecoderHandle CreateTRecorder(void);
  int TRreset(TrecoderHandle h);
  int TRSetRecorderCfgPath(const char* path, int len);
  int TRsetCamera(TrecoderHandle h, int camera_index);
  int TRsetOutput(TrecoderHandle h, char* path);
  int TRchangeOutputPath(TrecoderHandle h, char* path);
  int TRsetMaxRecordTimeMs(TrecoderHandle h, int ms);
  int TRsetRecorderCallback(TrecoderHandle h, TRecorderCallback cb, void* user);
  int TRprepare(TrecoderHandle h);
  int TRstart(TrecoderHandle h, int flags);
  int TRstop(TrecoderHandle h, int flags);
  int TRrelease(TrecoderHandle h);
  int TRmux(TrecoderHandle h, int cmd); // 3 = start muxing to file, 4 = stop

} // extern "C"
