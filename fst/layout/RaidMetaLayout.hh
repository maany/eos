//------------------------------------------------------------------------------
//! @file RaidMetaLayout.hh
//! @author Elvin-Alin Sindrilaru <esindril@cern.ch>
//! @brief Generic class to read/write RAID-like layout files using a gateway
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
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

#ifndef __EOSFST_RAIDMETALAYOUT_HH__
#define __EOSFST_RAIDMETALAYOUT_HH__

#include <vector>
#include <string>
#include <list>
#include "fst/layout/Layout.hh"
#include "fst/layout/HeaderCRC.hh"
#include "fst/XrdFstOfsFile.hh"

EOSFSTNAMESPACE_BEGIN

//------------------------------------------------------------------------------
//! Generic class to read/write different RAID-like layout files
//------------------------------------------------------------------------------
class RaidMetaLayout : public Layout
{
public:

  //----------------------------------------------------------------------------
  //! Constructor
  //!
  //! @param file handler to current file
  //! @param lid layout id
  //! @param client security information
  //! @param outError error information
  //! @param io access type
  //! @param timeout timeout value
  //! @param storeRecovery force writing back the recovered blocks to the files
  //! @param targetSize initial file size
  //! @param bookingOpaque opaque information
  //!
  //----------------------------------------------------------------------------
  RaidMetaLayout(XrdFstOfsFile* file,
                 unsigned long lid,
                 const XrdSecEntity* client,
                 XrdOucErrInfo* outError,
                 const char* path,
                 uint16_t timeout,
                 bool storeRecovery,
                 off_t targetSize,
                 std::string bookingOpaque);


  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  virtual ~RaidMetaLayout();

  //--------------------------------------------------------------------------
  //! Redirect to new target
  //--------------------------------------------------------------------------

  virtual void Redirect(const char*);

  //--------------------------------------------------------------------------
  //! Open file using a gateway
  //!
  //! @param flags flags O_RDWR/O_RDONLY/O_WRONLY
  //! @param mode creation permissions
  //! @param opaque opaque information
  //!
  //! @return 0 if successful, -1 otherwise and error code is set
  //!
  //----------------------------------------------------------------------------
  virtual int Open(
    XrdSfsFileOpenMode flags,
    mode_t mode,
    const char* opaque);

  //----------------------------------------------------------------------------
  //! Open file using parallel IO
  //!
  //! @param flags flags O_RDWR/O_RDONLY/O_WRONLY
  //! @param mode creation permissions
  //! @param opaque opaque information
  //!
  //! @return 0 if successful, -1 otherwise and error code is set
  //!
  //----------------------------------------------------------------------------
  virtual int OpenPio(std::vector<std::string> stripeUrls,
                      XrdSfsFileOpenMode flags,
                      mode_t mode = 0,
                      const char* opaque = "fst.pio");



  //----------------------------------------------------------------------------
  //! Read from file
  //!
  //! @param offset offset
  //! @param buffer place to hold the read data
  //! @param length length
  //! @param readahead readahead switch
  //!
  //! @return number of bytes read or -1 if error
  //!
  //--------------------------------------------------------------------------
  virtual int64_t Read(XrdSfsFileOffset offset,
                       char* buffer,
                       XrdSfsXferSize length,
                       bool readahead = false);


  //----------------------------------------------------------------------------
  //! Vector read
  //!
  //! @param chunkList list of chunks for the vector read
  //! @param len total length of the vector read
  //!
  //! @return number of bytes read of -1 if error
  //!
  //----------------------------------------------------------------------------
  virtual int64_t ReadV(XrdCl::ChunkList& chunkList,
                        uint32_t len);



  //----------------------------------------------------------------------------
  //! Write to file
  //!
  //! @param offset offset
  //! @param buffer data to be written
  //! @param length length
  //!
  //! @return number of bytes written or -1 if error
  //!
  //----------------------------------------------------------------------------
  virtual int64_t Write(XrdSfsFileOffset offset,
                        const char* buffer,
                        XrdSfsXferSize length);


  //----------------------------------------------------------------------------
  //! Truncate
  //!
  //! @param offset truncate file to this value
  //!
  //! @return 0 if successful, -1 otherwise and error code is set
  //!
  //----------------------------------------------------------------------------
  virtual int Truncate(XrdSfsFileOffset offset) = 0;


  //----------------------------------------------------------------------------
  //! Allocate file space
  //!
  //! @param length space to be allocated
  //!
  //! @return 0 if successful, -1 otherwise and error code is set
  //!
  //----------------------------------------------------------------------------
  virtual int Fallocate(XrdSfsFileOffset lenght) = 0;


  //----------------------------------------------------------------------------
  //! Deallocate file space
  //!
  //! @param fromOffset offset start
  //! @param toOffset offset end
  //!
  //! @return 0 if successful, -1 otherwise and error code is set
  //!
  //----------------------------------------------------------------------------
  virtual int Fdeallocate(XrdSfsFileOffset fromOffset,
                          XrdSfsFileOffset toOffset) = 0;


  //----------------------------------------------------------------------------
  //! Execute implementation dependant command
  //!
  //! @param cmd command
  //! @param client client identity
  //!
  //! @return 0 if successful, -1 otherwise
  //----------------------------------------------------------------------------
  virtual int Fctl(const std::string& cmd, const XrdSecEntity* client);


  //----------------------------------------------------------------------------
  //! Remove file
  //!
  //! @return 0 if successful, -1 otherwise and error code is set
  //!
  //----------------------------------------------------------------------------
  virtual int Remove();


  //----------------------------------------------------------------------------
  //! Sync file to disk
  //!
  //! @return 0 if successful, -1 otherwise and error code is set
  //!
  //----------------------------------------------------------------------------
  virtual int Sync();


  //----------------------------------------------------------------------------
  //! Close file
  //!
  //! @return 0 if successful, -1 otherwise and error code is set
  //!
  //----------------------------------------------------------------------------
  virtual int Close();


  //----------------------------------------------------------------------------
  //! Get stats about the file
  //!
  //! @param buf stat buffer
  //!
  //! @return 0 if successful, -1 otherwise and error code is set
  //!
  //----------------------------------------------------------------------------
  virtual int Stat(struct stat* buf);

  //--------------------------------------------------------------------------
  //! Get last error message
  //--------------------------------------------------------------------------
  const std::string&
  GetLastErrMsg()
  {
    return mLastErrMsg;
  }

protected:

  bool mIsRw; ///< mark for writing
  bool mIsOpen; ///< mark if open
  bool mIsPio; ///< mark if opened for parallel IO access
  bool mDoTruncate; ///< mark if there is a need to truncate
  bool mUpdateHeader; ///< mark if header updated
  bool mDoneRecovery; ///< mark if recovery done
  bool mFullDataBlocks; ///< mark if we have all data blocks to compute parity
  bool mIsStreaming; ///< file is written in streaming mode
  bool mStoreRecovery; ///< set if recovery also triggers writing back to the
  ///< files, this also means that all files must be available

  int mStripeHead; ///< head stripe value
  int mPhysicalStripeIndex; ///< physical index of the current stripe
  unsigned int mNbParityFiles; ///< number of parity files
  unsigned int mNbDataFiles; ///< number of data files
  unsigned int mNbTotalFiles; ///< total number of files ( data + parity )
  unsigned int mNbDataBlocks; ///< no. data blocks in a group
  unsigned int mNbTotalBlocks; ///< no. data and parity blocks in a group

  uint64_t mLastWriteOffset; ///< offset of the last write request
  uint64_t mStripeWidth; ///< stripe width
  uint64_t mSizeHeader; ///< size of header = 4KB
  uint64_t mFileSize; ///< total size of current file
  uint64_t mTargetSize; ///< expected final size (?!)
  uint64_t mSizeLine; ///< size of a line in a group
  int64_t mOffGroupParity; ///< offset of the last group for which we
  ///< computed the parity blocks
  uint64_t mSizeGroup; ///< size of a group of blocks
  ///< eg. RAIDDP: group = noDataStr^2 blocks

  std::string mBookingOpaque; ///< opaque information
  std::vector<char*> mDataBlocks; ///< vector containing the data in a group
  std::vector<FileIo*> mStripe; ///< file IO layout obj for each stripe
  std::vector<HeaderCRC*> mHdrInfo; ///< headers of the stripe files
  std::map<unsigned int, unsigned int> mapLP; ///< map of url to stripes
  std::map<unsigned int, unsigned int> mapPL; ///< map of stripes to url
  std::map<uint64_t, uint32_t> mMapPieces; ///< map of pieces written for which
  ///< parity computation has not been done yet
  std::string mLastErrMsg; ///< last error messages ssen

  //----------------------------------------------------------------------------
  //! Test and recover any corrupted headers in the stripe files
  //----------------------------------------------------------------------------
  virtual bool ValidateHeader();


  //----------------------------------------------------------------------------
  //! Recover corrupted chunks for the whole file
  //!
  //! @param errs list of chunks for which recovery is to be done
  //!
  //! @return true if recovery successful, false otherwise
  //!
  //----------------------------------------------------------------------------
  virtual bool RecoverPieces(XrdCl::ChunkList& errs);


  //----------------------------------------------------------------------------
  //! Compute and write parity blocks corresponding to a group of blocks
  //!
  //! @param offsetGroup offset of group of blocks
  //!
  //! @return true if successfully computed the parity and wrote it to the
  //!         corresponding files, otherwise false
  //!
  //----------------------------------------------------------------------------
  virtual bool DoBlockParity(uint64_t offGroup);


  //----------------------------------------------------------------------------
  //! Recover corrupted chunks from the current group
  //!
  //! @param grp_errs chunks to be recovered
  //!
  //! @return true if recovery successful, false otherwise
  //!
  //----------------------------------------------------------------------------
  virtual bool RecoverPiecesInGroup(XrdCl::ChunkList& grp_errs) = 0;


  //----------------------------------------------------------------------------
  //! Add new data block to the current group for parity computation, used
  //! when writing a file in streaming mode
  //!
  //! @param offset offset of the block added
  //! @param buffer data contained in the block
  //! @param length length of the data
  //!
  //----------------------------------------------------------------------------
  virtual void AddDataBlock(uint64_t offset,
                            const char* buffer,
                            uint32_t length) = 0;


  //------------------------------------------------------------------------------
  //! Compute error correction blocks
  //!
  //! @return true if parity info computed successfully, otherwise false
  //!
  //------------------------------------------------------------------------------
  virtual bool ComputeParity() = 0;


  //----------------------------------------------------------------------------
  //! Write parity information corresponding to a group to files
  //!
  //! @param offsetGroup offset of the group of blocks
  //!
  //! @return 0 if successful, otherwise error
  //!
  //----------------------------------------------------------------------------
  virtual int WriteParityToFiles(uint64_t offsetGroup) = 0;


  //----------------------------------------------------------------------------
  //! Map index from mNbDataBlocks representation to mNbTotalBlocks
  //!
  //! @param idSmall with values between 0 and 15, for exmaple in RAID-DP
  //!
  //! @return index with values between 0 and 23, -1 if error
  //!
  //----------------------------------------------------------------------------
  virtual unsigned int MapSmallToBig(unsigned int idSmall) = 0;


  //----------------------------------------------------------------------------
  //! Non-streaming operation
  //! Compute parity for the non-streaming case and write it to files
  //!
  //! @param force if true force parity computation of incomplete groups,
  //!              this means that parity will be computed even if there are
  //!              still some pieces missing - this is useful at the end of
  //!              a write operation when closing the file
  //!
  //! @return true if successful, otherwise error
  //!
  //----------------------------------------------------------------------------
  bool SparseParityComputation(bool force);



private:

  //----------------------------------------------------------------------------
  //! Non-streaming operation
  //! Add a new piece to the map of pieces written to the file
  //!
  //! @param offset offset of the new piece added
  //! @param length length of the new piece added
  //!
  //----------------------------------------------------------------------------
  void AddPiece(uint64_t offset, uint32_t length);


  //----------------------------------------------------------------------------
  //! Non-streaming operation
  //! Merge in place the pieces from the map
  //----------------------------------------------------------------------------
  void MergePieces();


  //----------------------------------------------------------------------------
  //! Non-streaming operation
  //! Get a list of the group offsets for which we can compute the parity info
  //!
  //! @param offsetGroups set of group offsets
  //! @param forceAll if true return also offsets of incomplete groups
  //!
  //----------------------------------------------------------------------------
  void GetOffsetGroups(std::set<uint64_t>& offsetGroups, bool forceAll);


  //----------------------------------------------------------------------------
  //! Non-streaming operation
  //! Read data from the current group for parity computation
  //!
  //! @param offsetGroup offset of the group about to be read
  //!
  //! @return true if operation successful, otherwise error
  //!
  //----------------------------------------------------------------------------
  bool ReadGroup(uint64_t offsetGroup);


  //----------------------------------------------------------------------------
  //! Convert a global offset (from the inital file) to a local offset within
  //! a stripe data file. The initial block does *NOT* span multiple chunks
  //! (stripes) therefore if the original length is bigger than one chunk the
  //! splitting must be done before calling this method.
  //!
  //! @param global_off initial offset
  //!
  //! @return tuple made up of the logical index of the stripe data file the
  //!         piece belongs to and the local offset within that file.
  //!
  //----------------------------------------------------------------------------
  virtual std::pair<int, uint64_t>
  GetLocalPos(uint64_t global_off) = 0;


  //----------------------------------------------------------------------------
  //! Convert a local position (from a stripe data file) to a global position
  //! within the initial file file. Note that the local offset has to come
  //! from a stripe data file since there is no corresponde in the original
  //! file for a piece which is in the parity stripe.
  //!
  //! @param stripe_id logical stripe index
  //! @param local_off local offset
  //!
  //! @return offset in the initial file of the local given piece
  //!
  //----------------------------------------------------------------------------
  virtual uint64_t
  GetGlobalOff(int stripe_id, uint64_t local_off) = 0;


  //----------------------------------------------------------------------------
  //! Split vector read request into requests for each of the data stripes with
  //! the offset and length of the new chunks adjusted to the LOCAL file stripe
  //!
  //! @param chunkList list of chunks to read from the whole file
  //! @param sizeHdr header size for local file which needs to be added to the
  //!        final local offset value
  //!
  //! @return vector of ChunkInfo structures containing the readv requests
  //!         corresponding to each of the stripe files making up the original
  //!         file.
  //!
  //----------------------------------------------------------------------------
  std::vector<XrdCl::ChunkList> SplitReadV(XrdCl::ChunkList& chunkList,
      uint32_t sizeHdr = 0);


  //----------------------------------------------------------------------------
  //! Split read request into requests spanning just one chunk so that each
  //! one is read from its corresponding stripe file. The offset values are
  //! GLOBAL i.e. they are relative to their position in the original file
  //!
  //! @param off read offset
  //! @param len read length
  //! @param buff buffer hoding the read data
  //!
  //! @return vector of ChunkInfo structures containing the read requests
  //!         corresponding to each of the chunks making up the original file
  //!
  //----------------------------------------------------------------------------
  XrdCl::ChunkList SplitRead(uint64_t off, uint32_t len, char* buff);


  //----------------------------------------------------------------------------
  //! Disable copy constructor
  //----------------------------------------------------------------------------
  RaidMetaLayout(const RaidMetaLayout&) = delete;


  //----------------------------------------------------------------------------
  //! Disable assign operator
  //----------------------------------------------------------------------------
  RaidMetaLayout& operator = (const RaidMetaLayout&) = delete;

};

EOSFSTNAMESPACE_END

#endif  // __EOSFST_RAIDMETALAYOUT_HH__
