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

//------------------------------------------------------------------------------
// author: Lukasz Janyst <ljanyst@cern.ch>
// desc:   User quota accounting
//------------------------------------------------------------------------------

#include "namespace/interface/IContainerMD.hh"
#include "namespace/interface/IFileMD.hh"
#include "namespace/ns_on_redis/accounting/QuotaStats.hh"

EOSNSNAMESPACE_BEGIN

const std::string QuotaStats::sQuotaPrefix = "quota_node:";
const std::string QuotaStats::sSetQuotaIds = "quota_set_ids";

const std::string QuotaNode::sSpaceTag = ":space";
const std::string QuotaNode::sPhysicalSpaceTag = ":physical_space";
const std::string QuotaNode::sFilesTag = ":files";

//------------------------------------------------------------------------------
// *** Class QuotaNode implementaion ***
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
QuotaNode::QuotaNode(IQuotaStats* quotaStats, IContainerMD::id_t node_id):
  IQuotaNode(quotaStats)
{
  pQuotaUidKey = QuotaStats::sQuotaPrefix + std::to_string(node_id) + ":uid";
  pQuotaGidKey = QuotaStats::sQuotaPrefix + std::to_string(node_id) + ":gid";
}

//------------------------------------------------------------------------------
// Account a new file, adjust the size using the size mapping function
//------------------------------------------------------------------------------
void QuotaNode::addFile(const IFileMD* file)
{
  const std::string suid = std::to_string(file->getCUid());
  const std::string sgid = std::to_string(file->getCGid());
  const int64_t size = pQuotaStats->getPhysicalSize(file);
  std::string field = suid + sPhysicalSpaceTag;
  redox::Redox* redox = RedisClient::getInstance();
  (void) redox->hincrby(pQuotaUidKey, field, size);
  field = sgid + sPhysicalSpaceTag;
  (void) redox->hincrby(pQuotaGidKey, field, size);
  field = suid + sSpaceTag;
  (void) redox->hincrby(pQuotaUidKey, field, file->getSize());
  field = sgid + sSpaceTag;
  (void) redox->hincrby(pQuotaGidKey, field, file->getSize());
  field = suid + sFilesTag;
  (void) redox->hincrby(pQuotaUidKey, field, 1);
  field = sgid + sFilesTag;
  (void) redox->hincrby(pQuotaGidKey, field, 1);
}

//------------------------------------------------------------------------------
// Remove a file, adjust the size using the size mapping function
//------------------------------------------------------------------------------
void QuotaNode::removeFile(const IFileMD* file)
{
  const std::string suid = std::to_string(file->getCUid());
  const std::string sgid = std::to_string(file->getCGid());
  int64_t size = pQuotaStats->getPhysicalSize(file);
  redox::Redox* redox = RedisClient::getInstance();
  std::string field = suid + sPhysicalSpaceTag;
  (void) redox->hincrby(pQuotaUidKey, field, -size);
  field = sgid + sPhysicalSpaceTag;
  (void) redox->hincrby(pQuotaGidKey, field, -size);
  field = suid + sSpaceTag;
  size = static_cast<int64_t>(file->getSize());
  (void) redox->hincrby(pQuotaUidKey, field, -size);
  field = sgid + sSpaceTag;
  (void) redox->hincrby(pQuotaGidKey, field, -size);
  field = suid + sFilesTag;
  (void) redox->hincrby(pQuotaUidKey, field, -1);
  field = sgid + sFilesTag;
  (void) redox->hincrby(pQuotaGidKey, field, -1);
}

//------------------------------------------------------------------------------
// Meld in another quota node
//------------------------------------------------------------------------------
void QuotaNode::meld(const IQuotaNode* node)
{
  std::string field;
  redox::Redox* redox = RedisClient::getInstance();
  std::vector<std::string> elems = redox->hgetall(node->getUidKey());

  for (auto it = elems.begin(); it != elems.end(); ++it)
  {
    field = *it;
    ++it;
    (void) redox->hincrby(pQuotaUidKey, field, *it);
  }

  elems = redox->hgetall(node->getGidKey());

  for (auto it = elems.begin(); it != elems.end(); ++it)
  {
    field = *it;
    ++it;
    (void) redox->hincrby(pQuotaGidKey, field, *it);
  }
}

//------------------------------------------------------------------------------
// Get the amount of space occupied by the given user
//------------------------------------------------------------------------------
uint64_t
QuotaNode::getUsedSpaceByUser(uid_t uid)
{
  std::string field = std::to_string(uid) + sSpaceTag;
  std::string val = "0";

  try
  {
    val = RedisClient::getInstance()->hget(pQuotaUidKey, field);
  }
  catch (std::runtime_error& e) {};
  return std::stoull(val);
}

//------------------------------------------------------------------------------
// Get the amount of space occupied by the given group
//------------------------------------------------------------------------------
uint64_t
QuotaNode::getUsedSpaceByGroup(gid_t gid)
{
  std::string field = std::to_string(gid) + sSpaceTag;
  std::string val = "0";

  try
  {
    val = RedisClient::getInstance()->hget(pQuotaGidKey, field);
  }
  catch (std::runtime_error& e) {};
  return std::stoull(val);
}

//------------------------------------------------------------------------------
// Get the amount of space occupied by the given user
//------------------------------------------------------------------------------
uint64_t
QuotaNode::getPhysicalSpaceByUser(uid_t uid)
{
  std::string field = std::to_string(uid) + sPhysicalSpaceTag;
  std::string val = "0";

  try
  {
    val = RedisClient::getInstance()->hget(pQuotaUidKey, field);
  }
  catch (std::runtime_error& e) {};
  return std::stoull(val);
}

//------------------------------------------------------------------------------
// Get the amount of space occupied by the given group
//------------------------------------------------------------------------------
uint64_t
QuotaNode::getPhysicalSpaceByGroup(gid_t gid)
{
  std::string field = std::to_string(gid) + sPhysicalSpaceTag;
  std::string val = "0";

  try
  {
    val = RedisClient::getInstance()->hget(pQuotaGidKey, field);
  }
  catch (std::runtime_error& e) {};
  return std::stoull(val);
}

//------------------------------------------------------------------------------
// Get the amount of space occupied by the given user
//------------------------------------------------------------------------------
uint64_t
QuotaNode::getNumFilesByUser(uid_t uid)
{
  std::string field = std::to_string(uid) + sFilesTag;
  std::string val = "0";

  try
  {
    val = RedisClient::getInstance()->hget(pQuotaUidKey, field);
  }
  catch (std::runtime_error& e) {};
  return std::stoull(val);
}

//----------------------------------------------------------------------------
// Get the amount of space occupied by the given group
//----------------------------------------------------------------------------
uint64_t
QuotaNode::getNumFilesByGroup(gid_t gid)
{
  std::string field = std::to_string(gid) + sFilesTag;
  std::string val = "0";

  try
  {
    val = RedisClient::getInstance()->hget(pQuotaGidKey, field);
  }
  catch (std::runtime_error& e) {};
  return std::stoull(val);
}

//------------------------------------------------------------------------------
// Get the set of uids for which information is stored in the current quota
// node.
//------------------------------------------------------------------------------
std::vector<unsigned long>
QuotaNode::getUids()
{
  std::string suid;
  std::vector<std::string> keys = RedisClient::getInstance()->hkeys(pQuotaUidKey);
  std::vector<unsigned long> uids;
  uids.resize(keys.size() / 3);

  // The keys have to following format: uid1:space, uid1:physical_space,
  // uid1:files ... uidn:files.
  for (auto elem: keys)
  {
    suid = elem.substr(0, elem.find(':'));
    uids.push_back(std::stoul(suid));
  }

  return uids;
}

//----------------------------------------------------------------------------
// Get the set of gids for which information is stored in the current quota
// node.
//----------------------------------------------------------------------------
std::vector<unsigned long>
QuotaNode::getGids()
{
  std::string sgid;
  std::vector<std::string> keys = RedisClient::getInstance()->hkeys(pQuotaGidKey);
  std::vector<unsigned long> gids;
  gids.resize(keys.size() / 3);

  // The keys have to following format: gid1:space, gid1:physical_space,
  // gid1:files ... gidn:files.
  for (auto&& elem: keys)
  {
    sgid = elem.substr(0, elem.find(':'));
    gids.push_back(std::stoul(elem));
  }

  return gids;
}


//------------------------------------------------------------------------------
// *** Class QuotaStats implementaion ***
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
QuotaStats::QuotaStats()
{
  pRedox = RedisClient::getInstance();
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
QuotaStats::~QuotaStats()
{
  pRedox = nullptr;

  for (auto it = pNodeMap.begin(); it != pNodeMap.end(); ++it)
    delete it->second;

  pNodeMap.clear();
}

//------------------------------------------------------------------------------
// Get a quota node associated to the container id
//------------------------------------------------------------------------------
IQuotaNode* QuotaStats::getQuotaNode(IContainerMD::id_t node_id)
{
  if (pNodeMap.count(node_id))
    return pNodeMap[node_id];

  if (!pRedox->sismember(sSetQuotaIds, std::to_string(node_id)))
    return 0;

  IQuotaNode* ptr {new QuotaNode(this, node_id)};
  pNodeMap[node_id] = ptr;
  return ptr;
}

//------------------------------------------------------------------------------
// Register a new quota node
//------------------------------------------------------------------------------
IQuotaNode* QuotaStats::registerNewNode(IContainerMD::id_t node_id)
{
  std::string snode_id = std::to_string(node_id);

  if (pRedox->sismember(sSetQuotaIds, snode_id))
  {
    MDException e;
    e.getMessage() << "Quota node already exist: " << node_id;
    throw e;
  }

  if (!pRedox->sadd(sSetQuotaIds, snode_id))
  {
    MDException e;
    e.getMessage() << "Failed to register new quota node: " << node_id;
    throw e;
  }

  IQuotaNode* ptr {new QuotaNode(this, node_id)};
  pNodeMap[node_id] = ptr;
  return ptr;
}

//------------------------------------------------------------------------------
// Remove quota node
//------------------------------------------------------------------------------
void QuotaStats::removeNode(IContainerMD::id_t node_id)
{
  std::string snode_id = std::to_string(node_id);

  if (pNodeMap.count(node_id))
    pNodeMap.erase(node_id);

  if (!pRedox->srem(sSetQuotaIds, snode_id))
  {
    MDException e;
    e.getMessage() << "Quota node " << node_id << "does not exist in set";
    throw e;
  }

  std::string sqnode = sQuotaPrefix + snode_id;

  if (!pRedox->del(sqnode))
  {
    MDException e;
    e.getMessage() << "Quota node " << node_id << "does not exist";
    throw e;
  }
}

//------------------------------------------------------------------------------
// Get the set of all quota node ids. The quota node id corresponds to the
// container id.
//------------------------------------------------------------------------------
std::set<std::string>
QuotaStats::getAllIds()
{
  return pRedox->smembers(sSetQuotaIds);
}

EOSNSNAMESPACE_END
