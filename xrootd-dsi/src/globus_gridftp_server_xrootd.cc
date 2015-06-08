/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2013 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

//------------------------------------------------------------------------------
//! @file globus_gridftp_server_xrootd.cc
//! @author Geoffray Adde - CERN
//! @brief Implementation of a GridFTP DSI plugin for XRootD with optional EOS features
//------------------------------------------------------------------------------
#if defined(linux)
#define _LARGE_FILES
#endif

// this is for debuuging only
// it logs some messages in /tmp/globus_alternate_log.txt
// and in /mnt/rd/globus_alternate_log.txt
//#define FORKDEBUGGING

#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <iostream>
#include <set>

/*----------------------------------------------------------------------------*/
#include "XrdCl/XrdClFile.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdSys/XrdSysDNS.hh"
#include "XrdOuc/XrdOucString.hh"

#include "XrdUtils.hh"
#include "ChunkHandler.hh"
#include "AsyncMetaHandler.hh"
#include "XrdFileIo.hh"
#include "dsi_xrootd.hh"
#include "XrdGsiBackendMapper.hh"

#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <sys/time.h>
#include <stdarg.h>
#include <algorithm>

class MyTimer
{
public:
  MyTimer ()
  {
    file = fopen ("/tmp/MyTimer.txt", "w+");
    pthread_mutex_init (&mutex, NULL);
  }

  ~MyTimer ()
  {
    fclose (file);
    pthread_mutex_destroy (&mutex);
  }

  void PrintAndFlush (const char *format, ...)
  {
    va_list argptr;
    va_start(argptr, format);
    Print (format, argptr);
    va_end(argptr);
    fflush (file);
  }

  void Print (const char *format, ...)
  {
    timeval t1;
    gettimeofday (&t1, NULL);
    pthread_mutex_lock (&mutex);
    fprintf (file, "%10.10d.%6.6d\t", (int) t1.tv_sec, (int) t1.tv_usec);

    va_list argptr;
    va_start(argptr, format);
    vfprintf (file, format, argptr);
    va_end(argptr);
    fflush (file);
    pthread_mutex_unlock (&mutex);
  }
protected:
  FILE* file;
  pthread_mutex_t mutex;
};

static XrdGsiBackendMapper *gsiFtpBackend;
/**
 * \brief Struct to handle the configuration of the DSI plugin.
 *
 * \details This structure should have only one instance.
 * The configuration is read from the environment as the struct is constructed.
 *
 */
struct globus_l_gfs_xrootd_config
{
  bool EosCks;
  bool EosChmod;
  bool EosAppTag;
  bool EosBook;
  bool EosRemote;
  bool EosNodeLs;
  int XrdReadAheadBlockSize, XrdReadAheadNBlocks, BackendServersDiscoveryTTL, BackendDiscoveryRetryInterval, BackendDefaultPort;
  std::string BackendDefaultPortString;
  std::string TruncationTmpFileSuffix;
  std::string XrootdVmp;
  std::string ServerRole;
  std::string BackendServersInitList;
  std::vector<std::string> allTheServers, allTheServersNoPort;

  globus_l_gfs_xrootd_config ()
  {
    gsiFtpBackend = new XrdGsiBackendMapper ();
    const char *cptr = 0;
    EosRemote = EosBook = EosCks = EosChmod = EosAppTag = EosNodeLs = false;
    XrdReadAheadBlockSize = (int) ReadaheadBlock::sDefaultBlocksize;
    XrdReadAheadNBlocks = (int) XrdFileIo::sNumRdAheadBlocks;
    TruncationTmpFileSuffix = ".__GridFtpTemp__";

    cptr = getenv ("XROOTD_VMP");
    if (cptr != 0) XrootdVmp = cptr;

    cptr = getenv ("XROOTD_DSI_SERVER_ROLE");
    if (cptr != 0) ServerRole = cptr;

    cptr = getenv ("XROOTD_DSI_GSIFTP_BACKENDSERVERS");
    if (cptr != 0) BackendServersInitList = cptr;

    if (getenv ("XROOTD_DSI_EOS"))
    {
      EosBook = EosCks = EosChmod = EosAppTag = EosRemote = EosNodeLs = true;
    }
    else
    {
      EosCks = (getenv ("XROOTD_DSI_EOS_CKS") != 0);
      EosChmod = (getenv ("XROOTD_DSI_EOS_CHMOD") != 0);
      EosAppTag = (getenv ("XROOTD_DSI_EOS_APPTAG") != 0);
      EosBook = (getenv ("XROOTD_DSI_EOS_BOOK") != 0);
      EosRemote = (getenv ("XROOTD_DSI_EOS_REMOTE") != 0);
      EosNodeLs = (getenv ("XROOTD_DSI_EOS_NODELS") != 0);
    }

    cptr = getenv ("XROOTD_DSI_READAHEADBLOCKSIZE");
    if (cptr != 0) XrdReadAheadBlockSize = atoi (cptr);

    cptr = getenv ("XROOTD_DSI_READAHEADNBLOCKS");
    if (cptr != 0) XrdReadAheadNBlocks = atoi (cptr);

    cptr = getenv ("XROOTD_DSI_GSIFTP_BACKENDDISCOVERTTL");
    BackendServersDiscoveryTTL = 0;
    if (cptr != 0)
    {
      BackendServersDiscoveryTTL = atoi (cptr);
      XrdGsiBackendMapper::This->SetAvailGsiTtl (BackendServersDiscoveryTTL);
    }

    cptr = getenv ("XROOTD_DSI_GSIFTP_BACKENDDISCOVERRETRYINTERVAL");
    if (cptr != 0)
    {
      BackendDiscoveryRetryInterval = atoi (cptr);
      XrdGsiBackendMapper::This->SetUnavailGsiRetryInterval (BackendDiscoveryRetryInterval);
    }

    cptr = getenv ("XROOTD_DSI_GSIFTP_BACKENDDEFAULTPORT");
    if (cptr != 0)
    {
      BackendDefaultPort = atoi (cptr);
      BackendDefaultPortString = cptr;
      XrdGsiBackendMapper::This->SetGsiBackendPort (BackendDefaultPortString);
    }

    if (!BackendServersInitList.empty () && ServerRole == "frontend")
    {
      int current, beg_tok = -1, end_tok = -1;
      std::stringstream ss;
      ss << "list of remote nodes is [";
      const char* sbeg = BackendServersInitList.c_str ();
      for (current = 0; current < (int) BackendServersInitList.size (); current++)
      {
        const char &c = BackendServersInitList[current];
        if (c == ' ' || c == '\t')
        {
          if (beg_tok < 0)
            continue; // looking for the beginning of token
          else if (end_tok < 0) end_tok = current; // previous position was the end of token
          // else looking for next ,
        }
        else if (c == ',' || current == (int) BackendServersInitList.size () - 1)
        {
          if (beg_tok >= 0 && end_tok < 0)
          {
            end_tok = current;
            if (c != ',') end_tok++;
          }
          if (beg_tok < 0 || end_tok < 0)
          {
            globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "error parsing XROOTD_DSI_GSIFTP_BACKENDSERVERS\n");
            allTheServers.clear ();
            allTheServersNoPort.clear ();
            break;
          }
          std::string token (sbeg + beg_tok, end_tok - beg_tok);
          allTheServers.push_back (token);
          auto idx = token.find (':');
          allTheServersNoPort.push_back (token.substr (0, idx));
          XrdGsiBackendMapper::This->AddToProbeList (token.substr (0, idx));
          ss << " " << allTheServers.back ();
          beg_tok = end_tok = -1;
        }
        else
        {
          if (beg_tok < 0) beg_tok = current;
          if (end_tok >= 0)
          {
            globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "error parsing XROOTD_DSI_GSIFTP_BACKENDSERVERS\n");
            allTheServers.clear ();
            allTheServersNoPort.clear ();
            break;
          }
        }
      }
      globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "%s ]\n", ss.str ().c_str ());
      XrdUtils::SortAlongFirstVect (allTheServersNoPort, allTheServers);
    }
  }
  ~globus_l_gfs_xrootd_config ()
  {
    XrdGsiBackendMapper::sDestructLock.WriteLock ();
    if (XrdGsiBackendMapper::This) delete XrdGsiBackendMapper::This;
    XrdGsiBackendMapper::This = NULL;
    XrdGsiBackendMapper::sDestructLock.UnLock ();
  }
};
globus_l_gfs_xrootd_config *config = NULL;

/**
 * Class for handling async responses when DSI receives data.
 * In that case, globus reads from network and XRootD writes the data to a file.
 */
class DsiRcvResponseHandler : public AsyncMetaHandler
{
public:
  /**
   * \brief Constructor
   *
   * @param XRootD handle
   */
  DsiRcvResponseHandler (globus_l_gfs_xrood_handle_s *handle) :
      AsyncMetaHandler (), mNumRegRead (0), mNumCbRead (0), mNumRegWrite (0), mNumCbWrite (0), mHandle (handle), mAllBufferMet (false), mOver (
          false), mNumExpectedBuffers (-1), clean_tid (0)
  {
    globus_mutex_init (&mOverMutex, NULL);
    globus_cond_init (&mOverCond, NULL);
  }

  /**
   * \brief Destructor
   */
  virtual ~DsiRcvResponseHandler ()
  {
    globus_mutex_destroy (&mOverMutex);
    globus_cond_destroy (&mOverCond);
  }

  /**
   * \brief Register the buffer associated to a given file chunk
   *
   * @param offset Offset of the file chunk
   * @param length Length of the file chunk
   * @param buffer Buffer
   */
  void RegisterBuffer (uint64_t offset, uint64_t length, globus_byte_t* buffer)
  {
    mBufferMap[std::pair<uint64_t, uint32_t> (offset, (uint32_t) length)] = buffer;
  }

  /**
   * \brief Disable the buffer
   *
   * The function disables the buffer.
   * When all the buffer are disable, that shows that the activity is over for the current copy.
   *
   * @param buffer Buffer to disable
   */
  void DisableBuffer (globus_byte_t* buffer)
  {
    mActiveBufferSet.erase (buffer);
    if (!mAllBufferMet)
    {
      // check the expected number of buffers
      mMetBufferSet.insert (buffer);
      // to cope with the fact that a buffer might be unregistered without having being used in any callback (typically small files)
      if ((int) mMetBufferSet.size () == mNumExpectedBuffers)
      {
        mAllBufferMet = true;
      }
    }
  }

  /**
   * \brief Set the number of buffers used for the copy.
   *
   * @param nBuffers
   */
  void SetExpectedBuffers (int nBuffers)
  {
    pthread_mutex_lock (&mHandle->mutex);
    mNumExpectedBuffers = nBuffers;
    pthread_mutex_unlock (&mHandle->mutex);
  }

  /**
   * \brief Get the count of active buffers
   *
   * An active buffer is merely a buffer that has been used once and not disabled.
   *
   * @return The count of active buffers
   */
  size_t GetActiveCount () const
  {
    return mActiveBufferSet.size ();
  }

  /**
   * \brief Get the count of buffers
   *
   * @return The number of different buffers ever called by RegisterBuffer
   */
  size_t GetBufferCount () const
  {
    return mMetBufferSet.size ();
  }

  /**
   * \brief Check if the copy is over.
   *
   * @return
   */
  bool IsOver () const
  {
    return (GetActiveCount () == 0) && (GetBufferCount () != 0) && ((int) GetBufferCount () == mNumExpectedBuffers)
        && (mNumCbRead == mNumRegRead) && (mNumExpectedResp == mNumReceivedResp);
  }

  /**
   * \brief XRootD response handler function.
   *
   * This function is called after a write to XRootD is executed.
   * If everything ran fine and if there is some data left, another Globus read is registered.
   *
   * @param pStatus The status of the XRootD write operation.
   * @param chunk The chunk handler associated to the writes for the money transfer to be registered.
   */
  virtual void HandleResponse (XrdCl::XRootDStatus* pStatus, ChunkHandler* chunk)
  {
    mNumCbWrite++;
    const char *func = "DsiRcvResponseHandler::HandleResponse";
    pthread_mutex_lock (&mHandle->mutex);

    if (!mAllBufferMet)
    {
      // check the expected number of buffers
      mMetBufferSet.insert (mBufferMap[std::pair<uint64_t, uint32_t> (chunk->GetOffset (), chunk->GetLength ())]);
      mActiveBufferSet.insert (mBufferMap[std::pair<uint64_t, uint32_t> (chunk->GetOffset (), chunk->GetLength ())]);
      if ((int) mMetBufferSet.size () == mNumExpectedBuffers)
      {
        mAllBufferMet = true;
      }
    }

    globus_byte_t* buffer = mBufferMap[std::pair<uint64_t, uint32_t> (chunk->GetOffset (), chunk->GetLength ())];
    if (pStatus->IsError ())
    { // if there is a xrootd write error
      if (mHandle->cached_res == GLOBUS_SUCCESS)
      { //if it's the first error
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: XRootd write issued an error response : %s \n", func, pStatus->ToStr ().c_str ());
        mHandle->cached_res = globus_l_gfs_make_error (pStatus->ToStr ().c_str (), pStatus->errNo);
        mHandle->done = GLOBUS_TRUE;
      }
      DisableBuffer (buffer);
    }
    else
    { // if there is no error, continue
      globus_gridftp_server_update_bytes_written (mHandle->op, chunk->GetOffset (), chunk->GetLength ());

      bool spawn = (mHandle->optimal_count >= (int) GetActiveCount ());
      // if required and valid, spawn again
      if (spawn && (mHandle->done == GLOBUS_FALSE))
      {
        mBufferMap.erase (std::pair<uint64_t, uint32_t> (chunk->GetOffset (), chunk->GetLength ()));
        globus_result_t result = globus_gridftp_server_register_read (mHandle->op, buffer, mHandle->block_size,
                                                                      globus_l_gfs_file_net_read_cb, mHandle);

        if (result != GLOBUS_SUCCESS)
        {
          globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: register Globus read has finished with a bad result \n", func);
          mHandle->cached_res = globus_l_gfs_make_error ("Error registering globus read", result);
          mHandle->done = GLOBUS_TRUE;
          DisableBuffer (buffer);
        }
        else
          mNumRegRead++;
      }
      else
      { // if not spawning, delete the buffer
        DisableBuffer (buffer);
      }
    }
    AsyncMetaHandler::HandleResponse (pStatus, chunk); // MUST be called AFTER the actual processing of the inheriting class
    SignalIfOver ();
    pthread_mutex_unlock (&mHandle->mutex);
  }

  /**
   * \brief Signal that cleanup is to be done if the copy is over.
   *
   * Note that the cleaning-up should be explicitly triggered after WaitOK
   */
  void SignalIfOver ()
  {
    if (IsOver ())
    {
      globus_mutex_lock (&mOverMutex);
      mOver = true;
      globus_cond_signal (&mOverCond);
      globus_mutex_unlock (&mOverMutex);
    }
  }

  /**
   * \brief Wait for the copy to be over.
   *
   * @return
   */
  virtual bool WaitOK ()
  {
    // wait for the end of the copy to be signaled
    globus_mutex_lock (&mOverMutex);
    while (!mOver)
      globus_cond_wait (&mOverCond, &mOverMutex);
    globus_mutex_unlock (&mOverMutex);

    return AsyncMetaHandler::WaitOK ();
  }

  /**
   * not necessary in normal operations, for debug purpose only.
   * Refer also to mNumExpectedResp mNumReceivedResp though it's not the same information.
   */
  int mNumRegRead, mNumCbRead;

  int mNumRegWrite, mNumCbWrite;

  /**
   * \brief Clean-up the handler making it ready for another copy
   *
   */
  void*
  CleanUp ()
  {
    std::stringstream ss;
    // close the XRootD destination file
    delete mHandle->fileIo;
    mHandle->fileIo = NULL;
    // move the file to its final name is needed
    if (mHandle->tmpsfix_size > 0)
    {
      XrdCl::XRootDStatus st = XrdUtils::RenameTmpToFinal (*mHandle->tempname, mHandle->tmpsfix_size, config->EosRemote);
      if (!st.IsOK ()) mHandle->cached_res = globus_l_gfs_make_error (ss.str ().c_str (), st.errNo);
      delete mHandle->tempname;
      mHandle->tempname = NULL;
      mHandle->tmpsfix_size = 0;
    }
    //auto
    // Reset the Response Handler
    Reset ();
    pthread_mutex_unlock (&mHandle->mutex);

    return NULL;
  }

protected:
  /**
   * \brief Reset the state of the handler, it's part of the clean-up procedure.
   */
  void Reset ()
  {
    for (std::set<globus_byte_t*>::iterator it = mMetBufferSet.begin (); it != mMetBufferSet.end (); it++)
      globus_free(*it);
    mMetBufferSet.clear ();
    mActiveBufferSet.clear ();
    mBufferMap.clear ();
    mNumExpectedBuffers = -1;
    mAllBufferMet = false;
    mNumRegRead = mNumCbRead = mNumRegWrite = mNumCbWrite = 0;
    mOver = false; // to finalize the Reset of the handler
    static_cast<AsyncMetaHandler*> (this)->Reset ();
  }

  globus_l_gfs_xrood_handle_s *mHandle;
  std::map<std::pair<uint64_t, uint32_t>, globus_byte_t*> mBufferMap;
  std::set<globus_byte_t*> mMetBufferSet;
  std::set<globus_byte_t*> mActiveBufferSet;
  bool mAllBufferMet, mOver;
  int mNumExpectedBuffers;
  mutable globus_mutex_t mOverMutex;
  mutable globus_cond_t mOverCond; ///< condition variable to signal that the cleanup is done
  mutable pthread_t clean_tid;
};

/**
 * Class for handling async responses when DSI sends data.
 * In that case, XRootD reads from a file and Globus writes to the network.
 * To overcome a globus limitation (bug?), a mechanism forces writes to Globus to be issued in order.
 * This mechanism can be disabled.
 */
class DsiSendResponseHandler : public AsyncMetaHandler
{
public:
  /**
   * \brief Constructor
   *
   * @param handle XRootD handle
   * @param writeinorder indicate that the response handler should request Globus writes with offsets strictly ordered
   */
  DsiSendResponseHandler (globus_l_gfs_xrood_handle_s *handle, bool writeinorder = true) :
      AsyncMetaHandler (), mNumRegRead (0), mNumCbRead (0), mNumRegWrite (0), mNumCbWrite (0), mWriteInOrder (writeinorder), mHandle (
          handle), mAllBufferMet (false), mOver (false), mNumExpectedBuffers (-1), clean_tid (0)
  {
    globus_mutex_init (&mOverMutex, NULL);
    globus_cond_init (&mOverCond, NULL);
    if (mWriteInOrder) pthread_cond_init (&mOrderCond, NULL);
  }

  /**
   * \brief Destructor
   */
  virtual ~DsiSendResponseHandler ()
  {
    globus_mutex_destroy (&mOverMutex);
    globus_cond_destroy (&mOverCond);
    if (mWriteInOrder) pthread_cond_destroy (&mOrderCond);
  }

  /**
   * \brief Register the buffer associated to a given file chunk
   *
   * @param offset Offset of the file chunck
   * @param length Length of the file chunk
   * @param buffer Buffer
   */
  void RegisterBuffer (uint64_t offset, uint64_t length, globus_byte_t* buffer)
  {
    mBufferMap[std::pair<uint64_t, uint32_t> (offset, length)] = buffer;
    mRevBufferMap[buffer] = std::pair<uint64_t, uint32_t> (offset, length);
  }

  /**
   * \brief Disable the buffer
   *
   * The function disables the buffer.
   * When all the buffer are disable, that shows that the activity is over for the current copy.
   *
   * @param buffer Buffer to disable
   */
  void DisableBuffer (globus_byte_t* buffer)
  {
    mActiveBufferSet.erase (buffer);
    mBufferMap.erase (mRevBufferMap[buffer]);
    mRevBufferMap.erase (buffer);
    if (!mAllBufferMet)
    { // check the expected number of buffers
      mMetBufferSet.insert (buffer);
      // to cope with the fact that a buffer might be unregistered without having being used in any callback (typically small files)
      if ((int) mMetBufferSet.size () == mNumExpectedBuffers)
      {
        mAllBufferMet = true;
      }
    }
  }
  ;

  /**
   * \brief Set the number of buffers used for the copy.
   *
   * @param nBuffers
   */
  void SetExpectedBuffers (int nBuffers)
  {
    pthread_mutex_lock (&mHandle->mutex);
    mNumExpectedBuffers = nBuffers;
    pthread_mutex_unlock (&mHandle->mutex);
  }

  /**
   * \brief Get the count of active buffers
   *
   * An active buffer is merely a buffer that has been used once and not disabled.
   *
   * @return The count of active buffers
   */
  size_t GetActiveCount () const
  {
    return mActiveBufferSet.size ();
  }

  /**
   * \brief Get the count of buffers
   *
   * @return The number of different buffers ever called by RegisterBuffer
   */
  size_t GetBufferCount () const
  {
    return mMetBufferSet.size ();
  }

  /**
   * \brief Check if the copy is over.
   *
   * @return
   */
  bool IsOver () const
  {
    return (GetActiveCount () == 0) && (GetBufferCount () != 0) && ((int) GetBufferCount () == mNumExpectedBuffers)
        && (mNumCbWrite == mNumRegWrite) && (mNumExpectedResp == mNumReceivedResp);
  }

  /**
   * \brief XRootD response handler function.
   *
   * This function is called after a read from XRootD is executed.
   * If the read ran fine, the corresponding Globus write is registered.
   *
   * @param pStatus The status of the XRootD write operation.
   * @param chunk The chunk handler associated to the read
   */
  virtual void HandleResponse (XrdCl::XRootDStatus* pStatus, ChunkHandler* chunk)
  {
    HandleResponse (pStatus->IsError (), pStatus->errNo, chunk->GetOffset (), chunk->GetLength (), chunk->GetRespLength (), pStatus, chunk);
  }

  /**
   * \brief XRootD response handler function.
   * @param isErr Error status of the response to handle
   * @param errNo Error number of the response to handle
   * @param offset Offset of the read attached to the response
   * @param len Length of the read attached to the response
   * @param rlen Length of the response itself (can be different from the length of the read)
   * @param pStatus Status object of the read operation (can be NULL)
   * @param chunk Read handler of the read operation (can be NULL)
   */
  void HandleResponse (bool isErr, uint32_t errNo, uint64_t offset, uint32_t len, uint32_t rlen, XrdCl::XRootDStatus* pStatus = 0,
                       ChunkHandler* chunk = 0)
  {
    mNumCbRead++;
    const char *func = "DsiSendResponseHandler::HandleResponse";
    pthread_mutex_lock (&mHandle->mutex);

    globus_byte_t* buffer = mBufferMap[std::pair<uint64_t, uint32_t> (offset, len)];

    if (!mAllBufferMet)
    { // check the expected number of buffers
      mMetBufferSet.insert (mBufferMap[std::pair<uint64_t, uint32_t> (offset, len)]);
      mActiveBufferSet.insert (buffer);
      if ((int) mMetBufferSet.size () == mNumExpectedBuffers)
      {
        mAllBufferMet = true;
      }
    }

    size_t nbread = rlen;
    if (isErr && errNo != EFAULT)
    { // if there is a xrootd read error which is not bad (offset,len)
      if (mHandle->cached_res == GLOBUS_SUCCESS)
      { //if it's the first one
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: XRootd read issued an error response : %s \n", func, pStatus->ToStr ().c_str ());
        mHandle->cached_res = globus_l_gfs_make_error (pStatus->ToStr ().c_str (), pStatus->errNo);
        mHandle->done = GLOBUS_TRUE;
      }
      DisableBuffer (buffer);
    }
    else if (isErr && errNo == EFAULT && nbread == 0)
    { // if there is a bad (offset,len) error with an empty response
      DisableBuffer (buffer);
      mHandle->done = GLOBUS_TRUE;
    }
    else
    { // if there is no error or if bad (offset,len) but non empty response

      // !!!!! WARNING the value of the offset argument of globus_gridftp_server_register_write is IGNORED
      // !!!!! a mechanism is then implemented to overcome this limitation, it can be enabled or disbaled
      if (mWriteInOrder)
      {
        // wait that the current offset is the next in the order or that the set of offsets to process is empty
        while ((globus_off_t) offset != (*mRegisterReadOffsets.begin ()) || mRegisterReadOffsets.size () == 0)
          pthread_cond_wait (&mOrderCond, &mHandle->mutex);
        mRegisterReadOffsets.erase ((globus_off_t) offset);
        pthread_cond_broadcast (&mOrderCond);
      }

      globus_result_t result = globus_gridftp_server_register_write (mHandle->op, buffer, nbread, offset, // !! this value doesn't matter
                                                                     -1, globus_l_gfs_net_write_cb, mHandle);

      if (result != GLOBUS_SUCCESS)
      {
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: register Globus write has finished with a bad result \n", func);
        mHandle->cached_res = globus_l_gfs_make_error ("Error registering globus write", result);
        mHandle->done = GLOBUS_TRUE;
        DisableBuffer (buffer);
      } // spawn
      else
        mNumRegWrite++;
    }
    if (pStatus != 0 && chunk != 0)
      AsyncMetaHandler::HandleResponse (pStatus, chunk); // MUST be called AFTER the actual processing of the inheriting class
    else
      mNumReceivedResp++;
    SignalIfOver ();
    pthread_mutex_unlock (&mHandle->mutex);
  }

  struct HandleRespStruct
  {
    DsiSendResponseHandler *_this;
    bool isErr;
    uint32_t errNo;
    uint64_t offset;
    uint32_t len;
    uint32_t rlen;
  };

  static void*
  RunHandleResp (void* handleRespStruct)
  {
    HandleRespStruct *hrs = static_cast<HandleRespStruct *> (handleRespStruct);
    hrs->_this->HandleResponse (hrs->isErr, hrs->errNo, hrs->offset, hrs->len, hrs->rlen);
    delete hrs;
    return NULL;
  }

  void HandleResponseAsync (bool isErr, uint32_t errNo, uint64_t offset, uint32_t len, uint32_t rlen)
  {
    HandleRespStruct *hrs = new HandleRespStruct;
    //*hrs={this,isErr,errNo,offset,len,rlen};
    hrs->_this = this;
    hrs->isErr = isErr;
    hrs->errNo = errNo;
    hrs->offset = offset;
    hrs->len = len;
    hrs->rlen = rlen;
    pthread_t thread;
    mNumExpectedResp++;
    XrdSysThread::Run (&thread, RunHandleResp, (void*) hrs);
  }

  /**
   * \brief Signal that cleanup is to be done if the copy is over.
   *
   * Note that the cleaning-up should be explicitly triggered after WaitOK
   */
  void SignalIfOver ()
  {
    if (IsOver ())
    {
      globus_mutex_lock (&mOverMutex);
      mOver = true;
      globus_cond_signal (&mOverCond);
      globus_mutex_unlock (&mOverMutex);
    }
  }

  /**
   * \brief Wait for the copy to be over.
   *
   * @return
   */
  virtual bool WaitOK ()
  {
    // wait for the end of the copy to be signaled
    globus_mutex_lock (&mOverMutex);
    while (!mOver)
      globus_cond_wait (&mOverCond, &mOverMutex);
    globus_mutex_unlock (&mOverMutex);

    return AsyncMetaHandler::WaitOK ();
  }

  int mNumRegRead, mNumCbRead;
  /**
   * not necessary in normal operations, for debug purpose only.
   * Refer also to mNumExpectedResp mNumReceivedResp though it's not the same information.
   */
  int mNumRegWrite, mNumCbWrite;
  /**
   * This map maps buffer -> (offset,length)
   */
  std::map<std::pair<uint64_t, uint32_t>, globus_byte_t*> mBufferMap;
  /**
   * This map maps (offset,length) -> buffer
   */
  std::map<globus_byte_t*, std::pair<uint64_t, uint32_t> > mRevBufferMap;

  /**
   * This boolean shows if the writes to globus must be issued in order
   */
  const bool mWriteInOrder;
  mutable pthread_cond_t mOrderCond; ///< condition variable to signal that the next write is the offset order is to be issued
  mutable std::set<globus_off_t> mRegisterReadOffsets; ///< Set of issued write to process the lowest offset when issuing a register_write

  /**
   * \brief Clean-up the handler making it ready for another copy
   *
   */
  void*
  CleanUp ()
  {
    pthread_mutex_lock (&mHandle->mutex);
    // close the XRootD source file
    delete mHandle->fileIo;
    mHandle->fileIo = NULL;
    // Reset the Response Handler
    Reset ();
    pthread_mutex_unlock (&mHandle->mutex);

    return NULL;
  }

protected:
  /**
   * \brief Reset the state of the handler, it's part of the clean-up procedure.
   */
  void Reset ()
  {
    for (std::set<globus_byte_t*>::iterator it = mMetBufferSet.begin (); it != mMetBufferSet.end (); it++)
      globus_free(*it);
    mMetBufferSet.clear ();
    mActiveBufferSet.clear ();
    mBufferMap.clear ();
    mRevBufferMap.clear ();
    mNumExpectedBuffers = -1;
    mAllBufferMet = false;
    mNumRegRead = mNumCbRead = mNumRegWrite = mNumCbWrite = 0;
    mOver = false; // to finalize the Reset of the handler
    static_cast<AsyncMetaHandler*> (this)->Reset ();
  }

  globus_l_gfs_xrood_handle_s *mHandle;
  std::set<globus_byte_t*> mMetBufferSet;
  std::set<globus_byte_t*> mActiveBufferSet;
  bool mAllBufferMet, mOver;
  int mNumExpectedBuffers;
  mutable globus_mutex_t mOverMutex;
  mutable globus_cond_t mOverCond; ///< condition variable to signal that the cleanup is done
  mutable pthread_t clean_tid;
};

/**
 * \brief Compute the length of the next chunk to be read and update the xroot_handle struct.
 */
int next_read_chunk (globus_l_gfs_xrood_handle_s *xrootd_handle, int64_t &nextreadl)
{
  //const char *func="next_read_chunk";

  if (xrootd_handle->blk_length == 0)
  { // for initialization and next block
    // check the next range to read
    globus_gridftp_server_get_read_range (xrootd_handle->op, &xrootd_handle->blk_offset, &xrootd_handle->blk_length);
    if (xrootd_handle->blk_length == 0)
    {
      return 1; // means no more chunk to read
    }
  }
  else
  {
    if (xrootd_handle->blk_length != -1)
    {
      //xrootd_handle->blk_length -= lastreadl;
      // here we suppose that when a read succeed it always read block size or block length
      xrootd_handle->blk_offset += (
          (xrootd_handle->blk_length >= (globus_off_t) xrootd_handle->block_size) ?
              (globus_off_t) xrootd_handle->block_size : (globus_off_t) xrootd_handle->blk_length);
      xrootd_handle->blk_length -=
          xrootd_handle->blk_length >= (globus_off_t) xrootd_handle->block_size ?
              (globus_off_t) xrootd_handle->block_size : (globus_off_t) xrootd_handle->blk_length;
    }
    else
    {
      xrootd_handle->blk_offset += xrootd_handle->block_size;
    }
  }
  if (xrootd_handle->blk_length == -1 || xrootd_handle->blk_length > (globus_off_t) xrootd_handle->block_size)
  {
    nextreadl = xrootd_handle->block_size;
  }
  else
  {
    nextreadl = xrootd_handle->blk_length;
  }

  return 0; // means chunk updated
}

XrootPath XP;
DsiRcvResponseHandler* RcvRespHandler;
DsiSendResponseHandler* SendRespHandler;

extern "C"
{

  static globus_version_t local_version =
  { 0, /* major version number */
  1, /* minor version number */
  1157544130, 0 /* branch ID */
  };

/// utility function to make errors
  static globus_result_t globus_l_gfs_make_error (const char *msg, int errCode)
  {
    char *err_str;
    globus_result_t result;
    GlobusGFSName(__FUNCTION__);
    err_str = globus_common_create_string ("%s error: %s", msg, strerror (errCode));
    result = GlobusGFSErrorGeneric(err_str);
    globus_free(err_str);
    return result;
  }

  /* fill the statbuf into globus_gfs_stat_t */
  void fill_stat_array (globus_gfs_stat_t * filestat, struct stat statbuf, char *name)
  {
    filestat->mode = statbuf.st_mode;
    ;
    filestat->nlink = statbuf.st_nlink;
    filestat->uid = statbuf.st_uid;
    filestat->gid = statbuf.st_gid;
    filestat->size = statbuf.st_size;

    filestat->mtime = statbuf.st_mtime;
    filestat->atime = statbuf.st_atime;
    filestat->ctime = statbuf.st_ctime;

    filestat->dev = statbuf.st_dev;
    filestat->ino = statbuf.st_ino;
    filestat->name = strdup (name);
  }
  /* free memory in stat_array from globus_gfs_stat_t->name */
  void free_stat_array (globus_gfs_stat_t * filestat, int count)
  {
    int i;
    for (i = 0; i < count; i++)
      free (filestat[i].name);
  }

  /**
   *  \brief This hook is called when a session is initialized
   *
   *  \details This function is called when a new session is initialized, ie a user
   *  connects to the server.  This hook gives the dsi an opportunity to
   *  set internal state that will be threaded through to all other
   *  function calls associated with this session.  And an opportunity to
   *  reject the user.
   *
   *  finished_info.info.session.session_arg should be set to an DSI
   *  defined data structure.  This pointer will be passed as the void *
   *  user_arg parameter to all other interface functions.
   *
   *  NOTE: at nice wrapper function should exist that hides the details
   *        of the finished_info structure, but it currently does not.
   *        The DSI developer should just follow this template for now
   *
   */
  static
  void globus_l_gfs_xrootd_start (globus_gfs_operation_t op, globus_gfs_session_info_t *session_info)
  {
    globus_l_gfs_xrootd_handle_t *xrootd_handle;
    globus_gfs_finished_info_t finished_info;
    const char *func = "globus_l_gfs_xrootd_start";

    GlobusGFSName(__FUNCTION__);

    xrootd_handle = (globus_l_gfs_xrootd_handle_t *) globus_malloc(sizeof(globus_l_gfs_xrootd_handle_t));
    if (!xrootd_handle)
    GlobusGFSErrorMemory("xroot_handle");
    try
    {
      RcvRespHandler = new DsiRcvResponseHandler (xrootd_handle);
      SendRespHandler = new DsiSendResponseHandler (xrootd_handle);
    }
    catch (...)
    {
      GlobusGFSErrorMemory("xrootResponseHandler");
    }

    globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "%s: started, uid: %u, gid: %u\n", func, getuid (), getgid ());
    pthread_mutex_init (&xrootd_handle->mutex, NULL);
    xrootd_handle->isInit = true;

    xrootd_handle->tempname = NULL;
    xrootd_handle->tmpsfix_size = 0;

    //=== additional init added for foreground / background mode ===//
    memset (&xrootd_handle->session_info, 0, sizeof(globus_gfs_session_info_t));
    memset (&xrootd_handle->cur_result, 0, sizeof(globus_result_t));
    memset (&xrootd_handle->active_delay, 0, sizeof(globus_bool_t));
    xrootd_handle->active_data_info = NULL;
    xrootd_handle->active_transfer_info = NULL;
    memset (&xrootd_handle->active_op, 0, sizeof(globus_gfs_operation_t));
    xrootd_handle->active_user_arg = NULL;
    memset (&xrootd_handle->active_callback, 0, sizeof(globus_gfs_storage_transfer_t));
    globus_mutex_init (&xrootd_handle->gfs_mutex, GLOBUS_NULL);

    if (session_info->username != NULL) xrootd_handle->session_info.username = strdup (session_info->username);
    if (session_info->password != NULL) xrootd_handle->session_info.password = strdup (session_info->password);
    if (session_info->subject != NULL) xrootd_handle->session_info.subject = strdup (session_info->subject);
    xrootd_handle->session_info.map_user = session_info->map_user;
    xrootd_handle->session_info.del_cred = session_info->del_cred;
    //=====

    memset (&finished_info, '\0', sizeof(globus_gfs_finished_info_t));
    finished_info.type = GLOBUS_GFS_OP_SESSION_START;
    finished_info.result = GLOBUS_SUCCESS;
    finished_info.info.session.session_arg = xrootd_handle;
    finished_info.info.session.username = session_info->username;
    // if null we will go to HOME directory
    finished_info.info.session.home_dir = NULL;

    globus_gridftp_server_operation_finished (op, GLOBUS_SUCCESS, &finished_info);

    return;
  }

  /**
   *  \brief This hook is called when a session ends
   *
   *  \details This is called when a session ends, ie client quits or disconnects.
   *  The dsi should clean up all memory they associated wit the session
   *  here.
   *
   */
  static void globus_l_gfs_xrootd_destroy (void *user_arg)
  {
    if (user_arg)
    {
      globus_l_gfs_xrootd_handle_t *xrootd_handle;
      xrootd_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;
      if (xrootd_handle->isInit)
      {
        globus_mutex_destroy (&xrootd_handle->gfs_mutex);
        delete RcvRespHandler;
        delete SendRespHandler;
        pthread_mutex_destroy (&xrootd_handle->mutex);
        globus_free(xrootd_handle);
      }
    }
    else
    {
      //globus_gfs_log_message(GLOBUS_GFS_LOG_INFO, "%s: xrootd_handle not allocated : no clean-up to make \n", "globus_l_gfs_xrootd_destroy");
    }
  }

  void globus_l_gfs_file_copy_stat (globus_gfs_stat_t * stat_object, XrdCl::StatInfo * stat_buf, const char * filename,
                                    const char * symlink_target)
  {
    GlobusGFSName(__FUNCTION__);

    XrootStatUtils::initStat (stat_object);

    stat_object->mode = XrootStatUtils::mapFlagsXrd2Pos (stat_buf->GetFlags ());
    stat_object->size = stat_buf->GetSize (); // stat
    stat_object->mtime = stat_buf->GetModTime ();
    stat_object->atime = stat_object->mtime;
    stat_object->ctime = stat_object->mtime;

    if (filename && *filename)
    {
      stat_object->name = strdup (filename);
    }
    else
    {
      stat_object->name = NULL;
    }
    if (symlink_target && *symlink_target)
    {
      stat_object->symlink_target = strdup (symlink_target);
    }
    else
    {
      stat_object->symlink_target = NULL;
    }
  }

  static
  void globus_l_gfs_file_destroy_stat (globus_gfs_stat_t * stat_array, int stat_count)
  {
    int i;
    GlobusGFSName(__FUNCTION__);

    for (i = 0; i < stat_count; i++)
    {
      if (stat_array[i].name != NULL)
      {
        globus_free(stat_array[i].name);
      }
      if (stat_array[i].symlink_target != NULL)
      {
        globus_free(stat_array[i].symlink_target);
      }
    }
    globus_free(stat_array);
  }

  /* basepath and filename must be MAXPATHLEN long
   * the pathname may be absolute or relative, basepath will be the same */
  static
  void globus_l_gfs_file_partition_path (const char * pathname, char * basepath, char * filename)
  {
    char buf[MAXPATHLEN];
    char * filepart;
    GlobusGFSName(__FUNCTION__);

    strncpy (buf, pathname, MAXPATHLEN);
    buf[MAXPATHLEN - 1] = '\0';

    filepart = strrchr (buf, '/');
    while (filepart && !*(filepart + 1) && filepart != buf)
    {
      *filepart = '\0';
      filepart = strrchr (buf, '/');
    }

    if (!filepart)
    {
      strcpy (filename, buf);
      basepath[0] = '\0';
    }
    else
    {
      if (filepart == buf)
      {
        if (!*(filepart + 1))
        {
          basepath[0] = '\0';
          filename[0] = '/';
          filename[1] = '\0';
        }
        else
        {
          *filepart++ = '\0';
          basepath[0] = '/';
          basepath[1] = '\0';
          strcpy (filename, filepart);
        }
      }
      else
      {
        *filepart++ = '\0';
        strcpy (basepath, buf);
        strcpy (filename, filepart);
      }
    }
  }

  /**
   *  \brief Stat a file.
   *
   *  \details This interface function is called whenever the server needs
   *  information about a given file or resource.  It is called then an
   *  LIST is sent by the client, when the server needs to verify that
   *  a file exists and has the proper permissions, etc.
   *
   */
  static
  void globus_l_gfs_xrootd_stat (globus_gfs_operation_t op, globus_gfs_stat_info_t * stat_info, void * user_arg)
  {
    globus_result_t result;
    globus_gfs_stat_t * stat_array;
    int stat_count = 0;
    char basepath[MAXPATHLEN];
    char filename[MAXPATHLEN];
    char symlink_target[MAXPATHLEN];
    char *PathName;
    char myServerPart[MAXPATHLEN], myPathPart[MAXPATHLEN];
    GlobusGFSName(__FUNCTION__);
    PathName = stat_info->pathname;

    std::string request (MAXPATHLEN * 2, '\0');
    XrdCl::Buffer arg;
    XrdCl::StatInfo* xrdstatinfo = 0;
    XrdCl::XRootDStatus status;
    XrdCl::URL server;
    /*
     If we do stat_info->pathname++, it will cause third-party transfer
     hanging if there is a leading // in path. Don't know why. To work
     around, we replaced it with PathName.
     */
    while ((strlen (PathName) > 1) && (PathName[0] == '/' && PathName[1] == '/'))
    {
      PathName++;
    }

    char *myPath, buff[2048];
    if (!(myPath = XP.BuildURL (PathName, buff, sizeof(buff)))) myPath = PathName;

    if (XrootPath::SplitURL (myPath, myServerPart, myPathPart, MAXPATHLEN))
    {
      result = GlobusGFSErrorSystemError("stat", ECANCELED);
      globus_gridftp_server_finished_stat (op, result, NULL, 0);
      return;
    }

    arg.FromString (myPathPart);
    server.FromString (myServerPart);
    XrdCl::FileSystem fs (server);
    status = fs.Stat (myPathPart, xrdstatinfo);
    if (status.IsError ())
    {
      if (xrdstatinfo) delete xrdstatinfo;
      result = GlobusGFSErrorSystemError("stat", XrootStatUtils::mapError (status.errNo));
      goto error_stat1;
    }

    globus_l_gfs_file_partition_path (myPathPart, basepath, filename);

    if (!(xrdstatinfo->GetFlags () & XrdCl::StatInfo::IsDir) || stat_info->file_only)
    {
      stat_array = (globus_gfs_stat_t *) globus_malloc(sizeof(globus_gfs_stat_t));
      if (!stat_array)
      {
        result = GlobusGFSErrorMemory("stat_array");
        goto error_alloc1;
      }

      globus_l_gfs_file_copy_stat (stat_array, xrdstatinfo, filename, symlink_target);
      stat_count = 1;
    }
    else
    {
      XrdCl::DirectoryList *dirlist = 0;
      status = fs.DirList (myPathPart, XrdCl::DirListFlags::Stat, dirlist, (uint16_t) 0);
      if (!status.IsOK ())
      {
        if (dirlist) delete dirlist;
        result = GlobusGFSErrorSystemError("opendir", XrootStatUtils::mapError (status.errNo));
        goto error_open;
      }

      stat_count = dirlist->GetSize ();

      stat_array = (globus_gfs_stat_t *) globus_malloc(sizeof(globus_gfs_stat_t) * (stat_count + 1));
      if (!stat_array)
      {
        if (dirlist) delete dirlist;
        result = GlobusGFSErrorMemory("stat_array");
        goto error_alloc2;
      }

      int i = 0;
      for (XrdCl::DirectoryList::Iterator it = dirlist->Begin (); it != dirlist->End (); it++)
      {
        std::string path = (*it)->GetName ();
        globus_l_gfs_file_partition_path (path.c_str (), basepath, filename);
        globus_l_gfs_file_copy_stat (&stat_array[i++], (*it)->GetStatInfo (), filename, NULL);
      }
      if (dirlist) delete dirlist;
    }

    globus_gridftp_server_finished_stat (op, GLOBUS_SUCCESS, stat_array, stat_count);

    globus_l_gfs_file_destroy_stat (stat_array, stat_count);

    return;

    error_alloc2: error_open: error_alloc1: error_stat1: globus_gridftp_server_finished_stat (op, result, NULL, 0);

    /*    GlobusGFSFileDebugExitWithError();  */
  }

  /**
   *  \brief Executes a gridftp command on the DSI back-end
   *
   *  \details This interface function is called when the client sends a 'command'.
   *  commands are such things as mkdir, remdir, delete.  The complete
   *  enumeration is below.
   *
   *  To determine which command is being requested look at:
   *      cmd_info->command
   *
   *      the complete list is :
   *      GLOBUS_GFS_CMD_MKD = 1,
   *      GLOBUS_GFS_CMD_RMD,
   *      GLOBUS_GFS_CMD_DELE,
   *      GLOBUS_GFS_CMD_SITE_AUTHZ_ASSERT,
   *      GLOBUS_GFS_CMD_SITE_RDEL,
   *      GLOBUS_GFS_CMD_RNTO,
   *      GLOBUS_GFS_CMD_RNFR,
   *      GLOBUS_GFS_CMD_CKSM,
   *      GLOBUS_GFS_CMD_SITE_CHMOD,
   *      GLOBUS_GFS_CMD_SITE_DSI,
   *      GLOBUS_GFS_CMD_SITE_SETNETSTACK,
   *      GLOBUS_GFS_CMD_SITE_SETDISKSTACK,
   *      GLOBUS_GFS_CMD_SITE_CLIENTINFO,
   *      GLOBUS_GFS_CMD_DCSC,
   *      GLOBUS_GFS_CMD_SITE_CHGRP,
   *      GLOBUS_GFS_CMD_SITE_UTIME,
   *      GLOBUS_GFS_CMD_SITE_SYMLINKFROM,
   *      GLOBUS_GFS_CMD_SITE_SYMLINK,
   *      GLOBUS_GFS_MIN_CUSTOM_CMD = 4096
   *
   */
  static void globus_l_gfs_xrootd_command (globus_gfs_operation_t op, globus_gfs_command_info_t* cmd_info, void *user_arg)
  {

    GlobusGFSName(__FUNCTION__);

    char cmd_data[MAXPATHLEN];
    char * PathName;
    globus_result_t rc = GLOBUS_SUCCESS;
    std::string cks;

    // create the full path and split it
    char *myPath, buff[2048];
    char myServerPart[MAXPATHLEN], myPathPart[MAXPATHLEN];
    PathName = cmd_info->pathname;
    while (PathName[0] == '/' && PathName[1] == '/')
      PathName++;
    if (!(myPath = XP.BuildURL (PathName, buff, sizeof(buff)))) myPath = PathName;
    if (XrootPath::SplitURL (myPath, myServerPart, myPathPart, MAXPATHLEN))
    {
      rc = GlobusGFSErrorGeneric("command fail : error parsing the filename");
      globus_gridftp_server_finished_command (op, rc, NULL);
      return;
    }

    // open the filesystem
    XrdCl::URL server;
    XrdCl::Buffer arg, *resp;
    XrdCl::Status status;
    arg.FromString (myPathPart);
    server.FromString (myServerPart);
    XrdCl::FileSystem fs (server);

    switch (cmd_info->command)
    {
      case GLOBUS_GFS_CMD_MKD:
        (status = fs.MkDir (myPathPart, XrdCl::MkDirFlags::None, (XrdCl::Access::Mode) XrootStatUtils::mapModePos2Xrd (0777))).IsError ()
            && (rc = GlobusGFSErrorGeneric((std::string ("mkdir() fail : ") += status.ToString ()).c_str ()));
        break;
      case GLOBUS_GFS_CMD_RMD:
        (status = fs.RmDir (myPathPart)).IsError ()
            && (rc = GlobusGFSErrorGeneric((std::string ("rmdir() fail") += status.ToString ()).c_str ()));
        break;
      case GLOBUS_GFS_CMD_DELE:
        (fs.Rm (myPathPart)).IsError () && (rc = GlobusGFSErrorGeneric((std::string ("rm() fail") += status.ToString ()).c_str ()));
        break;
      case GLOBUS_GFS_CMD_SITE_RDEL:
        /*
         result = globus_l_gfs_file_delete(
         op, PathName, GLOBUS_TRUE);
         */
        rc = GLOBUS_FAILURE;
        break;
      case GLOBUS_GFS_CMD_RNTO:
        char myServerPart2[MAXPATHLEN], myPathPart2[MAXPATHLEN];
        if (!(myPath = XP.BuildURL (cmd_info->from_pathname, buff, sizeof(buff)))) myPath = cmd_info->from_pathname;
        if (XrootPath::SplitURL (myPath, myServerPart2, myPathPart2, MAXPATHLEN))
        {
          rc = GlobusGFSErrorGeneric("rename() fail : error parsing the target filename");
          globus_gridftp_server_finished_command (op, rc, NULL);
          return;
        }
        (status = fs.Mv (myPathPart2, myPathPart)).IsError ()
            && (rc = GlobusGFSErrorGeneric((std::string ("rename() fail") += status.ToString ()).c_str ()));
        break;
      case GLOBUS_GFS_CMD_SITE_CHMOD:
        if (config->EosChmod)
        { // Using EOS Chmod
          char request[16384];
          sprintf (request, "%s?mgm.pcmd=chmod&mode=%d", myPathPart, cmd_info->chmod_mode); // specific to eos
          arg.FromString (request);
          status = fs.Query (XrdCl::QueryCode::OpaqueFile, arg, resp);
          rc = GlobusGFSErrorGeneric("chmod() fail");
          if (status.IsOK ())
          {
            char tag[4096];
            int retc = 0;
            int items = sscanf (resp->GetBuffer (), "%s retc=%d", tag, &retc);
            fflush (stderr);
            if (retc || (items != 2) || (strcmp (tag, "chmod:")))
            {
              // error
            }
            else
            {
              rc = GLOBUS_SUCCESS;
            }
          }
          delete resp;
        }
        else
        { // Using XRoot Chmod
          (status = fs.ChMod (myPathPart, (XrdCl::Access::Mode) XrootStatUtils::mapModePos2Xrd (cmd_info->chmod_mode))).IsError () && (rc =
              GlobusGFSErrorGeneric((std::string ("chmod() fail") += status.ToString ()).c_str ()));
        }
        break;
      case GLOBUS_GFS_CMD_CKSM:
        fflush (stderr);
        if (config->EosCks)
        { // Using EOS checksum
          if (!strcmp (cmd_info->cksm_alg, "adler32") || !strcmp (cmd_info->cksm_alg, "ADLER32"))
          {
            char request[16384];
            sprintf (request, "%s?mgm.pcmd=checksum", myPathPart); // specific to eos
            arg.FromString (request);
            status = fs.Query (XrdCl::QueryCode::OpaqueFile, arg, resp);
            fflush (stderr);
            if (status.IsOK ())
            {
              if ((strstr (resp->GetBuffer (), "retc=0") && (strlen (resp->GetBuffer ()) > 10)))
              {
                // the server returned a checksum via 'checksum: <checksum> retc='
                const char* cbegin = resp->GetBuffer () + 10;
                const char* cend = strstr (resp->GetBuffer (), "retc=");
                if (cend > (cbegin + 8))
                {
                  cend = cbegin + 8;
                }
                if (cbegin && cend)
                {
                  strncpy (cmd_data, cbegin, cend - cbegin);
                  // 0-terminate
                  cmd_data[cend - cbegin] = 0;
                  rc = GLOBUS_SUCCESS;
                  globus_gridftp_server_finished_command (op, rc, cmd_data);
                  return;
                }
                else
                {
                  rc = GlobusGFSErrorGeneric("checksum() fail : error parsing response");
                }
              }
              else
              {
                rc = GlobusGFSErrorGeneric("checksum() fail : error parsing response");
              }
            }
          }
          rc = GLOBUS_FAILURE;
        }
        else
        { // Using XRootD checksum
          if ((status = XrdUtils::GetRemoteCheckSum (cks, cmd_info->cksm_alg, myServerPart, myPathPart)).IsError ()
              || (cks.size () >= MAXPATHLEN))
          { //UPPER CASE CHECKSUM ?
            rc = GlobusGFSErrorGeneric((std::string ("checksum() fail") += status.ToString ()).c_str ());
            break;
          }
          strcpy (cmd_data, cks.c_str ());
          globus_gridftp_server_finished_command (op, GLOBUS_SUCCESS, cmd_data);
          return;
        }
        break;
      default:
        rc = GlobusGFSErrorGeneric("not implemented");
        break;
    }
    globus_gridftp_server_finished_command (op, rc, NULL);
  }

  /**
   *  \brief Touch a file to get physical locationif needed.
   *
   *  \details This is called only for remote send with EOS
   *
   */

  int xrootd_touch_file (const char *path, int flags, int mode, std::string *error = NULL)
  {
    XrdCl::XRootDStatus st;
    const char *func = "xrootd_touch_file_ifneeded";
    globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "%s: touch file \"%s\"\n", func, path);
    bool caught = false;
    try
    {
      char *myPath, buff[2048];
      if (!(myPath = XP.BuildURL (path, buff, sizeof(buff))))
      {
        strcpy (buff, path);
        myPath = buff;
      }

      if (config->EosAppTag)
      { // add the 'eos.gridftp' application tag
        if (strlen (myPath))
        {
          if (strchr (myPath, '?'))
          {
            strcat (myPath, "&eos.app=eos/gridftp"); // specific to EOS
          }
          else
          {
            strcat (myPath, "?eos.app=eos/gridftp"); // specific to EOS
          }
        }
      }

      XrdFileIo fileIo;
      st = fileIo.Open (myPath, (XrdCl::OpenFlags::Flags) XrootStatUtils::mapFlagsPos2Xrd (flags),
                        (XrdCl::Access::Mode) XrootStatUtils::mapModePos2Xrd (mode));

      if (!st.IsOK ())
      {
        *error = st.ToStr ();
        replace (error->begin (), error->end (), '\n', ' ');
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: XrdCl::File::Open error : %s\n", func, error->c_str ());
        throw;
      }
      else if (fileIo.Close () != SFS_OK)
      {
        st.status = XrdCl::stError;
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: XrdCl::File::Close error\n", func, error->c_str ());
        throw;
      }
    }
    catch (const std::exception& ex)
    {
      *error = ex.what ();
      caught = true;
    }
    catch (const std::string& ex)
    {
      *error = ex;
      caught = true;
    }
    catch (...)
    {
      *error = "unknown";
      caught = true;
    }
    if (caught)
    {
      replace (error->begin (), error->end (), '\n', ' ');
      globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: Exception caught when calling XrdCl::File::Open : %s \n", func, error->c_str ());
      *error = "exception : " + *error;
      return GLOBUS_FAILURE;
    }

    return ((!st.IsOK ()) ? GLOBUS_FAILURE : GLOBUS_SUCCESS);
  }

  /**
   *  \brief Receive a file from globus and store it into the DSI back-end.
   *
   *  \details This interface function is called when the client requests that a
   *  file be transfered to the server.
   *
   *  To receive a file the following functions will be used in roughly
   *  the presented order.  They are doced in more detail with the
   *  gridftp server documentation.
   *
   *      globus_gridftp_server_begin_transfer();
   *      globus_gridftp_server_register_read();
   *      globus_gridftp_server_finished_transfer();
   *
   */

  int xrootd_open_file (char *path, int flags, int mode, globus_l_gfs_xrootd_handle_t *xrootd_handle, std::string *error = NULL)
  {
    XrdCl::XRootDStatus st;
    const char *func = "xrootd_open_file";
    globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "%s: open file \"%s\"\n", func, path);
    bool caught = false;
    try
    {
      char *myPath, buff[2048];
      if (!(myPath = XP.BuildURL (path, buff, sizeof(buff))))
      {
        strcpy (buff, path);
        myPath = buff;
      }

      // if the file is opened for truncation as a backend server
      // if it's a not a backend server it should be a standalone server
      // we open it and then truncate it
      // it avoids physical relocation on EOS
      bool trunc = false;
      if ((flags & O_TRUNC) && config->ServerRole == "backend")
      {
        // check that the temp file exists which means we are in delayed passive connection
        char myServerPart[MAXPATHLEN + 1], myPathPart[MAXPATHLEN + 1];
        XrootPath::SplitURL (myPath, myServerPart, myPathPart, MAXPATHLEN);
        xrootd_handle->tempname = new std::string;
        //*xrootd_handle->tempname = url.GetLocation ();
        *xrootd_handle->tempname = myServerPart;
        xrootd_handle->tempname->append (std::string ("/") + myPathPart);
        XrdCl::URL url (*xrootd_handle->tempname);
        *xrootd_handle->tempname = url.GetLocation ();
        xrootd_handle->tempname->append (config->TruncationTmpFileSuffix);
        xrootd_handle->tmpsfix_size = config->TruncationTmpFileSuffix.size ();

        //url.FromString(myServerPart);
        XrdCl::FileSystem fs (url.GetProtocol () + "://" + url.GetHostId ());
        XrdCl::StatInfo *si = NULL;
        XrdCl::XRootDStatus st;
        //if ((st=fs.Stat (url.GetPath(), si)).IsOK () && si!=0)
        if ((st = fs.Stat (url.GetPath () + config->TruncationTmpFileSuffix, si)).IsOK () && si != 0)
        {
          globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "%s: trying to stat file %s OK \n", func,
                                  (url.GetPath () + config->TruncationTmpFileSuffix).c_str ());

          strncpy (buff, (*xrootd_handle->tempname + url.GetParamsAsString ()).c_str (), 2048);
          // in case we truncate the file with a delayed passive connection
          // a temp file has been created at the same time as the passive connection
          // we just open it and truncate it
          trunc = true;
          flags &= ~O_TRUNC;
          flags &= ~O_CREAT;
        }
        else
        {
          globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "%s: trying to stat file %s ERROR %s\n", func, xrootd_handle->tempname->c_str (),
                                  st.ToStr ().c_str ());
          delete xrootd_handle->tempname;
          xrootd_handle->tempname = NULL;
          xrootd_handle->tmpsfix_size = 0;
        }

        if (si) delete si;
      }

      if (config->EosAppTag)
      { // add the 'eos.gridftp' application tag
        if (strlen (myPath))
        {
          if (strchr (myPath, '?'))
          {
            strcat (myPath, "&eos.app=eos/gridftp"); // specific to EOS
          }
          else
          {
            strcat (myPath, "?eos.app=eos/gridftp"); // specific to EOS
          }
        }
      }

      xrootd_handle->fileIo = new XrdFileIo;

      globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "%s: open fileIo \"%s\"\n", func, myPath);
      st = xrootd_handle->fileIo->Open (myPath, (XrdCl::OpenFlags::Flags) XrootStatUtils::mapFlagsPos2Xrd (flags),
                                        (XrdCl::Access::Mode) XrootStatUtils::mapModePos2Xrd (mode));

      if (!st.IsOK ())
      {
        *error = st.ToStr ();
        replace (error->begin (), error->end (), '\n', ' ');
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: XrdCl::File::Open error : %s\n", func, error->c_str ());
      }
      else if (trunc)
      {
        st = xrootd_handle->fileIo->Truncate (0);
        if (!st.IsOK ())
        {
          *error = st.ToStr ();
          replace (error->begin (), error->end (), '\n', ' ');
          globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: XrdCl::File::Truncate error : %s\n", func, error->c_str ());
        }
      }
    }
    catch (const std::exception& ex)
    {
      *error = ex.what ();
      caught = true;
    }
    catch (const std::string& ex)
    {
      *error = ex;
      caught = true;
    }
    catch (...)
    {
      *error = "unknown";
      caught = true;
    }
    if (caught)
    {
      replace (error->begin (), error->end (), '\n', ' ');
      globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: Exception caught when calling XrdCl::File::Open : %s \n", func, error->c_str ());
      *error = "exception : " + *error;
      return GLOBUS_FAILURE;
    }

    return ((!st.IsOK ()) ? GLOBUS_FAILURE : GLOBUS_SUCCESS);
  }

  /* receive from client */
  static void globus_l_gfs_file_net_read_cb (globus_gfs_operation_t op, globus_result_t result, globus_byte_t *buffer, globus_size_t nbytes,
                                             globus_off_t offset, globus_bool_t eof, void *user_arg)
  {
    const char *func = "globus_l_gfs_file_net_read_cb";
    RcvRespHandler->mNumCbRead++;
    globus_l_gfs_xrootd_handle_t *xrootd_handle;

    xrootd_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;
    pthread_mutex_lock (&xrootd_handle->mutex);

    if (eof == GLOBUS_TRUE)
    {
      // if eof is reached, we are done, but we still need the buffer to write
      xrootd_handle->cached_res = result;
      xrootd_handle->done = GLOBUS_TRUE;
    }
    if ((result != GLOBUS_SUCCESS) || (nbytes == 0))
    {
      // if the read failed or succeeded with 0 byte, we are done.
      // The buffer is not needed anymore. Regardless if it's an error or not
      xrootd_handle->cached_res = result;
      xrootd_handle->done = GLOBUS_TRUE;
      RcvRespHandler->DisableBuffer (buffer);
    }
    else
    {
      RcvRespHandler->RegisterBuffer (offset, nbytes, buffer);
      int64_t ret = xrootd_handle->fileIo->Write (offset, (const char*) buffer, nbytes, RcvRespHandler);
      if (ret < 0)
      {
        xrootd_handle->cached_res = ret;
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: register XRootD write has finished with a bad result \n", func);
        GlobusGFSName(__FUNCTION__);
        xrootd_handle->cached_res = GlobusGFSErrorGeneric("Error registering XRootD write");
        xrootd_handle->done = GLOBUS_TRUE;
        RcvRespHandler->DisableBuffer (buffer);
      }
      else
      {
        RcvRespHandler->mNumRegWrite++;
      }
    }

    RcvRespHandler->SignalIfOver ();
    pthread_mutex_unlock (&xrootd_handle->mutex);
  }

  static void globus_l_gfs_xrootd_read_from_net (globus_l_gfs_xrootd_handle_t *xrootd_handle)
  {
    globus_byte_t **buffers;
    globus_result_t result;
    const char *func = "globus_l_gfs_xrootd_read_from_net";

    GlobusGFSName(__FUNCTION__);
    /* in the read case this number will vary */
    globus_gridftp_server_get_optimal_concurrency (xrootd_handle->op, &xrootd_handle->optimal_count);

    pthread_mutex_lock (&xrootd_handle->mutex);
    // allocations of the buffers
    buffers = (globus_byte_t**) globus_malloc(xrootd_handle->optimal_count * sizeof(globus_byte_t**));
    if (!buffers) goto error_alloc;
    if (buffers != 0)
    {
      for (int c = 0; c < xrootd_handle->optimal_count; c++)
      {
        buffers[c] = (globus_byte_t*) globus_malloc(xrootd_handle->block_size);
        if (!buffers[c]) goto error_alloc;
      }
    }
    pthread_mutex_unlock (&xrootd_handle->mutex);

    // this optimal count is kept because for every finishing request is replaced by a new one by the response handler if required
    int c;
    for (c = 0; c < xrootd_handle->optimal_count; c++)
    {
      result = globus_gridftp_server_register_read (xrootd_handle->op, buffers[c], xrootd_handle->block_size, globus_l_gfs_file_net_read_cb,
                                                    xrootd_handle);
      if (result != GLOBUS_SUCCESS)
      {
        pthread_mutex_lock (&xrootd_handle->mutex);
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: register Globus read has finished with a bad result \n", func);
        xrootd_handle->cached_res = GlobusGFSErrorGeneric("Error registering globus read");
        xrootd_handle->done = GLOBUS_TRUE;
        pthread_mutex_unlock (&xrootd_handle->mutex);
        break; // if an error happens just let the ResponseHandlers all terminate
      }
      else
        RcvRespHandler->mNumRegRead++;
    }

    RcvRespHandler->SetExpectedBuffers (c);
    RcvRespHandler->WaitOK ();
    RcvRespHandler->CleanUp ();

    globus_gridftp_server_finished_transfer (xrootd_handle->op, xrootd_handle->cached_res);

    globus_free(buffers);

    return;

    error_alloc: result = GlobusGFSErrorMemory("buffers");
    xrootd_handle->cached_res = result;
    xrootd_handle->done = GLOBUS_TRUE;
    delete xrootd_handle->fileIo;
    globus_gridftp_server_finished_transfer (xrootd_handle->op, xrootd_handle->cached_res);
    // free the allocated memory
    if (buffers)
    {
      for (int c = 0; c < xrootd_handle->optimal_count; c++)
        if (buffers[c])
        globus_free(buffers[c]);
      globus_free(buffers);
    }
    pthread_mutex_unlock (&xrootd_handle->mutex);
    return;
  }

  static void globus_l_gfs_xrootd_recv (globus_gfs_operation_t op, globus_gfs_transfer_info_t *transfer_info, void *user_arg)
  {
    globus_l_gfs_xrootd_handle_t *xrootd_handle;
    globus_result_t result;
    char pathname[16384];
    int flags;
    int rc;

    GlobusGFSName(__FUNCTION__);
    xrootd_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;

    if (config->EosBook && transfer_info->alloc_size)
    {
      snprintf (pathname, sizeof(pathname) - 1, "%s?eos.bookingsize=%lu&eos.targetsize=%lu", transfer_info->pathname,
                transfer_info->alloc_size, transfer_info->alloc_size); // specific to eos
    }
    else
    {
      snprintf (pathname, sizeof(pathname), "%s", transfer_info->pathname);
    }

    // try to open
    flags = O_WRONLY | O_CREAT;
    if (transfer_info->truncate) flags |= O_TRUNC;

    std::string error;
    rc = xrootd_open_file (pathname, flags, 0644, xrootd_handle, &error);

    if (rc)
    {
      //result = globus_l_gfs_make_error("open/create", errno);
      result = GlobusGFSErrorGeneric((std::string ("open/create : ") + error).c_str ());
      delete xrootd_handle->fileIo;
      globus_gridftp_server_finished_transfer (op, result);
      return;
    }

    // reset all the needed variables in the handle
    xrootd_handle->cached_res = GLOBUS_SUCCESS;
    xrootd_handle->done = GLOBUS_FALSE;
    xrootd_handle->blk_length = 0;
    xrootd_handle->blk_offset = 0;
    xrootd_handle->op = op;

    globus_gridftp_server_get_block_size (op, &xrootd_handle->block_size);

    globus_gridftp_server_begin_transfer (op, 0, xrootd_handle);

    globus_l_gfs_xrootd_read_from_net (xrootd_handle);

    return;
  }

  /*************************************************************************
   *  \brief Read a file from the DSI back-end and send it to the network.
   *
   *  \details This interface function is called when the client requests to receive
   *  a file from the server.
   *
   *  To send a file to the client the following functions will be used in roughly
   *  the presented order.  They are doced in more detail with the
   *  gridftp server documentation.
   *
   *      globus_gridftp_server_begin_transfer();
   *      globus_gridftp_server_register_write();
   *      globus_gridftp_server_finished_transfer();
   *
   ************************************************************************/
  static void globus_l_gfs_xrootd_send (globus_gfs_operation_t op, globus_gfs_transfer_info_t *transfer_info, void *user_arg)
  {
    globus_l_gfs_xrootd_handle_t *xrootd_handle;
    const char *func = "globus_l_gfs_xrootd_send";
    char *pathname;
    int rc;
    globus_bool_t done;
    globus_result_t result;

    GlobusGFSName(__FUNCTION__);
    xrootd_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;

    pathname = strdup (transfer_info->pathname);

    std::string error;
    rc = xrootd_open_file (pathname, O_RDONLY, 0, xrootd_handle, &error); /* mode is ignored */

    if (rc)
    {
      delete xrootd_handle->fileIo;
      //result = globus_l_gfs_make_error("open", errno);
      result = GlobusGFSErrorGeneric((std::string ("open : ") + error).c_str ());
      globus_gridftp_server_finished_transfer (op, result);
      free (pathname);
      return;
    }
    free (pathname);

    /* reset all the needed variables in the handle */
    xrootd_handle->cached_res = GLOBUS_SUCCESS;
    xrootd_handle->done = GLOBUS_FALSE;
    xrootd_handle->blk_length = 0;
    xrootd_handle->blk_offset = 0;
    xrootd_handle->op = op;

    globus_gridftp_server_get_optimal_concurrency (op, &xrootd_handle->optimal_count);
    globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "%s: optimal_concurrency: %u\n", func, xrootd_handle->optimal_count);

    globus_gridftp_server_get_block_size (op, &xrootd_handle->block_size);
    globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "%s: block_size: %ld\n", func, xrootd_handle->block_size);

    globus_gridftp_server_begin_transfer (op, 0, xrootd_handle);
    done = GLOBUS_FALSE;
    done = globus_l_gfs_xrootd_send_next_to_client (xrootd_handle);
    pthread_mutex_unlock (&xrootd_handle->mutex);
  }

  /* receive from client */
  void globus_l_gfs_net_write_cb_lock (globus_gfs_operation_t op, globus_result_t result, globus_byte_t *buffer, globus_size_t nbwrite,
                                       void * user_arg, bool lock = true)
  {
    const char *func = "globus_l_gfs_net_write_cb";
    globus_off_t read_length;
    int64_t nbread;
    bool usedReadCallBack = false;
    SendRespHandler->mNumCbWrite++;
    globus_l_gfs_xrootd_handle_t *xrootd_handle;

    xrootd_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;

    if (lock) pthread_mutex_lock (&xrootd_handle->mutex);

    GlobusGFSName(__FUNCTION__);

    if ((result != GLOBUS_SUCCESS))
    { // if the write failed, we are done the buffer is not needed anymore
      if (xrootd_handle->cached_res != GLOBUS_SUCCESS)
      { // don't overwrite the first error
        xrootd_handle->cached_res = result;
        xrootd_handle->done = GLOBUS_TRUE;
      }
      SendRespHandler->DisableBuffer (buffer);
      SendRespHandler->SignalIfOver ();
      if (lock) pthread_mutex_unlock (&xrootd_handle->mutex);
      return;
    }

    if (nbwrite == 0) // don't update on the first call
      globus_gridftp_server_update_bytes_written (xrootd_handle->op, SendRespHandler->mRevBufferMap[buffer].first,
                                                  SendRespHandler->mRevBufferMap[buffer].second);

    if (xrootd_handle->done == GLOBUS_FALSE)
    { // if we are not done, look for something else to copy
      if (next_read_chunk (xrootd_handle, read_length))
      { // if return is non zero, no more source to copy from
        xrootd_handle->cached_res = GLOBUS_SUCCESS;
        xrootd_handle->done = GLOBUS_TRUE;
        SendRespHandler->DisableBuffer (buffer);
        SendRespHandler->SignalIfOver ();
        if (lock) pthread_mutex_unlock (&xrootd_handle->mutex);
        return;
      }

      if (nbwrite != 0)
      {
        SendRespHandler->mBufferMap.erase (SendRespHandler->mRevBufferMap[buffer]);
        SendRespHandler->mRevBufferMap.erase (buffer);
      }
      SendRespHandler->RegisterBuffer (xrootd_handle->blk_offset, read_length, buffer);

      globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "%s: register XRootD read from globus_l_gfs_net_write_cb \n", func);
      if (SendRespHandler->mWriteInOrder) SendRespHandler->mRegisterReadOffsets.insert (xrootd_handle->blk_offset);
      nbread = xrootd_handle->fileIo->Read (xrootd_handle->blk_offset, (char*) buffer, read_length, SendRespHandler, true,
                                            &usedReadCallBack);

      if (nbread < 0)
      {
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: register XRootD read has finished with a bad result %d\n", func, nbread);
        xrootd_handle->cached_res = globus_l_gfs_make_error ("Error registering XRootD read", nbread);
        xrootd_handle->done = GLOBUS_TRUE;
        SendRespHandler->DisableBuffer (buffer);
        SendRespHandler->SignalIfOver ();
      }
      else if (nbread == 0)
      { // empty read, EOF is reached
        xrootd_handle->done = GLOBUS_TRUE;
        SendRespHandler->DisableBuffer (buffer);
        SendRespHandler->SignalIfOver ();
      }
      else
      { // succeed
        if (usedReadCallBack) SendRespHandler->mNumRegRead++;
        if (usedReadCallBack)
          globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "%s: register XRootD read from globus_l_gfs_net_write_cb ==> usedReadCallBack\n",
                                  func);
      }
    }
    else
    { // we are done, just drop the buffer
      SendRespHandler->DisableBuffer (buffer);
      SendRespHandler->SignalIfOver ();
      if (lock) pthread_mutex_unlock (&xrootd_handle->mutex);
      return;
    }
    int64_t loffset = xrootd_handle->blk_offset;
    //pthread_mutex_unlock(&xrootd_handle->mutex);
    if ((!usedReadCallBack) && (nbread > 0))
    {
      // take care if requested a read beyond the end of the file, an actual read will be issued even if the file is cache.
      // the current situation happens only if the read didn't issue any error.
      SendRespHandler->HandleResponseAsync (false, 0, loffset, read_length, nbread);
      globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP,
                              "%s: %p register XRootD read from globus_l_gfs_net_write_cb ==> Explicit Callback %d %d\n", func, buffer,
                              (int) read_length, (int) nbread);
    }
    if (lock) pthread_mutex_unlock (&xrootd_handle->mutex);
  }

  void globus_l_gfs_net_write_cb (globus_gfs_operation_t op, globus_result_t result, globus_byte_t *buffer, globus_size_t nbwrite,
                                  void * user_arg)
  {
    globus_l_gfs_net_write_cb_lock (op, result, buffer, nbwrite, user_arg, true);
  }

  static globus_bool_t globus_l_gfs_xrootd_send_next_to_client (globus_l_gfs_xrootd_handle_t *xrootd_handle)
  {
    globus_byte_t **buffers;
    globus_result_t result;

    GlobusGFSName(__FUNCTION__);
    /* in the read case this number will vary */
    globus_gridftp_server_get_optimal_concurrency (xrootd_handle->op, &xrootd_handle->optimal_count);

    pthread_mutex_lock (&xrootd_handle->mutex);
    // allocations of the buffers
    buffers = (globus_byte_t**) globus_malloc(xrootd_handle->optimal_count * sizeof(globus_byte_t**));
    if (!buffers) goto error_alloc;
    if (buffers != 0)
    {
      for (int c = 0; c < xrootd_handle->optimal_count; c++)
      {
        buffers[c] = (globus_byte_t*) globus_malloc(xrootd_handle->block_size);
        if (!buffers[c]) goto error_alloc;
      }
    }
    pthread_mutex_unlock (&xrootd_handle->mutex);

    // this optimal count is kept because for every finishing request is replaced by a new one by the response handler if required
    int c;
    pthread_mutex_lock (&xrootd_handle->mutex);
    xrootd_handle->blk_length = 0;
    for (c = 0; c < xrootd_handle->optimal_count; c++)
    {
      SendRespHandler->mNumRegWrite++;
      globus_l_gfs_net_write_cb_lock (xrootd_handle->op, GLOBUS_SUCCESS, buffers[c], xrootd_handle->blk_length, xrootd_handle, false);
    }
    pthread_mutex_unlock (&xrootd_handle->mutex);

    // wait for the write to be done
    SendRespHandler->SetExpectedBuffers (c);
    SendRespHandler->WaitOK ();
    SendRespHandler->CleanUp ();
    globus_gridftp_server_finished_transfer (xrootd_handle->op, xrootd_handle->cached_res);
    globus_free(buffers);

    return GLOBUS_TRUE;

    error_alloc: result = GlobusGFSErrorMemory("buffers");
    xrootd_handle->cached_res = result;
    xrootd_handle->done = GLOBUS_TRUE;
    delete xrootd_handle->fileIo;
    globus_gridftp_server_finished_transfer (xrootd_handle->op, xrootd_handle->cached_res);
    // free the allocated memory
    if (buffers)
    {
      for (int c = 0; c < xrootd_handle->optimal_count; c++)
        if (buffers[c])
        globus_free(buffers[c]);
      globus_free(buffers);
    }
    pthread_mutex_unlock (&xrootd_handle->mutex);
    return GLOBUS_FALSE;

  }

  // ================================================================== //
  //                                                                    //
  // All the following functions named *_remote_* were adapted          //
  // from the dmlite implementation                                     //
  // which are themselves adapted copy/paste of globus code             //
  // note: contrary to the dmlite case, we don't need to know if        //
  //       we are called from the delayed passive or a normal passive   //
  //                                                                    //
  // ================================================================== //

  static globus_result_t globus_l_gfs_remote_init_bounce_info (globus_l_gfs_remote_ipc_bounce_t ** bounce, globus_gfs_operation_t op,
                                                               void * state, globus_l_gfs_xrootd_handle_t * my_handle)
  {
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;
    globus_result_t result = GLOBUS_SUCCESS;
    GlobusGFSName(__FUNCTION__);

    bounce_info = (globus_l_gfs_remote_ipc_bounce_t *) globus_calloc(1, sizeof(globus_l_gfs_remote_ipc_bounce_t));

    if (!bounce_info)
    {
      result = GlobusGFSErrorMemory("bounce_info");
      goto error;
    }

    bounce_info->op = op;
    bounce_info->state = state;
    bounce_info->my_handle = my_handle;
    *bounce = bounce_info;

    error: return result;
  }

  static globus_result_t globus_l_gfs_remote_node_release (globus_l_gfs_remote_node_info_t * node_info,
                                                           globus_gfs_brain_reason_t release_reason)
  {
    GlobusGFSName(__FUNCTION__);

    globus_gfs_ipc_close (node_info->ipc_handle, NULL, NULL);

    if (node_info->cs)
    globus_free(node_info->cs);

    globus_free(node_info);

    return GLOBUS_SUCCESS;
  }

  static void globus_l_gfs_remote_ipc_error_cb (globus_gfs_ipc_handle_t ipc_handle, globus_result_t result, void * user_arg)
  {
//    globus_l_gfs_xrootd_handle_t * my_handle;
//    GlobusGFSName(__FUNCTION__);
//
//    my_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;
    globus_gfs_log_result (GLOBUS_GFS_LOG_ERR, "IPC error", result);
  }

  static void globus_l_gfs_remote_node_error_kickout (void * user_arg)
  {
    globus_l_gfs_remote_node_info_t * node_info;

    node_info = (globus_l_gfs_remote_node_info_t *) user_arg;

    globus_gfs_log_result (GLOBUS_GFS_LOG_ERR, "could not obtain IPC handle", node_info->cached_result);

    node_info->callback (node_info, node_info->cached_result, node_info->user_arg);
  }

  static void globus_l_gfs_remote_node_request_kickout (globus_gfs_ipc_handle_t ipc_handle, globus_result_t result,
                                                        globus_gfs_finished_info_t * reply, void * user_arg)
  {
    globus_bool_t callback = GLOBUS_FALSE;
    globus_l_gfs_remote_node_info_t * node_info;
    GlobusGFSName(__FUNCTION__);

    node_info = (globus_l_gfs_remote_node_info_t *) user_arg;

    /* LOCKS! */
    if (result == GLOBUS_SUCCESS)
    {
      globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "connected to remote node\n");
      node_info->ipc_handle = ipc_handle;

      callback = GLOBUS_TRUE;
    }
    else
    {
      globus_gfs_log_result (GLOBUS_GFS_LOG_ERR, "could not connect to remote node", result);

      node_info->error_count++;
      if (node_info->error_count >= IPC_RETRY)
      {
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "retry limit reached, giving up\n");
        XrdGsiBackendMapper::This->MarkAsDown (node_info->my_handle->session_info.host_id);
        callback = GLOBUS_TRUE;
      }
      else
      {
        result = globus_gfs_ipc_handle_obtain (&node_info->my_handle->session_info, &globus_gfs_ipc_default_iface,
                                               globus_l_gfs_remote_node_request_kickout, node_info, globus_l_gfs_remote_ipc_error_cb,
                                               node_info->my_handle);
        if (result != GLOBUS_SUCCESS) callback = GLOBUS_TRUE;
      }
    }

    if (callback)
    {
      node_info->callback (node_info, result, node_info->user_arg);
      if (result != GLOBUS_SUCCESS)
      {
        globus_free(node_info);
      }
    }
  }

  static globus_result_t globus_l_gfs_remote_node_request (globus_l_gfs_xrootd_handle_t * my_handle, char * pathname,
                                                           globus_l_gfs_remote_node_cb callback, void * user_arg)
  {
    globus_l_gfs_remote_node_info_t * node_info;
    globus_result_t result;

    GlobusGFSName(__FUNCTION__);

    if (!callback) return GLOBUS_FAILURE;

    globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "node request for pathname: %s \n", pathname);

    // ==== touch the temp file to give it a physical location
    if (pathname && my_handle->mode == XROOTD_FILEMODE_TRUNCATE)
    {
      // EOS SIZE BOOKING IS NOT POSSIBLE AT THIS POINT AS WE DON'T KNOW THE SIZE OF THE FILE THAT WILL BE COPIED
      std::string spathname (pathname);
      spathname.append (config->TruncationTmpFileSuffix);
      // there is no need to give a value to my_handle->tmpsfix_size
      // as the handle is not transmitted to the chose backend node
      // try to open
      auto flags = O_WRONLY | O_CREAT | O_TRUNC;
      std::string error;
      int rc = xrootd_touch_file (spathname.c_str (), flags, 0644, &error);
      if (rc)
      {
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "error touching temp file on passive connection: %s [%d]\n", error.c_str (), rc);
        return GLOBUS_FAILURE;
      }
    }
    // =======================================================

    std::vector<std::string> selectedServers;
    {
      // create the full path and split it
      char *myPath, buff[2048];
      char myServerPart[MAXPATHLEN], myPathPart[MAXPATHLEN];
      *myServerPart = '\0';
      *myPathPart = '\0';
      if (pathname)
      {
        char * PathName = pathname;
        while (PathName[0] == '/' && PathName[1] == '/')
          PathName++;
        if (!(myPath = XP.BuildURL (PathName, buff, sizeof(buff)))) myPath = PathName;
        if (XrootPath::SplitURL (myPath, myServerPart, myPathPart, MAXPATHLEN))
        {
          globus_result_t rc = GlobusGFSErrorGeneric("command fail : error parsing the filename");
          return rc;
        }
      }
      std::string errString;
      std::vector<std::string> potentialNewServers;
      //globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "calling remote server with %s %s and %d\n", myServerPart,myPathPart,my_handle->mode);

      XrdUtils::GetRemoteServers (selectedServers, errString, potentialNewServers, XrdGsiBackendMapper::This, myServerPart, myPathPart,
                                  config->TruncationTmpFileSuffix, my_handle->mode, config->EosRemote);
      if (config->BackendServersDiscoveryTTL > 0)
      {
        globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "autodiscovery: checking potential backend servers\n%s\n",
                                XrdGsiBackendMapper::This->DumpBackendMap ().c_str ());
        for (auto it = potentialNewServers.begin (); it != potentialNewServers.end (); ++it)
        {
          if (XrdGsiBackendMapper::This->AddToProbeList (*it))
            globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "autodiscovery: adding potential backend server %s to probe list\n", it->c_str ());
        }
      }
    }

    // TODO: Change this to be able to pass multiple servers to get multiple connections (How can I determine how many?)
    globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "remote node: %s\n", selectedServers.front ().c_str ());
    my_handle->session_info.host_id = strdup (selectedServers.front ().c_str ()); /* host_id */

    node_info = (globus_l_gfs_remote_node_info_t*) globus_malloc(sizeof(globus_l_gfs_remote_node_info_t));
    memset (node_info, 0, sizeof(globus_l_gfs_remote_node_info_t));
    node_info->callback = callback;
    node_info->user_arg = user_arg;
    node_info->my_handle = my_handle;

    result = globus_gfs_ipc_handle_obtain (&my_handle->session_info, &globus_gfs_ipc_default_iface,
                                           globus_l_gfs_remote_node_request_kickout, node_info, globus_l_gfs_remote_ipc_error_cb,
                                           my_handle);

    if (result != GLOBUS_SUCCESS)
    {
      node_info->cached_result = result;
      globus_callback_register_oneshot(NULL, NULL, globus_l_gfs_remote_node_error_kickout, node_info);
    }

    return GLOBUS_SUCCESS;
  }

  static void globus_l_gfs_remote_data_info_free (globus_gfs_data_info_t * data_info)
  {
    int idx;

    if (data_info->subject != NULL)
    globus_free(data_info->subject);
    if (data_info->interface != NULL)
    globus_free(data_info->interface);
    if (data_info->pathname != NULL)
    globus_free(data_info->pathname);
    if (data_info->contact_strings != NULL)
    {
      for (idx = 0; idx < data_info->cs_count; idx++)
        globus_free((char * )data_info->contact_strings[idx]);
      globus_free(data_info->contact_strings);
    }
  }

  static void globus_l_gfs_ipc_passive_cb (globus_gfs_ipc_handle_t ipc_handle, globus_result_t ipc_result,
                                           globus_gfs_finished_info_t * reply, void * user_arg)
  {
    globus_gfs_finished_info_t finished_info;
    globus_bool_t finished = GLOBUS_FALSE;
    int ndx;
    globus_l_gfs_xrootd_handle_t * my_handle;
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;
    globus_l_gfs_remote_node_info_t * node_info;
    GlobusGFSName(__FUNCTION__);

    node_info = (globus_l_gfs_remote_node_info_t *) user_arg;
    bounce_info = node_info->bounce;
    my_handle = bounce_info->my_handle;

    if (reply->result != GLOBUS_SUCCESS)
    {
      bounce_info->cached_result = reply->result;
    }
    else
    {
      /* XXX this is suspect if we chain DSIs another step */
      node_info->cs = globus_libc_strdup (reply->info.data.contact_strings[0]);
      node_info->data_arg = reply->info.data.data_arg;
    }

    globus_mutex_lock (&my_handle->gfs_mutex);
    {
      bounce_info->nodes_pending--;
      if (ipc_result == GLOBUS_SUCCESS) bounce_info->nodes_obtained++;

      if (!bounce_info->nodes_pending && !bounce_info->nodes_requesting)
      {
        finished = GLOBUS_TRUE;
        if (bounce_info->nodes_obtained == 0) goto error;

        memcpy (&finished_info, reply, sizeof(globus_gfs_finished_info_t));

        finished_info.info.data.data_arg = bounce_info->node_info;
        finished_info.info.data.cs_count = bounce_info->nodes_obtained;
        finished_info.info.data.contact_strings = (const char **) globus_calloc(sizeof(char *), finished_info.info.data.cs_count);

        ndx = 0;

        if (node_info != NULL)
        {
          node_info->stripe_count = 1;

          /* XXX handle case where cs_count from a single node > 1 */
          finished_info.info.data.contact_strings[ndx] = node_info->cs;
          node_info->cs = NULL;

          if (node_info->info && node_info->info_needs_free)
          {
            globus_free(node_info->info);
            node_info->info = NULL;
            node_info->info_needs_free = GLOBUS_FALSE;
          }
          ndx++;
        }globus_assert(ndx == finished_info.info.data.cs_count);
      }
    }
    globus_mutex_unlock (&my_handle->gfs_mutex);

    if (finished)
    {
      globus_gridftp_server_operation_finished (bounce_info->op, finished_info.result, &finished_info);

      for (ndx = 0; ndx < finished_info.info.data.cs_count; ndx++)
        globus_free((void * ) finished_info.info.data.contact_strings[ndx]);
      globus_free(finished_info.info.data.contact_strings);
      globus_free(bounce_info);
    }

    return;

    error: globus_mutex_unlock (&my_handle->gfs_mutex);
    globus_assert(finished && ipc_result != GLOBUS_SUCCESS);
    GlobusGFSErrorOpFinished(bounce_info->op, GLOBUS_GFS_OP_PASSIVE, ipc_result);
    globus_free(bounce_info);
  }

  static void globus_l_gfs_ipc_active_cb (globus_gfs_ipc_handle_t ipc_handle, globus_result_t ipc_result,
                                          globus_gfs_finished_info_t * reply, void * user_arg)
  {
    globus_bool_t finished = GLOBUS_FALSE;
    int ndx;
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;
    globus_l_gfs_remote_node_info_t * node_info;
    globus_gfs_finished_info_t finished_info;
    globus_gfs_data_info_t * info;
    globus_l_gfs_xrootd_handle_t * my_handle;
    GlobusGFSName(__FUNCTION__);

    node_info = (globus_l_gfs_remote_node_info_t *) user_arg;
    bounce_info = node_info->bounce;
    node_info->data_arg = reply->info.data.data_arg;
    my_handle = bounce_info->my_handle;

    node_info->stripe_count = 1;

    globus_mutex_lock (&my_handle->gfs_mutex);
    {
      bounce_info->nodes_pending--;
      if (ipc_result == GLOBUS_SUCCESS) bounce_info->nodes_obtained++;

      if (!bounce_info->nodes_pending && !bounce_info->nodes_requesting)
      {
        finished = GLOBUS_TRUE;
        if (bounce_info->nodes_obtained == 0) goto error;

        memcpy (&finished_info, reply, sizeof(globus_gfs_finished_info_t));

        finished_info.info.data.data_arg = bounce_info->node_info;

        if (node_info->info && node_info->info_needs_free)
        {
          info = (globus_gfs_data_info_t *) node_info->info;
          for (ndx = 0; ndx < info->cs_count; ndx++)
            globus_free((void * ) info->contact_strings[ndx]);
          globus_free(info->contact_strings);
          globus_free(node_info->info);
          node_info->info = NULL;
          node_info->info_needs_free = GLOBUS_FALSE;
        }
      }
    }
    globus_mutex_unlock (&my_handle->gfs_mutex);

    if (finished)
    {
      if (my_handle->active_delay)
      {
        /* return to the original callback */
        my_handle->active_delay = GLOBUS_FALSE;
        globus_l_gfs_remote_data_info_free (my_handle->active_data_info);
        my_handle->active_transfer_info->data_arg = bounce_info->node_info;
        my_handle->active_callback (my_handle->active_op, my_handle->active_transfer_info, my_handle->active_user_arg);
      }
      else
      {
        globus_gridftp_server_operation_finished (bounce_info->op, finished_info.result, &finished_info);
      }

      globus_free(bounce_info);
    }

    return;

    error: globus_assert(finished && ipc_result != GLOBUS_SUCCESS);
    if (my_handle->active_delay)
    {
      /* we have to fake a failure of a different operation */
      my_handle->active_delay = GLOBUS_FALSE;
      globus_l_gfs_remote_data_info_free (my_handle->active_data_info);
      globus_gridftp_server_finished_command (my_handle->active_op, ipc_result, GLOBUS_NULL);
    }
    else
    {
      GlobusGFSErrorOpFinished(bounce_info->op, GLOBUS_GFS_OP_ACTIVE, ipc_result);
    }
    globus_free(bounce_info);
    globus_mutex_unlock (&my_handle->gfs_mutex);
  }

  static void globus_l_gfs_ipc_transfer_cb (globus_gfs_ipc_handle_t ipc_handle, globus_result_t ipc_result,
                                            globus_gfs_finished_info_t * reply, void * user_arg)
  {
    globus_l_gfs_xrootd_handle_t * my_handle;
    globus_l_gfs_remote_node_info_t * node_info;
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;
    globus_gfs_finished_info_t finished_info;
    globus_gfs_operation_t op;
    globus_bool_t finish = GLOBUS_FALSE;
    GlobusGFSName(__FUNCTION__);

    node_info = (globus_l_gfs_remote_node_info_t *) user_arg;
    bounce_info = node_info->bounce;
    my_handle = bounce_info->my_handle;

    globus_mutex_lock (&my_handle->gfs_mutex);
    {
      bounce_info->nodes_pending--;
      if (reply->result != GLOBUS_SUCCESS) bounce_info->cached_result = reply->result;

      if (!bounce_info->nodes_pending && !bounce_info->nodes_requesting)
      {
        if (my_handle->cur_result == GLOBUS_SUCCESS) my_handle->cur_result = bounce_info->cached_result;

        memset (&finished_info, 0, sizeof(globus_gfs_finished_info_t));
        finished_info.type = reply->type;
        finished_info.id = reply->id;
        finished_info.code = reply->code;
        finished_info.msg = reply->msg;
        finished_info.result = bounce_info->cached_result;
        finish = GLOBUS_TRUE;
        op = bounce_info->op;

        if (!bounce_info->events_enabled)
        {
          if (node_info->info && node_info->info_needs_free)
          {
            globus_free(node_info->info);
            node_info->info = NULL;
            node_info->info_needs_free = GLOBUS_FALSE;
          }
          if (bounce_info->eof_count != NULL)
          {
            globus_free(bounce_info->eof_count);
          }
          globus_free(bounce_info);
        }
      }
    }
    globus_mutex_unlock (&my_handle->gfs_mutex);

    if (finish)
    {
      globus_gridftp_server_operation_finished (op, finished_info.result, &finished_info);
    }
  }

  static void globus_l_gfs_ipc_event_cb (globus_gfs_ipc_handle_t ipc_handle, globus_result_t ipc_result, globus_gfs_event_info_t * reply,
                                         void * user_arg)
  {
    globus_l_gfs_xrootd_handle_t * my_handle;
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;
    globus_bool_t finish = GLOBUS_FALSE;
    globus_l_gfs_remote_node_info_t * current_node = NULL;
    globus_l_gfs_remote_node_info_t * master_node = NULL;
    globus_l_gfs_remote_node_info_t * node_info;
    globus_gfs_transfer_info_t * info;
    globus_gfs_event_info_t event_info;
    globus_result_t result;
    int ctr;
    GlobusGFSName(__FUNCTION__);

    node_info = (globus_l_gfs_remote_node_info_t *) user_arg;
    bounce_info = node_info->bounce;
    my_handle = bounce_info->my_handle;

    globus_mutex_lock (&my_handle->gfs_mutex);
    {
      switch (reply->type)
      {
        case GLOBUS_GFS_EVENT_TRANSFER_BEGIN:
          node_info->event_arg = reply->event_arg;
          node_info->event_mask = reply->event_mask;

          bounce_info->begin_event_pending--;
          if (!bounce_info->begin_event_pending)
          {
            if (!bounce_info->nodes_requesting)
            {
              bounce_info->events_enabled = GLOBUS_TRUE;
              reply->event_arg = bounce_info;
              reply->event_mask = GLOBUS_GFS_EVENT_TRANSFER_ABORT | GLOBUS_GFS_EVENT_TRANSFER_COMPLETE | GLOBUS_GFS_EVENT_BYTES_RECVD
                  | GLOBUS_GFS_EVENT_RANGES_RECVD;

              globus_gridftp_server_operation_event (bounce_info->op,
              GLOBUS_SUCCESS,
                                                     reply);
            }
          }
          break;
        case GLOBUS_GFS_EVENT_TRANSFER_CONNECTED:
          bounce_info->event_pending--;
          if (!bounce_info->event_pending && !bounce_info->nodes_requesting)
          {
            finish = GLOBUS_TRUE;
          }
          break;
        case GLOBUS_GFS_EVENT_PARTIAL_EOF_COUNT:
          info = (globus_gfs_transfer_info_t *) node_info->info;
          if (node_info->ipc_handle == ipc_handle)
          {
            globus_assert(info->node_ndx != 0 && current_node == NULL);
            current_node = node_info;
          }
          if (info->node_ndx == 0)
          {
            globus_assert(master_node == NULL);
            master_node = node_info;
          }
          for (ctr = 0; ctr < reply->node_count; ctr++)
          {
            bounce_info->eof_count[ctr] += reply->eof_count[ctr];
          }
          bounce_info->partial_eof_counts++;
          if (bounce_info->partial_eof_counts + 1 == bounce_info->node_count && !bounce_info->finished)
          {
            memset (&event_info, 0, sizeof(globus_gfs_event_info_t));
            event_info.type = GLOBUS_GFS_EVENT_FINAL_EOF_COUNT;
            event_info.event_arg = master_node->event_arg;
            event_info.eof_count = bounce_info->eof_count;
            event_info.node_count = bounce_info->partial_eof_counts + 1;
            result = globus_gfs_ipc_request_transfer_event (master_node->ipc_handle, &event_info);
            bounce_info->final_eof++;
          }
          break;
        default:
          if (!bounce_info->event_pending || reply->type == GLOBUS_GFS_EVENT_BYTES_RECVD || reply->type == GLOBUS_GFS_EVENT_RANGES_RECVD)
          {
            finish = GLOBUS_TRUE;
          }
          break;
      }
    }
    globus_mutex_unlock (&my_handle->gfs_mutex);

    if (finish)
    {
      reply->event_arg = bounce_info;
      globus_gridftp_server_operation_event (bounce_info->op,
      GLOBUS_SUCCESS,
                                             reply);
    }
  }

  static void globus_l_gfs_remote_passive_kickout (globus_l_gfs_remote_node_info_t * node_info, globus_result_t result, void * user_arg)
  {
    globus_bool_t finished = GLOBUS_FALSE;
    globus_l_gfs_xrootd_handle_t * my_handle;
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;

    GlobusGFSName(__FUNCTION__);

    bounce_info = (globus_l_gfs_remote_ipc_bounce_t *) user_arg;
    my_handle = (globus_l_gfs_xrootd_handle_t *) bounce_info->my_handle;

    globus_mutex_lock (&my_handle->gfs_mutex);
    {
      bounce_info->nodes_requesting--;

      if (result != GLOBUS_SUCCESS) goto error;

      node_info->bounce = bounce_info;

      result = globus_gfs_ipc_request_passive_data (node_info->ipc_handle, (globus_gfs_data_info_t *) bounce_info->state,
                                                    globus_l_gfs_ipc_passive_cb, node_info);
      if (result != GLOBUS_SUCCESS)
      {
        goto error;
      }
      bounce_info->nodes_pending++;
      bounce_info->node_info = node_info;
    }
    globus_mutex_unlock (&my_handle->gfs_mutex);

    return;

    error:
    /* if no nodes were obtained and none are outstanding */
    if (bounce_info->nodes_requesting == 0 && bounce_info->nodes_pending == 0 && bounce_info->nodes_obtained == 0)
    {
      finished = GLOBUS_TRUE;
    }
    globus_mutex_unlock (&my_handle->gfs_mutex);
    if (finished)
    {
      GlobusGFSErrorOpFinished(bounce_info->op, GLOBUS_GFS_OP_PASSIVE, result);
    }
  }

  static void globus_l_gfs_remote_active_kickout (globus_l_gfs_remote_node_info_t * node_info, globus_result_t result, void * user_arg)
  {
    globus_bool_t finish = GLOBUS_FALSE;
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;
    globus_gfs_data_info_t * data_info;
    globus_gfs_data_info_t * new_data_info;
    globus_l_gfs_xrootd_handle_t * my_handle;

    GlobusGFSName(__FUNCTION__);

    bounce_info = (globus_l_gfs_remote_ipc_bounce_t *) user_arg;
    data_info = (globus_gfs_data_info_t *) bounce_info->state;
    my_handle = bounce_info->my_handle;

    globus_mutex_lock (&my_handle->gfs_mutex);
    {
      bounce_info->nodes_requesting--;

      if (result != GLOBUS_SUCCESS) goto error;

      node_info->bounce = bounce_info;

      new_data_info = (globus_gfs_data_info_t *) globus_calloc(1, sizeof(globus_gfs_data_info_t));

      memcpy (new_data_info, bounce_info->state, sizeof(globus_gfs_data_info_t));

      new_data_info->cs_count = 1;
      new_data_info->contact_strings = (const char **) calloc (1, sizeof(char *));
      new_data_info->contact_strings[0] = globus_libc_strdup (data_info->contact_strings[bounce_info->node_ndx]);

      node_info->info = new_data_info;
      node_info->info_needs_free = GLOBUS_TRUE;
      result = globus_gfs_ipc_request_active_data (node_info->ipc_handle, new_data_info, globus_l_gfs_ipc_active_cb, node_info);
      if (result != GLOBUS_SUCCESS) goto error;

      node_info->node_ndx = bounce_info->node_ndx;
      bounce_info->node_info = node_info;
      bounce_info->node_ndx++;
      bounce_info->nodes_pending++;
    }
    globus_mutex_unlock (&my_handle->gfs_mutex);

    return;

    error:
    /* if no nodes were obtained and none are outstanding */
    if (bounce_info->nodes_requesting == 0 && bounce_info->nodes_pending == 0 && bounce_info->nodes_obtained == 0)
    {
      finish = GLOBUS_TRUE;
    }

    if (finish)
    {
      if (my_handle->active_delay)
      {
        /* we have to fake a failure of a different operation */
        my_handle->active_delay = GLOBUS_FALSE;
        globus_l_gfs_remote_data_info_free (my_handle->active_data_info);
        globus_gridftp_server_finished_command (my_handle->active_op, result, GLOBUS_NULL);
      }
      else
      {
        GlobusGFSErrorOpFinished(bounce_info->op, GLOBUS_GFS_OP_ACTIVE, result);
      }
      globus_free(bounce_info);
    }
    globus_mutex_unlock (&my_handle->gfs_mutex);
  }

  static void globus_l_gfs_xrootd_remote_list (globus_gfs_operation_t op, globus_gfs_transfer_info_t * transfer_info, void * user_arg)
  {
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;
    globus_result_t result;
    globus_l_gfs_xrootd_handle_t * my_handle;
    globus_l_gfs_remote_node_info_t * node_info;

    GlobusGFSName(__FUNCTION__);

    my_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;

    if (my_handle->active_delay)
    {
      /* Request active connection from the right node first */
      globus_mutex_lock (&my_handle->gfs_mutex);

      my_handle->mode = XROOTD_FILEMODE_NONE;

      result = globus_l_gfs_remote_init_bounce_info (&bounce_info, op, my_handle->active_data_info, my_handle);

      if (result != GLOBUS_SUCCESS) goto error;

      bounce_info->nodes_requesting = 1;

      result = globus_l_gfs_remote_node_request (my_handle, transfer_info->pathname, globus_l_gfs_remote_active_kickout, bounce_info);

      if (result != GLOBUS_SUCCESS)
      {
        globus_free(bounce_info);
        goto error;
      }

      my_handle->active_callback = globus_l_gfs_xrootd_remote_list;
      my_handle->active_op = op;
      my_handle->active_transfer_info = transfer_info;
      my_handle->active_user_arg = user_arg;

      globus_mutex_unlock (&my_handle->gfs_mutex);
      return;
      /* Will be back here from active callback with delayed flag off */
    }

    /* XXX it appears no lock is needed here */
    result = globus_l_gfs_remote_init_bounce_info (&bounce_info, op, transfer_info, my_handle);

    node_info = (struct globus_l_gfs_remote_node_info_s *) transfer_info->data_arg;

    transfer_info->data_arg = node_info->data_arg;
    transfer_info->stripe_count = 1;
    transfer_info->node_ndx = 0;
    transfer_info->node_count = 1;
    bounce_info->node_info = node_info;
    bounce_info->event_pending = 1;
    bounce_info->begin_event_pending = 1;
    bounce_info->nodes_pending = 1;
    bounce_info->node_count = 1;
    node_info->info = NULL;
    node_info->info_needs_free = GLOBUS_FALSE;
    node_info->bounce = bounce_info;

    result = globus_gfs_ipc_request_list (node_info->ipc_handle, transfer_info, globus_l_gfs_ipc_transfer_cb, globus_l_gfs_ipc_event_cb,
                                          node_info);

    if (result == GLOBUS_SUCCESS) return;

    error:
    GlobusGFSErrorOpFinished(bounce_info->op, GLOBUS_GFS_OP_TRANSFER, result);
  }

  static void globus_l_gfs_xrootd_remote_send (globus_gfs_operation_t op, globus_gfs_transfer_info_t * transfer_info, void * user_arg)
  {
    globus_l_gfs_remote_ipc_bounce_t * bounce_info = NULL;
    globus_result_t result;
    globus_l_gfs_xrootd_handle_t * my_handle;
    globus_l_gfs_remote_node_info_t * node_info;
    globus_gfs_transfer_info_t * new_transfer_info;
    GlobusGFSName(__FUNCTION__);

    my_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;

    globus_mutex_lock (&my_handle->gfs_mutex);

    if (my_handle->active_delay)
    {
      /* Request active connection from the right node first */
      my_handle->mode = XROOTD_FILEMODE_READING;

      result = globus_l_gfs_remote_init_bounce_info (&bounce_info, op, my_handle->active_data_info, my_handle);

      if (result != GLOBUS_SUCCESS) goto error;

      bounce_info->nodes_requesting = 1;

      result = globus_l_gfs_remote_node_request (my_handle, transfer_info->pathname, globus_l_gfs_remote_active_kickout, bounce_info);

      if (result != GLOBUS_SUCCESS)
      {
        globus_free(bounce_info);
        goto error;
      }

      my_handle->active_transfer_info = transfer_info;
      my_handle->active_op = op;
      my_handle->active_user_arg = user_arg;
      my_handle->active_callback = globus_l_gfs_xrootd_remote_send;

      globus_mutex_unlock (&my_handle->gfs_mutex);

      return;
      /* Will be back here from active callback with delayed flag off */
    }

    result = globus_l_gfs_remote_init_bounce_info (&bounce_info, op, transfer_info, my_handle);

    node_info = (struct globus_l_gfs_remote_node_info_s *) transfer_info->data_arg;

    bounce_info->eof_count = (int *) globus_calloc(1, sizeof(int) + 1);

    bounce_info->nodes_requesting = 1;
    bounce_info->node_count = 1;
    bounce_info->sending = GLOBUS_TRUE;
    bounce_info->node_info = node_info;

    new_transfer_info = (globus_gfs_transfer_info_t *) globus_calloc(1, sizeof(globus_gfs_transfer_info_t));
    memcpy (new_transfer_info, transfer_info, sizeof(globus_gfs_transfer_info_t));

    // We rely on the xrootd server to notice that a local replica is available on the selected backend gridftp server and redirect the access to the local
    globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "send: requesting transfer of %s to %s\n", new_transfer_info->pathname,
                            my_handle->session_info.host_id);

    new_transfer_info->data_arg = node_info->data_arg;
    new_transfer_info->node_count = 1;
    new_transfer_info->stripe_count = node_info->stripe_count;
    new_transfer_info->node_ndx = 0;
    node_info->info = new_transfer_info;
    node_info->info_needs_free = GLOBUS_TRUE;
    node_info->bounce = bounce_info;

    result = globus_gfs_ipc_request_send (node_info->ipc_handle, new_transfer_info, globus_l_gfs_ipc_transfer_cb, globus_l_gfs_ipc_event_cb,
                                          node_info);
    if (result != GLOBUS_SUCCESS) goto error;
    bounce_info->nodes_pending++;
    bounce_info->event_pending++;
    bounce_info->begin_event_pending++;
    bounce_info->nodes_requesting--;

    globus_mutex_unlock (&my_handle->gfs_mutex);

    return;

    error: my_handle->cur_result = result;
    globus_mutex_unlock (&my_handle->gfs_mutex);
    GlobusGFSErrorOpFinished(bounce_info->op, GLOBUS_GFS_OP_TRANSFER, result);
  }

  static void globus_l_gfs_xrootd_remote_recv (globus_gfs_operation_t op, globus_gfs_transfer_info_t * transfer_info, void * user_arg)
  {
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;
    globus_result_t result;
    globus_l_gfs_xrootd_handle_t * my_handle;
    globus_l_gfs_remote_node_info_t * node_info;
    globus_gfs_transfer_info_t * new_transfer_info;
    GlobusGFSName(__FUNCTION__);

    my_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;

    globus_mutex_lock (&my_handle->gfs_mutex);

    if (my_handle->active_delay)
    {
      /* Request active connection from the right node first */

      if (transfer_info->truncate)
        my_handle->mode = XROOTD_FILEMODE_TRUNCATE;
      else
        my_handle->mode = XROOTD_FILEMODE_WRITING;

      result = globus_l_gfs_remote_init_bounce_info (&bounce_info, op, my_handle->active_data_info, my_handle);

      if (result != GLOBUS_SUCCESS) goto error;

      bounce_info->nodes_requesting = 1;

      result = globus_l_gfs_remote_node_request (my_handle, transfer_info->pathname, globus_l_gfs_remote_active_kickout, bounce_info);

      if (result != GLOBUS_SUCCESS)
      {
        globus_free(bounce_info);
        goto error;
      }

      my_handle->active_transfer_info = transfer_info;
      my_handle->active_op = op;
      my_handle->active_user_arg = user_arg;
      my_handle->active_callback = globus_l_gfs_xrootd_remote_recv;

      globus_mutex_unlock (&my_handle->gfs_mutex);

      return;
      /* Will be back here from active callback with delayed flag off */
    }

    result = globus_l_gfs_remote_init_bounce_info (&bounce_info, op, transfer_info, my_handle);

    node_info = (struct globus_l_gfs_remote_node_info_s *) transfer_info->data_arg;

    bounce_info->nodes_requesting = 1;
    bounce_info->node_count = 1;
    bounce_info->node_info = node_info;

    new_transfer_info = (globus_gfs_transfer_info_t *) globus_calloc(1, sizeof(globus_gfs_transfer_info_t));
    memcpy (new_transfer_info, transfer_info, sizeof(globus_gfs_transfer_info_t));

    //TODO: We rely on the xrootd server to notice that a local replica is available on the remote server and redirect the access to the local
    //      If it's not enough, is there a mechanism to pass an Xrootd address to the remote GridFTP server
    globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "recv: requesting transfer of %s to %s\n", new_transfer_info->pathname,
                            my_handle->session_info.host_id);

    new_transfer_info->data_arg = node_info->data_arg;
    new_transfer_info->node_count = 1;
    new_transfer_info->stripe_count = node_info->stripe_count;
    new_transfer_info->node_ndx = 0;
    node_info->info = new_transfer_info;
    node_info->info_needs_free = GLOBUS_TRUE;
    node_info->bounce = bounce_info;

    result = globus_gfs_ipc_request_recv (node_info->ipc_handle, new_transfer_info, globus_l_gfs_ipc_transfer_cb, globus_l_gfs_ipc_event_cb,
                                          node_info);
    if (result != GLOBUS_SUCCESS) goto error;
    /* could maybe get away with no lock if we moved the next few lines
     above the request.  we would have to then assume that the
     values were meaningless under error.  This way is more
     consistant and the lock is not very costly */
    bounce_info->nodes_pending++;
    bounce_info->event_pending++;
    bounce_info->begin_event_pending++;
    bounce_info->nodes_requesting--;

    globus_mutex_unlock (&my_handle->gfs_mutex);

    return;

    error: my_handle->cur_result = result;
    globus_mutex_unlock (&my_handle->gfs_mutex);
    GlobusGFSErrorOpFinished(bounce_info->op, GLOBUS_GFS_OP_TRANSFER, result);
  }

  static void globus_l_gfs_xrootd_remote_trev (globus_gfs_event_info_t * event_info, void * user_arg)
  {
    globus_result_t result;
    globus_l_gfs_xrootd_handle_t * my_handle;
    globus_l_gfs_remote_node_info_t * node_info;
    globus_gfs_event_info_t new_event_info;
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;

    GlobusGFSName(__FUNCTION__);

    bounce_info = (globus_l_gfs_remote_ipc_bounce_t *) event_info->event_arg;
    node_info = bounce_info->node_info;
    my_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;

    memset (&new_event_info, 0, sizeof(globus_gfs_event_info_t));
    new_event_info.type = event_info->type;
    new_event_info.event_arg = node_info->event_arg;

    result = globus_gfs_ipc_request_transfer_event (node_info->ipc_handle, &new_event_info);

    globus_mutex_lock (&my_handle->gfs_mutex);

    if (event_info->type == GLOBUS_GFS_EVENT_TRANSFER_COMPLETE)
    {

      node_info = bounce_info->node_info;

      if (node_info->info && node_info->info_needs_free)
      {
        globus_free(node_info->info);
        node_info->info = NULL;
        node_info->info_needs_free = GLOBUS_FALSE;
      }
      node_info->event_arg = NULL;
      node_info->event_mask = 0;
      if (bounce_info->eof_count != NULL)
      {
        globus_free(bounce_info->eof_count);
      }
      globus_free(bounce_info);

      /* Finished adding a new replica */
    }
    globus_mutex_unlock (&my_handle->gfs_mutex);
  }

  static void globus_l_gfs_xrootd_remote_active_delay (globus_gfs_operation_t op, globus_gfs_data_info_t * data_info, void * user_arg)
  {
    globus_gfs_data_info_t * new_data_info;
    globus_l_gfs_xrootd_handle_t * my_handle;
    globus_gfs_finished_info_t finished_info;
    int idx;
    GlobusGFSName(__FUNCTION__);

    my_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;

    globus_mutex_lock (&my_handle->gfs_mutex);
    my_handle->cur_result = GLOBUS_SUCCESS;
    my_handle->active_delay = GLOBUS_TRUE;

    new_data_info = (globus_gfs_data_info_t *) globus_calloc(1, sizeof(globus_gfs_data_info_t));
    memcpy (new_data_info, data_info, sizeof(globus_gfs_data_info_t));
    new_data_info->subject = globus_libc_strdup (data_info->subject);
    new_data_info->interface = globus_libc_strdup (data_info->interface);
    new_data_info->pathname = globus_libc_strdup (data_info->pathname);
    new_data_info->contact_strings = (const char **) calloc (data_info->cs_count, sizeof(char *));
    for (idx = 0; idx < data_info->cs_count; idx++)
      new_data_info->contact_strings[idx] = globus_libc_strdup (data_info->contact_strings[idx]);

    my_handle->active_data_info = new_data_info; /* It must be freed later */

    /* we must return OK here and initiate active connection from
     send()/recv()/list() when file name and remote node are known */

    memset (&finished_info, 0, sizeof(globus_gfs_finished_info_t));
    finished_info.type = GLOBUS_GFS_OP_ACTIVE;
    finished_info.result = GLOBUS_SUCCESS;
    finished_info.info.data.bi_directional = GLOBUS_TRUE;

    globus_gridftp_server_operation_finished (op, finished_info.result, &finished_info);

    globus_mutex_unlock (&my_handle->gfs_mutex);
  }

  static void globus_l_gfs_xrootd_remote_passive (globus_gfs_operation_t op, globus_gfs_data_info_t * data_info, void * user_arg)
  {
    globus_l_gfs_remote_ipc_bounce_t * bounce_info;
    globus_result_t result;
    globus_l_gfs_xrootd_handle_t * my_handle;
    GlobusGFSName(__FUNCTION__);
    char cmd[5];

    my_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;

    result = globus_l_gfs_remote_init_bounce_info (&bounce_info, op, data_info, my_handle);

    if (result != GLOBUS_SUCCESS) goto error;

    bounce_info->nodes_requesting = 1;

    globus_mutex_lock (&my_handle->gfs_mutex);
    my_handle->cur_result = GLOBUS_SUCCESS;

    /*  **** THIS IS A DIRTY HACK ****
     It's the only reason why we need to include "globus_gfs_internal_hack.h" with internal structures.
     There's no other way to tell whether we are reading a file or writing it in delayed passive mode.
     */
    my_handle->mode = XROOTD_FILEMODE_NONE;
    if (op->user_arg)
    {
      strncpy (cmd, ((globus_l_gfs_request_info_t *) op->user_arg)->control_op->command, sizeof(cmd) - 1);
      globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "Passive mode was triggered by command %s\n", cmd);
      if (!strcmp (cmd, "STOR") || !strcmp (cmd, "ESTO"))
      {
        my_handle->mode = XROOTD_FILEMODE_TRUNCATE;
      }
      else if (!strcmp (cmd, "APPE"))
      {
        my_handle->mode = XROOTD_FILEMODE_WRITING;
      }
      else if (!strcmp (cmd, "RETR") || !strcmp (cmd, "ERET"))
      {
        my_handle->mode = XROOTD_FILEMODE_READING;
      }
      else
      {
        my_handle->mode = XROOTD_FILEMODE_NONE; /* LIST and PASV in legacy passive mode */
      }
    }

    result = globus_l_gfs_remote_node_request (my_handle, data_info->pathname, globus_l_gfs_remote_passive_kickout, bounce_info);

    globus_mutex_unlock (&my_handle->gfs_mutex);

    if (result == GLOBUS_SUCCESS) return;

    globus_free(bounce_info);

    error:
    GlobusGFSErrorOpFinished(op, GLOBUS_GFS_OP_PASSIVE, result);
  }

  static void globus_l_gfs_xrootd_remote_data_destroy (void * data_arg, void * user_arg)
  {
    globus_result_t result;
    globus_l_gfs_xrootd_handle_t * my_handle;
    globus_l_gfs_remote_node_info_t * node_info;
    GlobusGFSName(__FUNCTION__);

    my_handle = (globus_l_gfs_xrootd_handle_t *) user_arg;
    node_info = (struct globus_l_gfs_remote_node_info_s *) data_arg;

    globus_mutex_lock (&my_handle->gfs_mutex);

    if (node_info && my_handle && !my_handle->active_delay)
    {
      result = globus_gfs_ipc_request_data_destroy (node_info->ipc_handle, node_info->data_arg);
      if (result != GLOBUS_SUCCESS)
      {
        globus_gfs_log_result (GLOBUS_GFS_LOG_ERR, "IPC ERROR: remote_data_destroy: ipc call", result);
      }
      if (node_info->cs != NULL)
      {
        globus_free(node_info->cs);
      }
      node_info->data_arg = NULL;
      node_info->stripe_count = 0;
      result = globus_l_gfs_remote_node_release (node_info, GLOBUS_GFS_BRAIN_REASON_COMPLETE);
      if (result != GLOBUS_SUCCESS)
      {
        globus_gfs_log_result (GLOBUS_GFS_LOG_ERR, "ERROR: remote_data_destroy: handle_release", result);
      }
    }
    globus_mutex_unlock (&my_handle->gfs_mutex);
  }

  static int
  globus_l_gfs_xrootd_activate (void);

  static int
  globus_l_gfs_xrootd_deactivate (void);

  ///================================================
  /// DSI FOR STANDALONE GRIDFTP SERVER CONFIGURATION
  ///================================================
  static globus_gfs_storage_iface_t globus_l_gfs_xrootd_dsi_iface =
  {
  GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING | GLOBUS_GFS_DSI_DESCRIPTOR_SENDER, globus_l_gfs_xrootd_start, globus_l_gfs_xrootd_destroy, NULL, /* list */
  globus_l_gfs_xrootd_send, globus_l_gfs_xrootd_recv, NULL, /* trev */
  NULL, /* active */
  NULL, /* passive */
  NULL, /* data destroy */
  globus_l_gfs_xrootd_command, globus_l_gfs_xrootd_stat, NULL, NULL };
  GlobusExtensionDefineModule (globus_gridftp_server_xrootd) =
  { (char*) "globus_gridftp_server_xrootd", globus_l_gfs_xrootd_activate, globus_l_gfs_xrootd_deactivate,
  NULL,
  NULL, &local_version,
  NULL };

  ///========================================================
  /// DSI FOR FRONTEND/BACKEND GRID FTP SERVERS CONFIGURATION
  ///========================================================
  /// in this configuration, all the commands excepted
  ///   send,recv,list are served by the frontend server
  ///   the data channel involved for those three commands
  ///   is created between the client and one of the backend servers
  ///   depending on the configuration, available backend servers
  ///   are either part of a static list, either auto discovered
  ///   (in that case Xrd source servers are checked for being gridftp servers)
  ///   among the available backend servers, selected servers are chosen as follows
  ///   Generic Xrootd Behaviour.
  ///   RECV:
  ///   - First all the XRD severs hosting a replica are looked up (in a pure XRoot way or in an eos-specific way
  ///     according to the configuration). Then, are kept only the ones runnning a background gridftp server.
  ///     If at least one is found. Pick-up one randomly and forward to transfer to this one.
  ///   - if no located XRD server is a gridftp server, just use one of the
  ///     globally known backend gridftp servers for RECV
  ///   SEND: it's a bit more tricky to make the operation atomic
  ///   - A temporary file is created
  ///   - First all the XRD severs hosting a replica of these are looked up (in a pure XRoot way or in an eos-specific way
  ///     according to the configuration). Then, are kept only the ones runnning a background gridftp server.
  ///     If at least one is found. Pick-up one randomly and forward to transfer to this one.
  ///   - if no located XRD server is a gridftp server, just use one of the
  ///     globally known backend gridftp servers for RECV
  ///   - at the end of the transfer, if it's sucessful, delete the previous target file (if any)
  ///     and rename the temporary file to the target file name
  static globus_gfs_storage_iface_t globus_l_gfs_xrootd_remote_dsi_iface =
  {
  GLOBUS_GFS_DSI_DESCRIPTOR_BLOCKING | GLOBUS_GFS_DSI_DESCRIPTOR_SENDER, globus_l_gfs_xrootd_start, globus_l_gfs_xrootd_destroy,
      globus_l_gfs_xrootd_remote_list, globus_l_gfs_xrootd_remote_send, globus_l_gfs_xrootd_remote_recv, globus_l_gfs_xrootd_remote_trev,
      globus_l_gfs_xrootd_remote_active_delay, globus_l_gfs_xrootd_remote_passive, globus_l_gfs_xrootd_remote_data_destroy,
      globus_l_gfs_xrootd_command, globus_l_gfs_xrootd_stat,
      NULL,
      NULL };

  struct sigaction globus_sa_sigint, globus_sa_sigterm, globus_sa_sigsegv;

  static void globus_l_gfs_xrootd_sighandler (int sig)
  {
    dbgprintf("My PID is %d , signal is sig %d\n", (int) getpid (), sig);

    if (config)
    //globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "%s: My PID is %d\n", "globus_l_gfs_xrootd_deactivate", (int)getpid());
    {
      delete config;
      config = NULL;
    }

    if (sig == 0) return;

    struct sigaction *sa = NULL;
    if (sig == SIGINT) sa = &globus_sa_sigint;
    if (sig == SIGTERM) sa = &globus_sa_sigterm;
    if (sig == SIGSEGV) sa = &globus_sa_sigsegv;

    if (sa && sa->sa_handler)
    {
      dbgprintf("Calling previous signal handler for signal %d. sa is %p and previous handler is %p\n", sig, sa, sa?sa->sa_handler:NULL);
      sa->sa_handler (sig);
      dbgprintf("Succesfully called previous signal handler for signal %d. sa is %p and previous handler is %p\n", sig, sa,
                sa?sa->sa_handler:NULL);
    }
    else
    {
      dbgprintf("I could not find the previous signal handler for signal %d. sa is %p and previous handler is %p\n", sig, sa,
                sa?sa->sa_handler:NULL);
      if (sig == SIGINT || sig == SIGTERM) exit (0);
      if (sig == SIGSEGV) abort ();
    }
  }

  static void globus_l_gfs_xrootd_atexit ()
  {
    globus_l_gfs_xrootd_sighandler (0);
  }

  static int globus_l_gfs_xrootd_activate (void)
  {
    // ### NOT CALLED for every process globus_gridftp_sever ### //
    dbgprintf("My PID is %d, XrdGsiBackendMapper::This is %p and config is %p\n", (int)getpid(), XrdGsiBackendMapper::This, config);

    // we register a few hooks to catch any exit of the program to leave all the shared stuff in a consistent state
    sigaction (SIGINT, NULL, &globus_sa_sigint);
    sigaction (SIGTERM, NULL, &globus_sa_sigterm);
    sigaction (SIGSEGV, NULL, &globus_sa_sigsegv);
    signal (SIGINT, globus_l_gfs_xrootd_sighandler);
    signal (SIGTERM, globus_l_gfs_xrootd_sighandler);
    signal (SIGSEGV, globus_l_gfs_xrootd_sighandler);
    atexit (globus_l_gfs_xrootd_atexit);

    // hadPreviousCfs is actually useless as on every fork, the plugin is reloaded
    // this means that all the static variables are re initialized on each fork
    bool hadPreeviousCfg = (config != 0);
    if (!hadPreeviousCfg) config = new globus_l_gfs_xrootd_config;

    // #### SETTING UP AND REPORTING THINGS INDEPENDANT OF STANDALONE/BACKEND/FRONTEND #### //
    ReadaheadBlock::sDefaultBlocksize = config->XrdReadAheadBlockSize;
    XrdFileIo::sNumRdAheadBlocks = config->XrdReadAheadNBlocks;
    globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "%s: My PID is %d\n", "globus_l_gfs_xrootd_activate", (int) getpid ());
    globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "%s: My Environment is as follow : \n", "globus_l_gfs_xrootd_activate");
    for (char **env = environ; *env; ++env)
      globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "%s\n", *env);
    globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "%s: Activating XRootD DSI plugin\n", "globus_l_gfs_xrootd_activate");
    if (config->XrootdVmp.empty ())
    {
      globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: XRootD Virtual Mount Point is NOT set. DSI plugin cannot start. \n",
                              "globus_l_gfs_xrootd_activate");
      return 1;
    }
    if (config->ServerRole.empty ()
        || (config->ServerRole != "frontend" && config->ServerRole != "backend" && config->ServerRole != "standalone"))
    {
      globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: XRootD DSI GridFtp server role is NOT set. DSI plugin cannot start. \n",
                              "globus_l_gfs_xrootd_activate");
      return 1;
    }
    globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "%s: XRootD Virtual Mount Point is set to: %s\n", "globus_l_gfs_xrootd_activate",
                            config->XrootdVmp.c_str ());
    {
      const size_t errBuffLen = 2048;
      char errBuff[errBuffLen];

      if (!XP.getParseErrStr ().empty ())
      {
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: Error parsing Virtual Mount Point : %s. DSI plugin cannot start. \n",
                                "globus_l_gfs_xrootd_activate", XP.getParseErrStr ().c_str ());
        return 1;
      }

      if (!XP.CheckVMP (errBuff, errBuffLen))
      {
        globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: Error : %s. DSI plugin cannot start. \n", "globus_l_gfs_xrootd_activate", errBuff);
        return 1;
      }
    }
    globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "%s: XRootD Read Ahead Block Size is set to: %d\n", "globus_l_gfs_xrootd_activate",
                            config->XrdReadAheadBlockSize);
    globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "%s: XRootD number of Read Ahead Blocks is set to: %d\n", "globus_l_gfs_xrootd_activate",
                            config->XrdReadAheadNBlocks);
    std::stringstream ss;
    if (config->EosAppTag) ss << " EosAppTag";
    if (config->EosChmod) ss << " EosChmod";
    if (config->EosCks) ss << " EosCks";
    if (config->EosBook) ss << " EosBook";
    if (config->EosRemote) ss << " EosRemote";
    std::string eosspec (ss.str ());
    if (eosspec.size ())
    {
      ss.str ("");
      ss << "globus_l_gfs_xrootd_activate: XRootD DSI plugin runs the following EOS specifics:";
      ss << eosspec << std::endl;
      globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, ss.str ().c_str ());
    }
    // ######################################################################################## //

    // #### SETTING UP AND REPORTING THINGS IN FRONTEND MODE #### //
    if (config->ServerRole == "frontend")
    {
      if (config->BackendServersDiscoveryTTL == 0 && config->allTheServers.size () == 0)
      {
        globus_gfs_log_message (
            GLOBUS_GFS_LOG_ERR,
            "%s: Error : enabling Frontend/Backend mode : backend discovery is off and backend hosts list is empty. There is no backend! Abort.\n",
            "globus_l_gfs_xrootd_activate", config->ServerRole.c_str ());
        return 1;
      }
      ss.str ("");
      ss << "globus_l_gfs_xrootd_activate: XRootD DSI plugin runs in Frontend-Backend mode:" << std::endl;
      if (config->BackendServersDiscoveryTTL > 0)
      {
        ss << "using backend autodiscovery TTL = " << config->BackendServersDiscoveryTTL << " sec." << std::endl;
        globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, ss.str ().c_str ());
      }
      if (config->allTheServers.size ())
      {
        ss << "using backend servers = ";
        for (auto it = config->allTheServers.begin (); it != config->allTheServers.end (); it++)
        {
          if (it != config->allTheServers.begin ()) ss << " , ";
          ss << *it;
        }
        ss << std::endl;
        globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, ss.str ().c_str ());
      }

      if (!hadPreeviousCfg)
      {
        int cpt = 0;
        do
        {
          int oldstate;
          pthread_setcancelstate (PTHREAD_CANCEL_DISABLE, &oldstate);
          XrdGsiBackendMapper::This->LockBackendServers ();
          if (XrdGsiBackendMapper::This->GetActiveBackEnd ()->size ())
          {
            XrdGsiBackendMapper::This->UnLockBackendServers ();
            pthread_setcancelstate (oldstate, NULL);
            break;
          }
          //time_t now=time(0);
          //globus_gfs_log_message (GLOBUS_GFS_LOG_DUMP, "now is %d, XrdGsiBackendMapper::This.GetBackEndServers()->size () %d\n",now,XrdGsiBackendMapper::This->GetActiveBackEnd()->size ());
          XrdGsiBackendMapper::This->UnLockBackendServers ();
          pthread_setcancelstate (oldstate, NULL);
          sleep (1);
          if (++cpt == 10)
          {
            globus_gfs_log_message (GLOBUS_GFS_LOG_ERR,
                                    "%s: could not successfully probe any backend server. There is no backend! Abort.\n",
                                    "globus_l_gfs_xrootd_activate");
            return 1;
          }
        }
        while (true);

        globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, "%s: Early startup backend list is \n %s\n", "globus_l_gfs_xrootd_activate",
                                XrdGsiBackendMapper::This->DumpActiveBackend ().c_str ());

        if (config->EosNodeLs)
        {
          std::vector<std::string> HeadNodeServersV;
          std::string HeadNodeServersS;
          XP.GetServerList (&HeadNodeServersV, &HeadNodeServersS);
          XrdGsiBackendMapper::This->AddToProbeList ("eos_node_ls" + HeadNodeServersS);
        }

        if (config->BackendServersDiscoveryTTL > 0) XrdGsiBackendMapper::This->StartUpdater ();
      }

      if ((config->EosAppTag || config->EosBook || config->EosChmod || config->EosCks || config->EosNodeLs) && !config->EosRemote
          && config->BackendServersDiscoveryTTL > 0)
      {
        globus_gfs_log_message (
            GLOBUS_GFS_LOG_WARN,
            "%s: Warning : The server is run in frontend mode with some EOS specifics and backend discovery is enabled. Most likely, the backend storage is an EOS instance. In that case, backend discovery won't work properly unless the environment variable XROOTD_DSI_EOS_REMOTE is set.  \n",
            "globus_l_gfs_xrootd_activate");
      }

      globus_extension_registry_add (GLOBUS_GFS_DSI_REGISTRY, (void*) "xrootd", GlobusExtensionMyModule(globus_gridftp_server_xrootd),
                                     &globus_l_gfs_xrootd_remote_dsi_iface);
    }
    // ######################################################################################## //

    // #### SETTING UP AND REPORTING THINGS IN BACKEND/STANDALONE MODE #### //
    else if (config->ServerRole == "backend" || config->ServerRole == "standalone")
    {
      if (config->BackendServersDiscoveryTTL > 0 || config->allTheServers.size () > 0)
      {
        globus_gfs_log_message (
            GLOBUS_GFS_LOG_WARN,
            "%s: Warning : enabling Frontend/Backend mode : backend autodiscovery configured TTL or static backend nodes list given in the config but ServerRole is not frontend! Those parameters will be ignored. \n",
            "globus_l_gfs_xrootd_activate");
      }
      globus_extension_registry_add (GLOBUS_GFS_DSI_REGISTRY, (void*) "xrootd", GlobusExtensionMyModule(globus_gridftp_server_xrootd),
                                     &globus_l_gfs_xrootd_dsi_iface);
    }
    // ######################################################################################## //

    else
    {
      globus_gfs_log_message (GLOBUS_GFS_LOG_ERR, "%s: Error : invalid role %s : valid roles are 'frontend', 'backend', and 'standalone'\n",
                              "globus_l_gfs_xrootd_activate", config->ServerRole.c_str ());
      return 1;
    }
    ss.str ("");
    ss << "globus_l_gfs_xrootd_activate: XRootD DSI plugin runs in " << config->ServerRole << " mode:" << std::endl;
    globus_gfs_log_message (GLOBUS_GFS_LOG_INFO, ss.str ().c_str ());

    return 0;
  }

  static int globus_l_gfs_xrootd_deactivate (void)
  {
    // ### NOT CALLED ON ALL FORKS ### //
    // never use globus_gfs_log_message, it could deadlock!!!!!
    dbgprintf("My PID is %d\n", (int) getpid ());

    globus_extension_registry_remove (GLOBUS_GFS_DSI_REGISTRY, (void*) "xrootd");

    if (config)
    {
      delete config;
      config = NULL;
    }

    return 0;
  }
}// end extern "C"
