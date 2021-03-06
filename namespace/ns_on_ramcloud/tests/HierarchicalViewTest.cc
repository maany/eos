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
//! @author Elvin-Alin Sindrilaru <esindril@cern.ch>
//! @breif HierarchicalView tests
//------------------------------------------------------------------------------
#include <stdint.h>
#include <unistd.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <memory>
#include <pthread.h>
#include <cppunit/extensions/HelperMacros.h>
#include "namespace/utils/TestHelpers.hh"
#include "namespace/interface/IContainerMD.hh"
#include "namespace/ns_quarkdb/views/HierarchicalView.hh"
#include "namespace/ns_quarkdb/accounting/QuotaStats.hh"
#include "namespace/ns_quarkdb/persistency/ContainerMDSvc.hh"
#include "namespace/ns_quarkdb/persistency/FileMDSvc.hh"

//------------------------------------------------------------------------------
// HierarchicalViewTest class
//------------------------------------------------------------------------------
class HierarchicalViewTest: public CppUnit::TestCase
{
public:
  CPPUNIT_TEST_SUITE(HierarchicalViewTest);
  CPPUNIT_TEST(loadTest);
  CPPUNIT_TEST(quotaTest);
  CPPUNIT_TEST(lostContainerTest);
  CPPUNIT_TEST_SUITE_END();

  void loadTest();
  void quotaTest();
  void lostContainerTest();
};

CPPUNIT_TEST_SUITE_REGISTRATION(HierarchicalViewTest);

//------------------------------------------------------------------------------
// Load test
//------------------------------------------------------------------------------
void HierarchicalViewTest::loadTest()
{
  try {
    std::map<std::string, std::string> config = {
      {"redis_host", "localhost"},
      {"redis_port", "6380"}
    };
    std::unique_ptr<eos::IContainerMDSvc> contSvc {new eos::ContainerMDSvc()};
    std::unique_ptr<eos::IFileMDSvc> fileSvc {new eos::FileMDSvc()};
    std::unique_ptr<eos::IView> view {new eos::HierarchicalView()};
    fileSvc->setContMDService(contSvc.get());
    contSvc->setFileMDService(fileSvc.get());
    fileSvc->configure(config);
    contSvc->configure(config);
    view->setContainerMDSvc(contSvc.get());
    view->setFileMDSvc(fileSvc.get());
    view->configure(config);
    view->initialize();
    std::shared_ptr<eos::IContainerMD> cont1 {view->createContainer("/test/embed/embed1", true)};
    std::shared_ptr<eos::IContainerMD> cont2 {view->createContainer("/test/embed/embed2", true)};
    std::shared_ptr<eos::IContainerMD> cont3 {view->createContainer("/test/embed/embed3", true)};
    std::shared_ptr<eos::IContainerMD> cont4 {view->createContainer("/test/embed/embed4", true)};
    std::shared_ptr<eos::IContainerMD> root {view->getContainer("/")};
    std::shared_ptr<eos::IContainerMD> test {view->getContainer("/test")};
    std::shared_ptr<eos::IContainerMD> embed {view->getContainer("/test/embed")};
    CPPUNIT_ASSERT(root != 0);
    CPPUNIT_ASSERT(root->getId() == root->getParentId());
    CPPUNIT_ASSERT(test != 0);
    CPPUNIT_ASSERT(test->findContainer("embed") != 0);
    CPPUNIT_ASSERT(embed != 0);
    CPPUNIT_ASSERT(embed->findContainer("embed1") != 0);
    CPPUNIT_ASSERT(embed->findContainer("embed2") != 0);
    CPPUNIT_ASSERT(embed->findContainer("embed3") != 0);
    CPPUNIT_ASSERT(cont1->getName() == embed->findContainer("embed1")->getName());
    CPPUNIT_ASSERT(cont2->getName() == embed->findContainer("embed2")->getName());
    CPPUNIT_ASSERT(cont3->getName() == embed->findContainer("embed3")->getName());
    view->removeContainer("/test/embed/embed2");
    CPPUNIT_ASSERT(embed->findContainer("embed2") == 0);
    view->createFile("/test/embed/file1");
    view->createFile("/test/embed/file2");
    view->createFile("/test/embed/embed1/file1");
    view->createFile("/test/embed/embed1/file2");
    view->createFile("/test/embed/embed1/file3");
    std::shared_ptr<eos::IFileMD> fileR {view->createFile("/test/embed/embed1/fileR")};
    CPPUNIT_ASSERT(view->getFile("/test/embed/file1"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/file2"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/embed1/file1"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/embed1/file2"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/embed1/file3"));
    // Rename
    CPPUNIT_ASSERT_NO_THROW(view->renameContainer(cont4.get(), "embed4.renamed"));
    CPPUNIT_ASSERT(cont4->getName() == "embed4.renamed");
    CPPUNIT_ASSERT_THROW(view->renameContainer(cont4.get(), "embed1"),
                         eos::MDException);
    CPPUNIT_ASSERT_THROW(view->renameContainer(cont4.get(), "embed1/asd"),
                         eos::MDException);
    CPPUNIT_ASSERT_NO_THROW(view->getContainer("/test/embed/embed4.renamed"));
    CPPUNIT_ASSERT_NO_THROW(view->renameFile(fileR.get(), "fileR.renamed"));
    CPPUNIT_ASSERT(fileR->getName() == "fileR.renamed");
    CPPUNIT_ASSERT_THROW(view->renameFile(fileR.get(), "file1"), eos::MDException);
    CPPUNIT_ASSERT_THROW(view->renameFile(fileR.get(), "file1/asd"),
                         eos::MDException);
    CPPUNIT_ASSERT_NO_THROW(view->getFile("/test/embed/embed1/fileR.renamed"));
    CPPUNIT_ASSERT_THROW(view->renameContainer(root.get(), "rename"),
                         eos::MDException);
    // Test the "reverse" lookup
    std::shared_ptr<eos::IFileMD> file {view->getFile("/test/embed/embed1/file3")};
    std::shared_ptr<eos::IContainerMD> container {view->getContainer("/test/embed/embed1")};
    CPPUNIT_ASSERT(view->getUri(container.get()) == "/test/embed/embed1/");
    CPPUNIT_ASSERT(view->getUri(file.get()) == "/test/embed/embed1/file3");
    CPPUNIT_ASSERT_THROW(view->getUri((eos::IFileMD*)0), eos::MDException);
    std::shared_ptr<eos::IFileMD> toBeDeleted {view->getFile("/test/embed/embed1/file2")};
    toBeDeleted->addLocation(12);
    // This should not succeed since the file should have a replica
    CPPUNIT_ASSERT_THROW(view->removeFile(toBeDeleted.get()), eos::MDException);
    // We unlink the file - at this point the file should not be attached to the
    // hierarchy but should still be accessible by id and thus the md pointer
    // should stay valid
    view->unlinkFile("/test/embed/embed1/file2");
    CPPUNIT_ASSERT_THROW(view->getFile("/test/embed/embed1/file2"),
                         eos::MDException);
    CPPUNIT_ASSERT(cont1->findFile("file2") == 0);
    // We remove the replicas and the file but we need to reload the toBeDeleted pointer
    eos::IFileMD::id_t id = toBeDeleted->getId();
    toBeDeleted = view->getFileMDSvc()->getFileMD(id);
    toBeDeleted->clearUnlinkedLocations();
    CPPUNIT_ASSERT_NO_THROW(view->removeFile(toBeDeleted.get()));
    CPPUNIT_ASSERT_THROW(view->getFileMDSvc()->getFileMD(id), eos::MDException);
    view->finalize();
    view->initialize();
    CPPUNIT_ASSERT(view->getContainer("/"));
    CPPUNIT_ASSERT(view->getContainer("/test"));
    CPPUNIT_ASSERT(view->getContainer("/test/embed"));
    CPPUNIT_ASSERT(view->getContainer("/test/embed/embed1"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/file1"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/file2"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/embed1/file1"));
    CPPUNIT_ASSERT(view->getFile("/test/embed/embed1/file3"));
    CPPUNIT_ASSERT_NO_THROW(view->getContainer("/test/embed/embed4.renamed"));
    CPPUNIT_ASSERT_NO_THROW(view->getFile("/test/embed/embed1/fileR.renamed"));
    // Cleanup
    // Unlink files - need to do it in this order since the unlink removes the
    // file from the container and then getFile by path won't work anymore
    std::shared_ptr<eos::IFileMD> file1 {view->getFile("/test/embed/file1")};
    std::shared_ptr<eos::IFileMD> file2 {view->getFile("/test/embed/file2")};
    std::shared_ptr<eos::IFileMD> file11 {view->getFile("/test/embed/embed1/file1")};
    std::shared_ptr<eos::IFileMD> file13 {view->getFile("/test/embed/embed1/file3")};
    CPPUNIT_ASSERT_NO_THROW(view->unlinkFile("/test/embed/file1"));
    CPPUNIT_ASSERT_NO_THROW(view->unlinkFile("/test/embed/file2"));
    CPPUNIT_ASSERT_NO_THROW(view->unlinkFile("/test/embed/embed1/file1"));
    CPPUNIT_ASSERT_NO_THROW(view->unlinkFile("/test/embed/embed1/file3"));
    CPPUNIT_ASSERT_NO_THROW(view->unlinkFile("/test/embed/embed1/fileR.renamed"));
    // Remove files
    CPPUNIT_ASSERT_NO_THROW(view->removeFile(view->getFileMDSvc()->getFileMD(
                              file1->getId()).get()));
    CPPUNIT_ASSERT_NO_THROW(view->removeFile(view->getFileMDSvc()->getFileMD(
                              file2->getId()).get()));
    CPPUNIT_ASSERT_NO_THROW(view->removeFile(view->getFileMDSvc()->getFileMD(
                              file11->getId()).get()));
    CPPUNIT_ASSERT_NO_THROW(view->removeFile(view->getFileMDSvc()->getFileMD(
                              file13->getId()).get()));
    CPPUNIT_ASSERT_NO_THROW(view->removeFile(view->getFileMDSvc()->getFileMD(
                              fileR->getId()).get()));
    // Remove all containers
    CPPUNIT_ASSERT_NO_THROW(view->removeContainer("/test/", true));
    // Remove the root container
    CPPUNIT_ASSERT_NO_THROW(contSvc->removeContainer(root.get()));
    view->finalize();
  } catch (eos::MDException& e) {
    CPPUNIT_ASSERT_MESSAGE(e.getMessage().str(), false);
  }
}

//------------------------------------------------------------------------------
// File size mapping function
//------------------------------------------------------------------------------
static uint64_t mapSize(const eos::IFileMD* file)
{
  eos::IFileMD::layoutId_t lid = file->getLayoutId();

  if (lid > 3) {
    eos::MDException e(ENOENT);
    e.getMessage() << "Location does not exist" << std::endl;
    throw (e);
  }

  return lid * file->getSize();
}

//------------------------------------------------------------------------------
// Create files at given path
//------------------------------------------------------------------------------
static void createFiles(const std::string&                          path,
                        eos::IView*                                 view,
                        std::map<uid_t, eos::QuotaNode::UsageInfo>& users,
                        std::map<gid_t, eos::QuotaNode::UsageInfo>& groups)
{
  eos::IQuotaNode* node = view->getQuotaNode(view->getContainer(path).get());

  for (int i = 0; i < 1000; ++i) {
    std::ostringstream p;
    p << path << "file" << i;
    std::shared_ptr<eos::IFileMD> file {view->createFile(p.str())};
    file->setCUid(random() % 10 + 1);
    file->setCGid(random() % 3 + 1);
    file->setSize(random() % 1000000 + 1);
    file->setLayoutId(random() % 3 + 1);
    view->updateFileStore(file.get());
    node->addFile(file.get());
    uint64_t size = mapSize(file.get());
    eos::IQuotaNode::UsageInfo& user  = users[file->getCUid()];
    eos::IQuotaNode::UsageInfo& group = groups[file->getCGid()];
    user.space += file->getSize();
    user.physicalSpace += size;
    user.files++;
    group.space += file->getSize();
    group.physicalSpace += size;
    group.files++;
  }
}

//------------------------------------------------------------------------------
// Quota test
//------------------------------------------------------------------------------
void HierarchicalViewTest::quotaTest()
{
  srandom(time(0));
  // Initialize the system
  std::map<std::string, std::string> config = {
    {"redis_host", "localhost"},
    {"redis_port", "6380"}
  };
  std::unique_ptr<eos::ContainerMDSvc> contSvc {new eos::ContainerMDSvc()};
  std::unique_ptr<eos::FileMDSvc> fileSvc {new eos::FileMDSvc()};
  std::unique_ptr<eos::IView> view {new eos::HierarchicalView()};
  fileSvc->setContMDService(contSvc.get());
  fileSvc->configure(config);
  contSvc->setFileMDService(fileSvc.get());
  contSvc->configure(config);
  view->setContainerMDSvc(contSvc.get());
  view->setFileMDSvc(fileSvc.get());
  view->configure(config);
  view->getQuotaStats()->registerSizeMapper(mapSize);
  CPPUNIT_ASSERT_NO_THROW(view->initialize());
  // Create some structures, insert quota nodes and test their correctness
  std::shared_ptr<eos::IContainerMD> cont1 {view->createContainer("/test/embed/embed1", true)};
  std::shared_ptr<eos::IContainerMD> cont2 {view->createContainer("/test/embed/embed2", true)};
  std::shared_ptr<eos::IContainerMD> cont3 {view->createContainer("/test/embed/embed3", true)};
  std::shared_ptr<eos::IContainerMD> cont4 {view->getContainer("/test/embed")};
  std::shared_ptr<eos::IContainerMD> cont5 {view->getContainer("/test")};
  eos::IQuotaNode* qnCreated1 = view->registerQuotaNode(cont1.get());
  eos::IQuotaNode* qnCreated2 = view->registerQuotaNode(cont3.get());
  eos::IQuotaNode* qnCreated3 = view->registerQuotaNode(cont5.get());
  CPPUNIT_ASSERT_THROW(view->registerQuotaNode(cont1.get()), eos::MDException);
  CPPUNIT_ASSERT(qnCreated1);
  CPPUNIT_ASSERT(qnCreated2);
  CPPUNIT_ASSERT(qnCreated3);
  eos::IQuotaNode* qn1 = view->getQuotaNode(cont1.get());
  eos::IQuotaNode* qn2 = view->getQuotaNode(cont2.get());
  eos::IQuotaNode* qn3 = view->getQuotaNode(cont3.get());
  eos::IQuotaNode* qn4 = view->getQuotaNode(cont4.get());
  eos::IQuotaNode* qn5 = view->getQuotaNode(cont5.get());
  CPPUNIT_ASSERT(qn1);
  CPPUNIT_ASSERT(qn2);
  CPPUNIT_ASSERT(qn3);
  CPPUNIT_ASSERT(qn4);
  CPPUNIT_ASSERT(qn5);
  CPPUNIT_ASSERT(qn2 == qn5);
  CPPUNIT_ASSERT(qn4 == qn5);
  CPPUNIT_ASSERT(qn1 != qn5);
  CPPUNIT_ASSERT(qn3 != qn5);
  CPPUNIT_ASSERT(qn3 != qn2);
  // Create some files
  std::map<uid_t, eos::IQuotaNode::UsageInfo> users1;
  std::map<gid_t, eos::IQuotaNode::UsageInfo> groups1;
  std::string path1 = "/test/embed/embed1/";
  createFiles(path1, view.get(), users1, groups1);
  std::map<uid_t, eos::IQuotaNode::UsageInfo> users2;
  std::map<gid_t, eos::IQuotaNode::UsageInfo> groups2;
  std::string path2 = "/test/embed/embed2/";
  createFiles(path2, view.get(), users2, groups2);
  std::map<uid_t, eos::IQuotaNode::UsageInfo> users3;
  std::map<gid_t, eos::IQuotaNode::UsageInfo> groups3;
  std::string path3 = "/test/embed/embed3/";
  createFiles(path3, view.get(), users3, groups3);
  // Verify correctness
  eos::IQuotaNode* node1 = view->getQuotaNode(view->getContainer(path1).get());
  eos::IQuotaNode* node2 = view->getQuotaNode(view->getContainer(path2).get());

  for (int i = 1; i <= 10; ++i) {
    CPPUNIT_ASSERT(node1->getPhysicalSpaceByUser(i) == users1[i].physicalSpace);
    CPPUNIT_ASSERT(node2->getPhysicalSpaceByUser(i) == users2[i].physicalSpace);
    CPPUNIT_ASSERT(node1->getUsedSpaceByUser(i) == users1[i].space);
    CPPUNIT_ASSERT(node2->getUsedSpaceByUser(i) == users2[i].space);
    CPPUNIT_ASSERT(node1->getNumFilesByUser(i)  == users1[i].files);
    CPPUNIT_ASSERT(node2->getNumFilesByUser(i)  == users2[i].files);
  }

  for (int i = 1; i <= 3; ++i) {
    CPPUNIT_ASSERT(node1->getPhysicalSpaceByGroup(i) == groups1[i].physicalSpace);
    CPPUNIT_ASSERT(node2->getPhysicalSpaceByGroup(i) == groups2[i].physicalSpace);
    CPPUNIT_ASSERT(node1->getUsedSpaceByGroup(i) == groups1[i].space);
    CPPUNIT_ASSERT(node2->getUsedSpaceByGroup(i) == groups2[i].space);
    CPPUNIT_ASSERT(node1->getNumFilesByGroup(i)  == groups1[i].files);
    CPPUNIT_ASSERT(node2->getNumFilesByGroup(i)  == groups2[i].files);
  }

  // Restart and check if the quota stats are reloaded correctly
  CPPUNIT_ASSERT_NO_THROW(view->finalize());
  view->setQuotaStats(new eos::QuotaStats(config));
  view->getQuotaStats()->registerSizeMapper(mapSize);
  CPPUNIT_ASSERT_NO_THROW(view->initialize());
  node1 = view->getQuotaNode(view->getContainer(path1).get());
  node2 = view->getQuotaNode(view->getContainer(path2).get());
  CPPUNIT_ASSERT(node1);
  CPPUNIT_ASSERT(node2);

  for (int i = 1; i <= 10; ++i) {
    CPPUNIT_ASSERT(node1->getPhysicalSpaceByUser(i) == users1[i].physicalSpace);
    CPPUNIT_ASSERT(node2->getPhysicalSpaceByUser(i) == users2[i].physicalSpace);
    CPPUNIT_ASSERT(node1->getUsedSpaceByUser(i) == users1[i].space);
    CPPUNIT_ASSERT(node2->getUsedSpaceByUser(i) == users2[i].space);
    CPPUNIT_ASSERT(node1->getNumFilesByUser(i)  == users1[i].files);
    CPPUNIT_ASSERT(node2->getNumFilesByUser(i)  == users2[i].files);
  }

  for (int i = 1; i <= 3; ++i) {
    CPPUNIT_ASSERT(node1->getPhysicalSpaceByGroup(i) == groups1[i].physicalSpace);
    CPPUNIT_ASSERT(node2->getPhysicalSpaceByGroup(i) == groups2[i].physicalSpace);
    CPPUNIT_ASSERT(node1->getUsedSpaceByGroup(i) == groups1[i].space);
    CPPUNIT_ASSERT(node2->getUsedSpaceByGroup(i) == groups2[i].space);
    CPPUNIT_ASSERT(node1->getNumFilesByGroup(i)  == groups1[i].files);
    CPPUNIT_ASSERT(node2->getNumFilesByGroup(i)  == groups2[i].files);
  }

  // Remove the quota nodes on /test/embed/embed1 and /dest/embed/embed2
  // and check if the quota on /test has been updated
  eos::IQuotaNode* parentNode = 0;
  CPPUNIT_ASSERT_NO_THROW(parentNode = view->getQuotaNode(
                                         view->getContainer("/test").get()));
  CPPUNIT_ASSERT_NO_THROW(view->removeQuotaNode(view->getContainer(path1).get()));

  for (int i = 1; i <= 10; ++i) {
    CPPUNIT_ASSERT(parentNode->getPhysicalSpaceByUser(i) ==
                   users1[i].physicalSpace + users2[i].physicalSpace);
    CPPUNIT_ASSERT(parentNode->getUsedSpaceByUser(i) ==
                   users1[i].space + users2[i].space);
    CPPUNIT_ASSERT(parentNode->getNumFilesByUser(i) ==
                   users1[i].files + users2[i].files);
  }

  for (int i = 1; i <= 3; ++i) {
    CPPUNIT_ASSERT(parentNode->getPhysicalSpaceByGroup(i) ==
                   groups1[i].physicalSpace + groups2[i].physicalSpace);
    CPPUNIT_ASSERT(parentNode->getUsedSpaceByGroup(i) ==
                   groups1[i].space + groups2[i].space);
    CPPUNIT_ASSERT(parentNode->getNumFilesByGroup(i) ==
                   groups1[i].files + groups2[i].files);
  }

  CPPUNIT_ASSERT_NO_THROW(view->removeQuotaNode(view->getContainer(path3).get()));
  CPPUNIT_ASSERT_THROW(view->removeQuotaNode(view->getContainer(path3).get()),
                       eos::MDException);

  for (int i = 1; i <= 10; ++i) {
    CPPUNIT_ASSERT(parentNode->getPhysicalSpaceByUser(i)  ==
                   users1[i].physicalSpace + users2[i].physicalSpace + users3[i].physicalSpace);
    CPPUNIT_ASSERT(parentNode->getUsedSpaceByUser(i) ==
                   users1[i].space + users2[i].space + users3[i].space);
    CPPUNIT_ASSERT(parentNode->getNumFilesByUser(i) ==
                   users1[i].files + users2[i].files + users3[i].files);
  }

  for (int i = 1; i <= 3; ++i) {
    CPPUNIT_ASSERT(parentNode->getPhysicalSpaceByGroup(i)  ==
                   groups1[i].physicalSpace + groups2[i].physicalSpace + groups3[i].physicalSpace);
    CPPUNIT_ASSERT(parentNode->getUsedSpaceByGroup(i) ==
                   groups1[i].space + groups2[i].space + groups3[i].space);
    CPPUNIT_ASSERT(parentNode->getNumFilesByGroup(i) ==
                   groups1[i].files + groups2[i].files + groups3[i].files);
  }

  // Clean up
  // Remove all the quota nodes
  CPPUNIT_ASSERT_THROW(view->removeQuotaNode(cont1.get()), eos::MDException);
  CPPUNIT_ASSERT_THROW(view->removeQuotaNode(cont2.get()), eos::MDException);
  CPPUNIT_ASSERT_THROW(view->removeQuotaNode(cont3.get()), eos::MDException);
  CPPUNIT_ASSERT_THROW(view->removeQuotaNode(cont4.get()), eos::MDException);
  CPPUNIT_ASSERT_NO_THROW(view->removeQuotaNode(cont5.get()));
  // Remove all the files
  std::list<std::string> paths {path1, path2, path3};

  for (auto && path_elem : paths) {
    for (int i = 0; i < 1000; ++i) {
      std::ostringstream p;
      p << path_elem << "file" << i;
      std::shared_ptr<eos::IFileMD> file {view->getFile(p.str())};
      CPPUNIT_ASSERT_NO_THROW(view->unlinkFile(p.str()));
      CPPUNIT_ASSERT_NO_THROW(view->removeFile(view->getFileMDSvc()->getFileMD(
                                file->getId()).get()));
    }
  }

  // Remove all containers
  CPPUNIT_ASSERT_NO_THROW(view->removeContainer("/test/", true));
  // Remove the root container
  std::shared_ptr<eos::IContainerMD> root {view->getContainer("/")};
  CPPUNIT_ASSERT_NO_THROW(contSvc->removeContainer(root.get()));
  CPPUNIT_ASSERT_NO_THROW(view->finalize());
}

//------------------------------------------------------------------------------
// Lost container test
//------------------------------------------------------------------------------
void HierarchicalViewTest::lostContainerTest()
{
  // Initializer
  std::map<std::string, std::string> config = {
    {"redis_host", "localhost"},
    {"redis_port", "6380"}
  };
  std::unique_ptr<eos::ContainerMDSvc> contSvc {new eos::ContainerMDSvc()};
  std::unique_ptr<eos::FileMDSvc> fileSvc {new eos::FileMDSvc()};
  std::unique_ptr<eos::IView> view {new eos::HierarchicalView()};
  fileSvc->setContMDService(contSvc.get());
  fileSvc->configure(config);
  contSvc->configure(config);
  contSvc->setFileMDService(fileSvc.get());
  view->setContainerMDSvc(contSvc.get());
  view->setFileMDSvc(fileSvc.get());
  view->configure(config);
  view->initialize();
  std::shared_ptr<eos::IContainerMD> cont1 {view->createContainer("/test/embed/embed1", true)};
  std::shared_ptr<eos::IContainerMD> cont2 {view->createContainer("/test/embed/embed2", true)};
  std::shared_ptr<eos::IContainerMD> cont3 {view->createContainer("/test/embed/embed3", true)};
  std::shared_ptr<eos::IContainerMD> cont4 {view->createContainer("/test/embed/embed1/embedembed", true)};
  std::shared_ptr<eos::IContainerMD> cont5 {view->createContainer("/test/embed/embed3.conflict", true)};

  // Create some files
  for (int i = 0; i < 1000; ++i) {
    std::ostringstream s1;
    s1 << "/test/embed/embed1/file" << i;
    std::ostringstream s2;
    s2 << "/test/embed/embed2/file" << i;
    std::ostringstream s3;
    s3 << "/test/embed/embed3/file" << i;
    std::ostringstream s4;
    s4 << "/test/embed/embed1/embedembed/file" << i;
    std::ostringstream s5;
    s5 << "/test/embed/embed3.conflict/file" << i;
    std::ostringstream s6;
    s6 << "/test/embed/embed2/conflict_file" << i;
    view->createFile(s1.str());
    view->createFile(s2.str());
    view->createFile(s3.str());
    view->createFile(s4.str());
    view->createFile(s5.str());
    view->createFile(s6.str());
    std::shared_ptr<eos::IFileMD> file {view->getFile(s6.str())};

    if (i) {
      CPPUNIT_ASSERT_THROW(view->renameFile(file.get(), "conflict_file"),
                           eos::MDException);
    } else {
      CPPUNIT_ASSERT_NO_THROW(view->renameFile(file.get(), "conflict_file"));
    }
  }

  // Trying to remove a non-empty container should result in an exception
  CPPUNIT_ASSERT_THROW(view->getContainerMDSvc()->removeContainer(cont1.get()),
                       eos::MDException);
  // Trying to rename a container to an already existing one should result in
  // an exception
  CPPUNIT_ASSERT_THROW(cont5->setName("embed3"), eos::MDException);

  // Cleanup
  for (int i = 0; i < 1000; ++i) {
    std::ostringstream s1;
    s1 << "/test/embed/embed1/file" << i;
    std::ostringstream s2;
    s2 << "/test/embed/embed2/file" << i;
    std::ostringstream s3;
    s3 << "/test/embed/embed3/file" << i;
    std::ostringstream s4;
    s4 << "/test/embed/embed1/embedembed/file" << i;
    std::ostringstream s5;
    s5 << "/test/embed/embed3.conflict/file" << i;
    std::ostringstream s6;
    s6 << "/test/embed/embed2/conflict_file" << i;
    std::list<std::string> paths {s1.str(), s2.str(), s3.str(), s4.str(), s5.str()};

    if (i) {
      paths.insert(paths.end(), s6.str());
    }

    for (auto && elem : paths) {
      std::shared_ptr<eos::IFileMD> file {view->getFile(elem)};
      CPPUNIT_ASSERT_NO_THROW(view->unlinkFile(elem));
      CPPUNIT_ASSERT_NO_THROW(view->removeFile(view->getFileMDSvc()->getFileMD(
                                file->getId()).get()));
    }
  }

  // Remove the conflict_file
  std::string path = "test/embed/embed2/conflict_file";
  std::shared_ptr<eos::IFileMD> file {view->getFile(path)};
  CPPUNIT_ASSERT_NO_THROW(view->unlinkFile(path));
  CPPUNIT_ASSERT_NO_THROW(view->removeFile(view->getFileMDSvc()->getFileMD(
                            file->getId()).get()));
  // Remove all containers
  CPPUNIT_ASSERT_NO_THROW(view->removeContainer("/test/", true));
  // Remove the root container
  std::shared_ptr<eos::IContainerMD> root {view->getContainer("/")};
  CPPUNIT_ASSERT_NO_THROW(contSvc->removeContainer(root.get()));
  CPPUNIT_ASSERT_NO_THROW(view->finalize());
}
