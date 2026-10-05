#include "shm_ipc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/types.h>

#include "monitor_common.h"
#include "ms_log.h"

namespace
{
constexpr unsigned char kAvaProcId = 0x7f;
constexpr unsigned long kSegSize = 0x7d02; // 2 + 50 * 0x280
constexpr size_t kFrameSize = 0x280;

union semun
{
  int val;
  semid_ds* buf;
  unsigned short* array;
};
} // namespace

// ---- ShmSegment ---------------------------------------------------------------------------

ShmSegment::~ShmSegment()
{ // 0x4a5934
  if (m_addr && shmdt(m_addr) == -1)
    LOG_ERR << "[ShmSegment]Free Shm shmdt failed!" << -1;
}

// 0x4a5a6c — 0x7b6 = IPC_CREAT|IPC_EXCL|0666, then 0x3b6 = IPC_CREAT|0666.
bool ShmSegment::OpenOrCreateSegment(unsigned long size)
{
  m_key = GetSegmentKey(m_procId);
  LOG_INFO << "[ShmSegment]seg key:" << m_key << " procID:" << static_cast<int>(m_procId);
  m_segId = shmget(static_cast<key_t>(m_key), size, IPC_CREAT | IPC_EXCL | 0666);
  LOG_INFO << "[ShmSegment]segID:" << m_segId;
  if (m_segId == -1)
  {
    m_existed = false;
    m_segId = shmget(static_cast<key_t>(m_key), size, IPC_CREAT | 0666);
    if (m_segId == -1)
    {
      LOG_ERR << "[ShmSegment]get segment fail!!";
      return false;
    }
  }
  else
  {
    // We created it, i.e. ava has not; per the original this is "invalid" but continues.
    m_existed = true;
    LOG_ERR << "[ShmSegment]ava create seg invalid!!";
  }
  m_addr = shmat(m_segId, nullptr, 0);
  // shmat() reports errors as (void*)-1.
  if (m_addr ==
      reinterpret_cast<void*>(-1)) // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
  {
    m_addr = nullptr;
    LOG_ERR << "[ShmSegment]seg point NULL!!";
    return false;
  }
  LOG_INFO << "[ShmSegment]seg create success seg:" << m_segId;
  return true;
}

int ShmSegment::OpenOnlySegment() const { return shmget(static_cast<key_t>(m_key), 0, 0); } // 0x4a60b8
bool ShmSegment::CheckShmExist() const
{
  return shmget(static_cast<key_t>(m_key), 0, IPC_CREAT | IPC_EXCL) == -1;
} // 0x4a6114

// ---- ShmSemaphore -------------------------------------------------------------------------

// 0x4a6994
int ShmSemaphore::GetSemID(unsigned char procId)
{
  const key_t key = static_cast<key_t>(procId) << 16;
  const int id = semget(key, 1, IPC_CREAT | 0666);
  LOG_INFO << "sem_id:" << id;
  if (id == -1)
  {
    perror("semget failed");
    LOG_ERR << "Semaphore semget failed procID:" << static_cast<int>(procId);
  }
  return id;
}

// 0x4a62c8 — SETVAL 1
bool ShmSemaphore::EnableSem() const
{
  if (m_semId == -1)
  {
    LOG_ERR << "Semaphore semID is -1 proc:" << static_cast<int>(m_procId);
    return false;
  }
  semun arg{};
  // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access,cppcoreguidelines-pro-type-vararg,hicpp-vararg):
  // semctl() is variadic and takes its argument as a union semun, by definition.
  arg.val = 1;
  const int rc = semctl(m_semId, 0, SETVAL, arg);
  // NOLINTEND(cppcoreguidelines-pro-type-union-access,cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  if (rc == -1)
  {
    LOG_ERR << "Semaphore semctl failed proc:" << static_cast<int>(m_procId);
    return false;
  }
  return true;
}

// 0x4a652c
bool ShmSemaphore::GetLock() const
{
  if (m_semId == -1)
  {
    LOG_ERR << "Semaphore GetLock semID -1";
    return false;
  }
  sembuf op{.sem_num = 0, .sem_op = -1, .sem_flg = SEM_UNDO};
  if (semop(m_semId, &op, 1) == -1)
  {
    LOG_ERR << "Semaphore GetLock failed semID:" << m_semId;
    return false;
  }
  return true;
}

// 0x4a6760
bool ShmSemaphore::Unlock() const
{
  if (m_semId == -1)
  {
    LOG_ERR << "Semaphore Unlock semID -1";
    return false;
  }
  sembuf op{.sem_num = 0, .sem_op = 1, .sem_flg = SEM_UNDO};
  if (semop(m_semId, &op, 1) == -1)
  {
    LOG_ERR << "Semaphore Unlock failed semID:" << m_semId;
    return false;
  }
  return true;
}

// ---- ShmMsg -------------------------------------------------------------------------------

ShmMsg::~ShmMsg()
{ // 0x4a447c
  delete m_segment;
  delete m_sem;
}

// 0x4a4518
bool ShmMsg::Init()
{
  static bool inited = false; // DAT_0053fd91
  if (inited)
    return true;
  // NOLINTBEGIN(cppcoreguidelines-owning-memory): owned by ShmMsg and deleted in its destructor,
  // as in the original.
  if (!m_segment)
    m_segment = new ShmSegment(kAvaProcId);
  if (!m_sem)
  {
    m_sem = new ShmSemaphore(kAvaProcId);
    // NOLINTEND(cppcoreguidelines-owning-memory)
    if (!m_sem)
    {
      LOG_ERR << "new ShmSemaphore failed!!";
      return false;
    }
  }
  if (!m_sem->EnableSem())
  {
    LOG_ERR << "Enable ShmSemaphore failed!!";
    return false;
  }
  if (!m_segment->OpenOrCreateSegment(kSegSize))
  {
    LOG_ERR << "ShmMsg open shm failed!";
    return false;
  }
  if (!m_addr)
  {
    m_addr = static_cast<unsigned char*>(m_segment->GetSegmentPointer());
    if (!m_addr)
    {
      LOG_ERR << "ShmMsg get shm addr failed!";
      return false;
    }
  }
  inited = true;
  return true;
}

// 0x4a49f0
bool ShmMsg::IsTalk()
{
  if (!m_addr)
  {
    LOG_ERR << "ShmMsg::IsTalk Segment NULL!";
    return false;
  }
  if (!m_sem->GetLock())
  {
    LOG_ERR << "ShmMsg::IsTalk Semaphore Lock err!";
    return false;
  }
  const bool talk = m_addr[0] == 1;
  m_sem->Unlock();
  return talk;
}

// 0x4a4c14
unsigned char ShmMsg::GetFrameCnt()
{
  if (!m_addr)
  {
    LOG_ERR << "ShmMsg::GetFrameCnt Segment NULL!";
    return 0;
  }
  if (!m_sem->GetLock())
  {
    LOG_ERR << "ShmMsg::GetFrameCnt Semaphore Lock err!";
    return 0;
  }
  const unsigned char n = m_addr[1];
  m_sem->Unlock();
  return n;
}

// 0x4a4e2c — moves every pending frame into the local queue and pops one.
bool ShmMsg::GetData(AudioDataFromAgora& out)
{
  if (!m_addr)
  {
    LOG_ERR << "ShmMsg::GetData Segment NULL!";
    return false;
  }
  if (!m_sem->GetLock())
  {
    LOG_ERR << "ShmMsg::GetData Semaphore Lock err!";
    return false;
  }
  const unsigned char n = m_addr[1];
  if (n == 0)
  {
    m_sem->Unlock();
    if (m_frames.empty())
      return false;
    out = m_frames.front();
    m_frames.pop();
    return true;
  }
  for (unsigned char i = 0; i < n; ++i)
  {
    AudioDataFromAgora f;
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): queued raw buffer, freed by the consumer
    f.audioBuffer = malloc(kFrameSize);
    f.size = kFrameSize;
    memcpy(f.audioBuffer, m_addr + 2 + (i * kFrameSize), kFrameSize);
    m_frames.push(f);
  }
  m_addr[1] = 0;
  m_sem->Unlock();
  out = m_frames.front();
  m_frames.pop();
  return true;
}

// 0x4a5170
bool ShmMsg::SetTalk(bool talk)
{
  if (!m_addr)
  {
    LOG_ERR << "ShmMsg::SetTalk Segment NULL!";
    return false;
  }
  if (!m_sem->GetLock())
  {
    LOG_ERR << "ShmMsg::SetTalk Semaphore Lock err!";
    return false;
  }
  m_addr[0] = static_cast<unsigned char>(talk);
  m_sem->Unlock();
  return true;
}

// 0x4a53a0
bool ShmMsg::SetFrameCnt(unsigned char cnt)
{
  if (!m_addr)
  {
    LOG_ERR << "ShmMsg::SetFrameCnt Segment NULL!";
    return false;
  }
  if (!m_sem->GetLock())
  {
    LOG_ERR << "ShmMsg::SetFrameCnt Semaphore Lock err!";
    return false;
  }
  m_addr[1] = cnt;
  m_sem->Unlock();
  return true;
}
