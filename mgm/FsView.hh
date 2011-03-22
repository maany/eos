#ifndef __EOSMGM_FSVIEW__HH__
#define __EOSMGM_FSVIEW__HH__

/*----------------------------------------------------------------------------*/
#include "mgm/Namespace.hh"
#include "common/FileSystem.hh"
#include "common/RWMutex.hh"
#include "common/Logging.hh"
#include "common/GlobalConfig.hh"
/*----------------------------------------------------------------------------*/
#include "XrdOuc/XrdOucString.hh"
/*----------------------------------------------------------------------------*/
#include <sys/vfs.h>
#include <map>
#include <set>

/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN

//------------------------------------------------------------------------
//! Classes providing views on filesystems by space,group,node
//------------------------------------------------------------------------

class BaseView : public std::set<eos::common::FileSystem::fsid_t> {
private:
  time_t      mHeartBeat;
  std::string mHeartBeatString;
  std::string mHeartBeatDeltaString;
  std::string mStatus;
  std::string mSize;
public:
  std::string mName;
  std::string mType;
  
  BaseView(){};
  ~BaseView(){};
  
  virtual const char* GetConfigQueuePrefix() { return "";}

  void Print(std::string &out, std::string headerformat, std::string listformat);
  
  virtual std::string GetMember(std::string member);


  void SetHeartBeat(time_t hb)       { mHeartBeat = hb;       }
  void SetStatus(const char* status) { mStatus = status;      }
  const char* GetStatus()            { return mStatus.c_str();}
  time_t      GetHeartBeat()         { return mHeartBeat;     }


  long long SumLongLong(const char* param); // calculates the sum of <param> as long long
  double SumDouble(const char* param);      // calculates the sum of <param> as double
  double AverageDouble(const char* param);  // calculates the average of <param> as double
  double SigmaDouble(const char* param);    // calculates the standard deviation of <param> as double
};

class FsSpace : public BaseView {
public:

  FsSpace(const char* name) {mName = name; mType = "spaceview";}
  ~FsSpace() {};

  static std::string gConfigQueuePrefix;
  virtual const char* GetConfigQueuePrefix() { return gConfigQueuePrefix.c_str();}
  static const char* sGetConfigQueuePrefix() { return gConfigQueuePrefix.c_str();}
};

//------------------------------------------------------------------------
class FsGroup : public BaseView {
public:

  FsGroup(const char* name) {mName = name; mType="groupview";}
  ~FsGroup(){};

  static std::string gConfigQueuePrefix;
  virtual const char* GetConfigQueuePrefix() { return gConfigQueuePrefix.c_str();}
  static const char* sGetConfigQueuePrefix() { return gConfigQueuePrefix.c_str();}
};

//------------------------------------------------------------------------
class FsNode : public BaseView {
public:

  FsNode(const char* name) {mName = name; mType="nodesview";}
  ~FsNode(){};

  static std::string gConfigQueuePrefix;
  virtual const char* GetConfigQueuePrefix() { return gConfigQueuePrefix.c_str();}
  static const char* sGetConfigQueuePrefix() { return gConfigQueuePrefix.c_str();}
};

//------------------------------------------------------------------------
class FsView : public eos::common::LogId {
private:
  
  eos::common::FileSystem::fsid_t NextFsId;
  std::map<eos::common::FileSystem::fsid_t , std::string> Fs2UuidMap;
  std::map<std::string, eos::common::FileSystem::fsid_t>  Uuid2FsMap;

public:

  bool Register   (eos::common::FileSystem* fs); // this adds or modifies a filesystem
  bool UnRegister (eos::common::FileSystem* fs); // this removes a filesystem

  bool RegisterNode   (const char* nodequeue);            // this adds or modifies an fst node
  bool UnRegisterNode (const char* nodequeue);            // this removes an fst node

  bool RegisterSpace  (const char* spacename);            // this adds or modifies a space 
  bool UnRegisterSpace(const char* spacename);            // this remove a space

  bool RegisterGroup   (const char* groupname);            // this adds or modifies a group
  bool UnRegisterGroup (const char* groupname);            // this removes a group

  eos::common::RWMutex ViewMutex;  // protecting all xxxView variables
  eos::common::RWMutex MapMutex;   // protecting all xxxMap varables

  std::map<std::string , FsSpace* > mSpaceView;
  std::map<std::string , FsGroup* > mGroupView;
  std::map<std::string , FsNode* >  mNodeView;

  std::map<eos::common::FileSystem::fsid_t, eos::common::FileSystem*> mIdView;
  std::map<eos::common::FileSystem*, eos::common::FileSystem::fsid_t> mFileSystemView;

  // filesystem mapping functions
  eos::common::FileSystem::fsid_t CreateMapping(std::string fsuuid);
  bool                            ProvideMapping(std::string fsuuid, eos::common::FileSystem::fsid_t fsid);
  eos::common::FileSystem::fsid_t GetMapping(std::string fsuuid);
  std::string GetMapping(eos::common::FileSystem::fsid_t fsuuid);
  bool        RemoveMapping(eos::common::FileSystem::fsid_t fsid, std::string fsuuid);

  void PrintSpaces(std::string &out, std::string headerformat, std::string listformat);
  void PrintGroups(std::string &out, std::string headerformat, std::string listformat);
  void PrintNodes (std::string &out, std::string headerformat, std::string listformat);
  
  static std::string GetNodeFormat       (std::string option);
  static std::string GetGroupFormat      (std::string option);
  static std::string GetSpaceFormat      (std::string option);
  static std::string GetFileSystemFormat (std::string option);


  FsView() {};
  ~FsView() {};

  void SetConfigQueues(const char* nodeconfigqueue, const char* groupconfigqueue, const char* spaceconfigqueue) {
    FsSpace::gConfigQueuePrefix = spaceconfigqueue;
    FsGroup::gConfigQueuePrefix = groupconfigqueue;
    FsNode::gConfigQueuePrefix  = nodeconfigqueue;
  }
  static FsView gFsView; // singleton
};

EOSMGMNAMESPACE_END

#endif
