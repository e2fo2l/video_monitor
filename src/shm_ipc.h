// SysV shm + semaphore channel to ava ("ShmMsg"). Linked in but never used by the shipped
// binary (intercom audio goes through Agora instead). Keys are procID << 16.
#pragma once

#include <queue>

#include "monitor_common.h"

class ShmSegment
{
public:
  explicit ShmSegment(unsigned char procId)
      : m_procId(procId)
  {
  }
  virtual ~ShmSegment();
  virtual bool OpenOrCreateSegment(unsigned long size);
  virtual void* GetSegmentPointer() { return m_addr; }
  int OpenOnlySegment() const;
  static long GetSegmentKey(unsigned char procId) { return static_cast<long>(procId) << 16; }
  bool CheckShmExist() const;

private:
  unsigned char m_procId; // +0x08
  long m_key = 0;         // +0x10
  int m_segId = -1;       // +0x18
  void* m_addr = nullptr; // +0x20
  bool m_existed = false; // +0x28 segment was already created by ava
};

class ShmSemaphore
{
public:
  explicit ShmSemaphore(unsigned char procId)
      : m_procId(procId)
  {
    m_semId = GetSemID(procId);
  }
  ~ShmSemaphore() { DisenableSem(); }
  bool EnableSem() const;
  void DisenableSem() {}
  bool GetLock() const;
  bool Unlock() const;
  int GetSemID(unsigned char procId);

private:
  int m_semId;            // +0x00
  unsigned char m_procId; // +0x04
};

// Segment layout: [0]=talk flag, [1]=frame count, [2..] frames of 0x280 bytes.
class ShmMsg
{
public:
  ShmMsg() = default;
  ~ShmMsg();
  bool Init();
  bool IsTalk();
  unsigned char GetFrameCnt();
  bool GetData(AudioDataFromAgora& out);
  bool SetTalk(bool talk);
  bool SetFrameCnt(unsigned char cnt);

private:
  ShmSegment* m_segment = nullptr;         // +0x00
  unsigned char* m_addr = nullptr;         // +0x08
  ShmSemaphore* m_sem = nullptr;           // +0x10
  std::queue<AudioDataFromAgora> m_frames; // +0x18
};
