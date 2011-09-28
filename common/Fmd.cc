/*----------------------------------------------------------------------------*/
#include "common/Namespace.hh"
#include "common/Fmd.hh"
#include "common/ClientAdmin.hh"
#include "common/FileId.hh"
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
#include <stdio.h>
#include <sys/mman.h>
/*----------------------------------------------------------------------------*/

EOSCOMMONNAMESPACE_BEGIN

/*----------------------------------------------------------------------------*/
FmdHandler gFmdHandler;
/*----------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------*/
bool 
FmdHeader::Read(int fd, bool ignoreversion) 
{
  // the header is already in the beginning
  lseek(fd,0,SEEK_SET);
  int nread = read(fd,&fmdHeader, sizeof(struct FMDHEADER));
  if (nread != sizeof(struct FMDHEADER)) {
    eos_crit("unable to read fmd header");
    return false;
  }

  eos_info("fmd header version %s creation time is %u filesystem id %04d", fmdHeader.version, fmdHeader.ctime, fmdHeader.fsid);
  if (strcmp(fmdHeader.version, FMDVERSION)) {
    if (!ignoreversion) {
      eos_crit("fmd header contains version %s but this is version %s", fmdHeader.version, FMDVERSION);
      return false;
    } else {
      eos_warning("fmd header contains version %s but this is version %s", fmdHeader.version, FMDVERSION);
    }
  }
  if (fmdHeader.magic != EOSCOMMONFMDHEADER_MAGIC) {
    eos_crit("fmd header magic is wrong - found %x", fmdHeader.magic);
    return false;
  }
  return true;
}
 
/*----------------------------------------------------------------------------*/
bool 
FmdHeader::Write(int fd) 
{
  // the header is always in the beginning
  lseek(fd,0,SEEK_SET);
  int nwrite = write(fd,&fmdHeader, sizeof(struct FMDHEADER));
  if (nwrite != sizeof(struct FMDHEADER)) {
    eos_crit("unable to write fmd header");
    return false;
  }
  eos_debug("wrote fmd header version %s creation time %u filesystem id %04d", fmdHeader.version, fmdHeader.ctime, fmdHeader.fsid);
  return true;
}

/*----------------------------------------------------------------------------*/
void
FmdHeader::Dump(struct FMDHEADER* header) 
{
  time_t then = (time_t) header->ctime;
  XrdOucString stime = ctime(&then);
  stime.erase(stime.length()-1);

  fprintf(stdout,"HEADER: [%s] magic=%llx version=%s ctime=%llu fsid=%d\n",stime.c_str(),header->magic, header->version,header->ctime,header->fsid);
}


/*----------------------------------------------------------------------------*/
bool 
Fmd::Write(int fd) 
{
  // compute crc32
  fMd.crc32 = ComputeCrc32((char*)(&(fMd.fid)), sizeof(struct FMD) - sizeof(fMd.magic) - (2*sizeof(fMd.sequencetrailer)) - sizeof(fMd.crc32));
  eos_debug("computed meta CRC for fileid %d to %x", fMd.fid, fMd.crc32);
  if ( (write(fd, &fMd, sizeof(fMd))) != sizeof(fMd)) {
    eos_crit("failed to write fmd struct");
    return false;
  }
  return true;
}

/*----------------------------------------------------------------------------*/
bool 
Fmd::Read(int fd, off_t offset) 
{
  if ( (pread(fd, &fMd, sizeof(fMd),offset)) != sizeof(fMd)) {
    eos_crit("failed to read fmd struct");
    return false;
  }

  return true;
}

/*----------------------------------------------------------------------------*/
void
Fmd::Dump(struct FMD* fmd) {
  XrdOucString magic="?";
  XrdOucString checksum="";
  if (IsCreate(fmd)) {
    magic = "C";
  }
  if (IsDelete(fmd)) {
    magic = "D";
  }

  for (unsigned int i=0; i< SHA_DIGEST_LENGTH; i++) {
    char hb[3]; sprintf(hb,"%02x", (unsigned char) (fmd->checksum[i]));
    checksum += hb;
  }
    
  fprintf(stderr,"%s %06lu %08llx %06llu %04lu %010lu %010lu %010lu %010lu %08llu %s %03lu %05u %05u %32s %s %06lu %06lu\n",
          magic.c_str(),
          fmd->sequenceheader,
          fmd->fid,
          fmd->cid,
          fmd->fsid,
          fmd->ctime,
          fmd->ctime_ns,
          fmd->mtime,
          fmd->mtime_ns,
          fmd->size,
          checksum.c_str(),
          fmd->lid,
          fmd->uid,
          fmd->gid,
          fmd->name,
          fmd->container,
          fmd->crc32,
          fmd->sequencetrailer);
}

/*----------------------------------------------------------------------------*/
bool
FmdHandler::SetChangeLogFile(const char* changelogfilename, int fsid, XrdOucString option) 
{
  // option is pass from the fsck executable in XrdFstOfs
  // this is identified via the option field 'c'
  // for fsck we don't want to automatically create a new changelog file if the path is not yet existing !

  eos_debug("");

  RWMutexWriteLock lock(Mutex);

  bool isNew=false;

  if (!FmdMap.count(fsid)) {
    FmdMap[fsid].set_empty_key(0xffffffffeULL);
    FmdMap[fsid].set_deleted_key(0xffffffffULL);
  }

  char fsChangeLogFileName[1024];
  sprintf(fsChangeLogFileName,"%s.%04d.mdlog", ChangeLogFileName.c_str(),fsid);
  
  if (fdChangeLogRead[fsid]>0) {
    eos_info("closing changelog read file %s", ChangeLogFileName.c_str());
    close(fdChangeLogRead[fsid]);
  }
  if (fdChangeLogWrite[fsid]>0) {
    eos_info("closing changelog read file %s", ChangeLogFileName.c_str());
    close(fdChangeLogWrite[fsid]);
  }

  sprintf(fsChangeLogFileName,"%s.%04d.mdlog", changelogfilename,fsid);
  ChangeLogFileName = changelogfilename;
  eos_info("changelog file is now %s\n", ChangeLogFileName.c_str());

  // check if this is a new changelog file
  if ((access(fsChangeLogFileName,R_OK))) {
    isNew = true;
  }

  if (((option.find("c"))!=STR_NPOS) && isNew) {
    // we don't want to create a new file and return an error!
    fdChangeLogWrite[fsid] = fdChangeLogRead[fsid] = -1;
    eos_err("changelog file is not existing: %s\n", ChangeLogFileName.c_str());
    return false;
  }


  if ( (fdChangeLogWrite[fsid] = open(fsChangeLogFileName,O_CREAT| O_RDWR, 0600 )) <0) {
    eos_err("unable to open changelog file for writing %s",fsChangeLogFileName);
    fdChangeLogWrite[fsid] = fdChangeLogRead[fsid] = -1;
    return false;
  }

  // spool to the end
  lseek(fdChangeLogWrite[fsid], 0, SEEK_END);
 
  if ( (fdChangeLogRead[fsid] = open(fsChangeLogFileName,O_RDONLY)) < 0) {
    eos_err("unable to open changelog file for writing %s",fsChangeLogFileName);
    close(fdChangeLogWrite[fsid]);
    fdChangeLogWrite[fsid] = fdChangeLogRead[fsid] = -1;
    return false;
  }
  eos_info("opened changelog file %s for filesystem %04d", fsChangeLogFileName, fsid);

  if (isNew) {
    // try to write the header
    fmdHeader.SetId(fsid);
    fmdHeader.SetLogId("FmdHeader");
    if (!fmdHeader.Write(fdChangeLogWrite[fsid])) {
      // cannot write the header 
      isOpen=false;
      return isOpen;
    }
  }

  isOpen = ReadChangeLogHash(fsid, option);

  return isOpen;
}


int 
FmdHandler::CompareMtime(const void* a, const void *b) {
  struct filestat {
    struct stat buf;
    char filename[1024];
  };
  return ( (((struct filestat*)b)->buf.st_mtime) - ((struct filestat*)a)->buf.st_mtime);
}

/*----------------------------------------------------------------------------*/
bool FmdHandler::AttachLatestChangeLogFile(const char* changelogdir, int fsid) 
{
  eos_debug("");
  DIR *dir;
  struct dirent *dp;
  struct filestat {
    struct stat buf;
    char filename[1024];
  };

  eos_debug("before set");
  if (!FmdMap.count(fsid)) {
    FmdMap[fsid].set_empty_key(0xffffffffeULL);
    FmdMap[fsid].set_deleted_key(0xffffffffULL);
  }
  eos_debug("after set");

  int nobjects=0;
  long tdp=0;

  struct filestat* allstat = 0;
  ChangeLogDir = changelogdir;
  ChangeLogDir += "/";
  while (ChangeLogDir.replace("//","/")) {};
  XrdOucString Directory = changelogdir;
  XrdOucString FileName ="";
  char fileend[1024];
  sprintf(fileend,".%04d.mdlog",fsid);
  if ((dir = opendir(Directory.c_str()))) {
    tdp = telldir(dir);
    while ((dp = readdir (dir)) != 0) {
      FileName = dp->d_name;
      if ( (!strcmp(dp->d_name,".")) || (!strcmp(dp->d_name,".."))
           || (strlen(dp->d_name) != strlen("fmd.1272892439.0000.mdlog")) || (strncmp(dp->d_name,"fmd.",4)) || (!FileName.endswith(fileend)))
        continue;
      
      nobjects++;
    }
    allstat = (struct filestat*) malloc(sizeof(struct filestat) * nobjects);
    if (!allstat) {
      eos_err("cannot allocate sorting array");
      if (dir)
        closedir(dir);
      return false;
    }
   
    eos_debug("found %d old changelog files\n", nobjects);
    // go back
    seekdir(dir,tdp);
    int i=0;
    while ((dp = readdir (dir)) != 0) {
      FileName = dp->d_name;
      if ( (!strcmp(dp->d_name,".")) || (!strcmp(dp->d_name,".."))
           || (strlen(dp->d_name) != strlen("fmd.1272892439.0000.mdlog")) || (strncmp(dp->d_name,"fmd.",4)) || (!FileName.endswith(fileend)))
        continue;
      char fullpath[8192];
      sprintf(fullpath,"%s/%s",Directory.c_str(),dp->d_name);

      sprintf(allstat[i].filename,"%s",dp->d_name);
      eos_debug("stat on %s\n", dp->d_name);
      if (stat(fullpath, &(allstat[i].buf))) {
        eos_err("cannot stat after readdir file %s", fullpath);
      }
      i++;
    }
    closedir(dir);
    // do the sorting
    qsort(allstat,nobjects,sizeof(struct filestat),FmdHandler::CompareMtime);
  } else {
    eos_err("cannot open changelog directory",Directory.c_str());
    return false;
  }
  
  XrdOucString changelogfilename = changelogdir;

  if (allstat && (nobjects>0)) {
    // attach an existing one
    changelogfilename += "/";
    while (changelogfilename.replace("//","/")) {};
    changelogfilename += allstat[0].filename;
    changelogfilename.replace(fileend,"");
    eos_info("attaching existing changelog file %s", changelogfilename.c_str());
    free(allstat);
  } else {
    // attach a new one
    CreateChangeLogName(changelogfilename.c_str(), changelogfilename);
    eos_info("creating new changelog file %s", changelogfilename.c_str());
    if (allstat) 
      free(allstat);
  }
  // initialize sequence number
  fdChangeLogSequenceNumber[fsid]=0;
  return SetChangeLogFile(changelogfilename.c_str(),fsid);
}

/*----------------------------------------------------------------------------*/
bool FmdHandler::ReadChangeLogHash(int fsid, XrdOucString option) 
{
  eos_debug("");

  struct timeval tv1,tv2;
  struct timezone tz;

  gettimeofday(&tv1,&tz);

  bool ignoreversion=false;
  bool dump=false;
  bool isfsck=false;
  // force option to read all versions

  if (option.find("f")!=STR_NPOS) 
    ignoreversion=true;
  if (option.find("d")!=STR_NPOS)
    dump=true;
  if (option.find("c")!=STR_NPOS)
    isfsck=true;

  
  if (!fmdHeader.Read(fdChangeLogRead[fsid],ignoreversion)) {
    // failed to read header
    return false;
  }
  struct stat stbuf; 

  if (dump) {
    FmdHeader::Dump(&fmdHeader.fmdHeader);
  }

  if (::fstat(fdChangeLogRead[fsid], &stbuf)) {
    eos_crit("unable to stat file size of changelog file - errc%d", errno);
    return false;
  }

  if (stbuf.st_size > (6 * 1000ll*1000ll*1000ll)) {
    // we don't map more than 6 GB ... should first trim here
    eos_crit("changelog file exceeds memory limit of 6 GB for boot procedure");
    return false;
  }
  
  // mmap the complete changelog ... wow!

  if ( (unsigned long)stbuf.st_size <= sizeof(struct FmdHeader::FMDHEADER)) {
    eos_info("changelog is empty - nothing to check");
    return true;
  }

  char* changelogmap   =  (char*) mmap(0, stbuf.st_size, PROT_READ, MAP_SHARED, fdChangeLogRead[fsid],0);

  if (!changelogmap) {
    eos_crit("unable to mmap changelog file - errc=%d",errno);
    return false;
  }

  char* changelogstart =  changelogmap + sizeof(struct FmdHeader::FMDHEADER);;
  char* changelogstop  =  changelogmap + stbuf.st_size;
  struct Fmd::FMD* pMd;
  bool success = true;
  unsigned long sequencenumber=0;
  int retc=0;
  unsigned long long nchecked=0;
  unsigned long long errormagic=0;
  unsigned long long errorcrc=0;
  unsigned long long errorsequence=0;
  unsigned long long errormismatch=0;

  eos_debug("memory mapped changelog file at %lu", changelogstart);

  while ( (changelogstart+sizeof(struct Fmd::FMD)) <= changelogstop) {
    bool faulty=false;
    nchecked++;
    if (!(nchecked%1000)) {
      eos_info("checking SEQ# %d # %d", sequencenumber, nchecked);
    } else {
      eos_debug("checking SEQ# %d # %d", sequencenumber, nchecked);
    }

    pMd = (struct Fmd::FMD*) changelogstart;
    eos_debug("%llx %llx %ld %llx %lu %llu %lu %x", pMd, &(pMd->fid), sizeof(*pMd), pMd->magic, pMd->sequenceheader, pMd->fid, pMd->fsid, pMd->crc32);
    if (!(retc = Fmd::IsValid(pMd, sequencenumber))) {
      // good meta data block
    } else {
      // illegal meta data block
      if (retc == EINVAL) {
        eos_crit("Block is neither creation/update or deletion block %u offset %llu", sequencenumber, ((char*)pMd) - changelogmap);
        faulty = true;
        errormagic++;
      }
      if (retc == EILSEQ) {
        eos_crit("CRC32 error in meta data block sequencenumber %u offset %llu", sequencenumber, ((char*)pMd) - changelogmap);
        faulty = true;
        errorcrc++;
      }
      if (retc == EOVERFLOW) {
        eos_crit("SEQ# error in meta data block sequencenumber %u offset %llu", sequencenumber, ((char*)pMd) - changelogmap);
        faulty = true;
        errorsequence++;
      }
      if (retc == EFAULT) {
        eos_crit("SEQ header/trailer mismatch in meta data block sequencenumber %u/%u offset %llu", pMd->sequenceheader,pMd->sequencetrailer, ((char*)pMd) - changelogmap);
        faulty = true;
        errormismatch++;
      }
      success = false;
    }
    
    if (!faulty && dump) {
      Fmd::Dump(pMd);
    }

    if (pMd->sequenceheader > (unsigned long long) fdChangeLogSequenceNumber[fsid]) {
      fdChangeLogSequenceNumber[fsid] = pMd->sequenceheader;
    }


    if (!faulty) {
      // setup the hash entries
      FmdMap[fsid][pMd->fid] = (unsigned long long) (changelogstart-changelogmap);
      
      // do quota hashs
      if (Fmd::IsCreate(pMd)) {

        long long exsize = -1;
        if (FmdSize.count(pMd->fid)>0) {
          // exists
          exsize = FmdSize[pMd->fid];
        }
        
        // store new size
        FmdSize[pMd->fid] = pMd->size;
        
      }
      if (Fmd::IsDelete(pMd)) {
        if (FmdSize.count(pMd->fid)>0) {
          FmdMap[fsid].erase(pMd->fid);
          FmdSize.erase(pMd->fid);
        } else {
          eos_crit("Double Deletion detected sequencenumber %u fid %llu", sequencenumber, pMd->fid);
        }
      } 
    }
    pMd++;
    changelogstart += sizeof(struct Fmd::FMD);
  }

  FmdSize.resize(0);

  munmap(changelogmap, stbuf.st_size);
  eos_debug("checked %d FMD entries",nchecked);

  gettimeofday(&tv2,&tz);

  if (isfsck) {
    float rtime  = ( ((tv2.tv_sec -tv1.tv_sec)*1.0) + ((tv2.tv_usec-tv1.tv_usec)/1000000.0));
    fprintf(stdout,"---------------------------------------\n");
    fprintf(stdout,"=> FSCK Runtime     : %.02f sec\n", rtime);
    fprintf(stdout,"=> FMD Entries      : %llu\n", nchecked);
    fprintf(stdout,"=> Speed            : %.02f\n", nchecked/rtime);
    fprintf(stdout,"---------------------------------------\n");
    fprintf(stdout,"=> Error Magic      : %llu\n", errormagic);
    fprintf(stdout,"=> Error CRC32      : %llu\n", errorcrc);
    fprintf(stdout,"=> Error Sequence   : %llu\n", errorsequence);
    fprintf(stdout,"=> Error HT-Mismatch: %llu\n", errormismatch);
    fprintf(stdout,"---------------------------------------\n");
  }
  return success;
}

/*----------------------------------------------------------------------------*/
Fmd*
FmdHandler::GetFmd(unsigned long long fid, unsigned int fsid, uid_t uid, gid_t gid, unsigned int layoutid, bool isRW) 
{
  RWMutexWriteLock lock(Mutex);
  if (fdChangeLogRead[fsid]>0) {
    if (FmdMap[fsid][fid] != 0) {
      // this is to read an existing entry
      Fmd* fmd = new Fmd();
      if (!fmd) return 0;

      if (!fmd->Read(fdChangeLogRead[fsid],FmdMap[fsid][fid])) {
        eos_crit("unable to read block for fid %d on fs %d", fid, fsid);
        delete fmd;
        return 0;
      }
      if ( fmd->fMd.fid != fid) {
        // fatal this is somehow a wrong record!
        eos_crit("unable to get fmd for fid %d on fs %d - file id mismatch in meta data block", fid, fsid);
        delete fmd;
        return 0;
      }
      if ( fmd->fMd.fsid != fsid) {
        // fatal this is somehow a wrong record!
        eos_crit("unable to get fmd for fid %d on fs %d - filesystem id mismatch in meta data block", fid, fsid);
        delete fmd;
        return 0;
      }
      // return the new entry
      return fmd;
    }
    if (isRW) {
      // make a new record
      Fmd* fmd = new Fmd(fid, fsid);
      if (!fmd)
        return 0;

      fmd->MakeCreationBlock();
      
      if (fdChangeLogWrite[fsid]>0) {
        off_t position = lseek(fdChangeLogWrite[fsid],0,SEEK_CUR);

        fdChangeLogSequenceNumber[fsid]++;
        // set sequence number
        fmd->fMd.uid = uid;
        fmd->fMd.gid = gid;
        fmd->fMd.lid = layoutid;
        fmd->fMd.sequenceheader=fmd->fMd.sequencetrailer = fdChangeLogSequenceNumber[fsid];
        fmd->fMd.ctime = fmd->fMd.mtime = time(0);
        // get micro seconds
        struct timeval tv;
        struct timezone tz;
        
        gettimeofday(&tv, &tz);
        fmd->fMd.ctime_ns = fmd->fMd.mtime_ns = tv.tv_usec * 1000;
        
        // write this block
        if (!fmd->Write(fdChangeLogWrite[fsid])) {
          // failed to write
          eos_crit("failed to write new block for fid %d on fs %d", fid, fsid);
          delete fmd;
          return 0;
        }
        // add to the in-memory hashes
        FmdMap[fsid][fid]     = position;
        FmdSize[fid] = 0;

        eos_debug("returning meta data block for fid %d on fs %d", fid, fsid);
        // return the mmaped meta data block
        return fmd;
      } else {
        eos_crit("unable to write new block for fid %d on fs %d - no changelog file open for writing", fid, fsid);
        delete fmd;
        return 0;
      }
    } else {
      eos_err("unable to get fmd for fid %d on fs %d - record not found", fid, fsid);
      return 0;
    }
  } else {
    eos_crit("unable to get fmd for fid %d on fs %d - there is no changelog file open for that file system id", fid, fsid);
    return 0;
  }
}


/*----------------------------------------------------------------------------*/
bool
FmdHandler::DeleteFmd(unsigned long long fid, unsigned int fsid) 
{
  bool rc = true;
  eos_static_info("");
  Fmd* fmd = FmdHandler::GetFmd(fid,fsid, 0,0,0,false);\
  if (!fmd) 
    rc = false;
  else {
    fmd->fMd.magic = EOSCOMMONFMDDELETE_MAGIC;
    fmd->fMd.size  = 0;
    rc = Commit(fmd);
    delete fmd;
  }

  // erase the has entries
  RWMutexWriteLock lock(Mutex);
    
  FmdMap[fsid].erase(fid);
  FmdSize.erase(fid);

  return rc;
}


/*----------------------------------------------------------------------------*/
bool
FmdHandler::Commit(Fmd* fmd)
{
  if (!fmd)
    return false;

  int fsid = fmd->fMd.fsid;
  int fid  = fmd->fMd.fid;

  RWMutexWriteLock lock(Mutex);
  
  // get file position
  off_t position = lseek(fdChangeLogWrite[fsid],0,SEEK_CUR);
  fdChangeLogSequenceNumber[fsid]++;
  // set sequence number
  fmd->fMd.sequenceheader=fmd->fMd.sequencetrailer = fdChangeLogSequenceNumber[fsid];

  // put modification time
  fmd->fMd.mtime = time(0);
  // get micro seconds
  struct timeval tv;
  struct timezone tz;
  
  gettimeofday(&tv, &tz);
  fmd->fMd.mtime_ns = tv.tv_usec * 1000;
  
  // write this block
  if (!fmd->Write(fdChangeLogWrite[fsid])) {
    // failed to write
    eos_crit("failed to write commit block for fid %d on fs %d", fid, fsid);
    return false;
  }

  // store present size
  // add to the in-memory hashes
  FmdMap[fsid][fid]     = position;
  FmdSize[fid] = fmd->fMd.size;

  return true;
}

/*----------------------------------------------------------------------------*/
bool
FmdHandler::TrimLogFile(int fsid, XrdOucString option) {
  bool rc=true;
  char newfilename[1024];
  sprintf(newfilename,".%04d.mdlog", fsid);
  XrdOucString NewChangeLogFileName;
  CreateChangeLogName(ChangeLogDir.c_str(),NewChangeLogFileName);
  NewChangeLogFileName += newfilename;
  XrdOucString NewChangeLogFileNameTmp = NewChangeLogFileName;
  NewChangeLogFileNameTmp+= ".tmp";

  struct stat statbefore;
  struct stat statafter;

  // stat before trim
  fstat(fdChangeLogRead[fsid], &statbefore);

  int newfd = open(NewChangeLogFileNameTmp.c_str(),O_CREAT|O_TRUNC| O_RDWR, 0600);

  eos_static_info("trimming opening new changelog file %s\n", NewChangeLogFileNameTmp.c_str());
  if (newfd<0) 
    return false;

  int newrfd = open(NewChangeLogFileNameTmp.c_str(),O_RDONLY);
  
  if (newrfd<0) {
    close(newfd);
    return false;
  }

  // write new header
  if (!fmdHeader.Write(newfd)) {
    close(newfd);
    close(newrfd);
    return false;
  }

  std::vector <unsigned long long> alloffsets;
  google::dense_hash_map <unsigned long long, unsigned long long> offsetmapping;
  offsetmapping.set_empty_key(0xffffffff);

  eos_static_info("trimming step 1");

  google::dense_hash_map<unsigned long long, unsigned long long>::const_iterator it;
  for (it = FmdMap[fsid].begin(); it != FmdMap[fsid].end(); ++it) {
    eos_static_info("adding offset %llu to fsid %u",it->second, fsid);
    alloffsets.push_back(it->second);
  }
  eos_static_info("trimming step 2");

  // sort the offsets
  std::sort(alloffsets.begin(), alloffsets.end());
  eos_static_info("trimming step 3");

  // write the new file
  std::vector<unsigned long long>::const_iterator fmdit;
  Fmd fmdblock;
  int rfd = dup(fdChangeLogRead[fsid]);

  eos_static_info("trimming step 4");

  if (rfd > 0) {
    for (fmdit = alloffsets.begin(); fmdit != alloffsets.end(); ++fmdit) {
      if (!fmdblock.Read(rfd,*fmdit)) {
        eos_static_crit("fatal error reading active changelog file at position %llu",*fmdit);
        rc = false;
        break;
      } else {
        off_t newoffset = lseek(newfd,0,SEEK_CUR);
        offsetmapping[*fmdit] = newoffset;
        if (!fmdblock.Write(newfd)) {
          rc = false;
          break; 
        }
      }
      
    }
  } else {
    eos_static_crit("fatal error duplicating read file descriptor");
    rc = false;
  }

  eos_static_info("trimming step 5");

  if (rc) {
    // now we take a lock, copy the latest changes since the trimming to the new file and exchange the current filedescriptor
    long long oldtailoffset = lseek(fdChangeLogWrite[fsid],0,SEEK_CUR);
    long long newtailoffset = lseek(newfd,0,SEEK_CUR);
    long long offset = oldtailoffset;

    ssize_t tailchange = oldtailoffset-newtailoffset;
    eos_static_info("tail length is %llu [ %llu %llu %llu ] ", tailchange, oldtailoffset, newtailoffset, offset);
    char copybuffer[128*1024];
    int nread=0;
    do {
      nread = pread(rfd, copybuffer, sizeof(copybuffer), offset);
      if (nread>0) {
        offset += nread;
        int nwrite = write(newfd,copybuffer,nread);
        if (nwrite != nread) {
          eos_static_crit("fatal error doing last recent change copy");
          rc = false;
          break;
        }
      }
    } while (nread>0);

    
    // now adjust the in-memory map
    // clean up erased
    FmdMap[fsid].resize(0);
    FmdSize.resize(0);

    google::dense_hash_map<unsigned long long, unsigned long long>::iterator it;
    for (it = FmdMap[fsid].begin(); it != FmdMap[fsid].end(); it++) {
      if ((long long)it->second >= oldtailoffset) {
        // this has just to be adjusted by the trim length
        it->second -= tailchange;
      } else {
        if (offsetmapping.count(it->second)) {
          // that is what we expect
          it->second = offsetmapping[it->second];
        } else {
          // that should never happen!
          eos_static_crit("fatal error found not mapped offset position during trim procedure!");
          rc = false;
        }
      }      
    }
    if (!rename(NewChangeLogFileNameTmp.c_str(),NewChangeLogFileName.c_str())) {
      // now high-jack the old write and read filedescriptor;
      close(fdChangeLogWrite[fsid]);
      close(fdChangeLogRead[fsid]);
      fdChangeLogWrite[fsid] = newfd;
      fdChangeLogRead[fsid] = newrfd;
      ChangeLogFileName = NewChangeLogFileName;
    } else {
      eos_static_crit("cannot move the temporary trim file into active file");
      rc = false;
    }
  }
  eos_static_info("trimming step 6");  

  if (rfd>0) 
    close(rfd);

  // stat after trim
  fstat(fdChangeLogRead[fsid], &statafter);

  if ((option.find("c"))!=STR_NPOS) {
    if (rc) {
      fprintf(stdout,"---------------------------------------\n");
      fprintf(stdout,"=> Trim CL File     : %s\n", NewChangeLogFileName.c_str());
      fprintf(stdout,"=> Original Size    : %llu\n", (unsigned long long)statbefore.st_size);
      fprintf(stdout,"=> Trimmed Size     : %llu\n", (unsigned long long)statafter.st_size);
      fprintf(stdout,"---------------------------------------\n");
    } else {
      fprintf(stderr,"error: trimming failed!\n");
    }

  }
  return rc;
}

/*----------------------------------------------------------------------------*/
XrdOucEnv*
Fmd::FmdToEnv() 
{
  char serialized[1024*64];
  XrdOucString base64checksum;

  // base64 encode the checksum
  SymKey::Base64Encode(fMd.checksum, SHA_DIGEST_LENGTH, base64checksum);

  sprintf(serialized,"mgm.fmd.magic=%llu&mgm.fmd.sequenceheader=%lu&mgm.fmd.fid=%llu&mgm.fmd.cid=%llu&mgm.fmd.fsid=%lu&mgm.fmd.ctime=%lu&mgm.fmd.ctime_ns=%lu&mgm.fmd.mtime=%lu&mgm.fmd.mtime_ns=%lu&mgm.fmd.size=%llu&mgm.fmd.checksum64=%s&mgm.fmd.lid=%lu&mgm.fmd.uid=%u&mgm.fmd.gid=%u&mgm.fmd.name=%s&mgm.fmd.container=%s&mgm.fmd.crc32=%lu&mgm.fmd.sequencetrailer=%lu",
          fMd.magic,fMd.sequenceheader,fMd.fid,fMd.cid,fMd.fsid,fMd.ctime,fMd.ctime_ns,fMd.mtime,fMd.mtime_ns,fMd.size,base64checksum.c_str(),fMd.lid,fMd.uid,fMd.gid,fMd.name,fMd.container,fMd.crc32,fMd.sequencetrailer);
  return new XrdOucEnv(serialized);
};

/*----------------------------------------------------------------------------*/
bool 
Fmd::EnvToFmd(XrdOucEnv &env, struct Fmd::FMD &fmd)
{
  // check that all tags are present
  if ( !env.Get("mgm.fmd.magic") ||
       !env.Get("mgm.fmd.sequenceheader") ||
       !env.Get("mgm.fmd.fid") ||
       !env.Get("mgm.fmd.cid") ||
       !env.Get("mgm.fmd.fsid") ||
       !env.Get("mgm.fmd.ctime") ||
       !env.Get("mgm.fmd.ctime_ns") ||
       !env.Get("mgm.fmd.mtime") ||
       !env.Get("mgm.fmd.mtime_ns") ||
       !env.Get("mgm.fmd.size") ||
       !env.Get("mgm.fmd.checksum64") ||
       !env.Get("mgm.fmd.lid") ||
       !env.Get("mgm.fmd.uid") ||
       !env.Get("mgm.fmd.gid") ||
       //       !env.Get("mgm.fmd.name") ||
       //       !env.Get("mgm.fmd.container") ||
       !env.Get("mgm.fmd.crc32") ||
       !env.Get("mgm.fmd.sequencetrailer"))
    return false;

  // base64 decode
  XrdOucString checksum64 = env.Get("mgm.fmd.checksum64");
  unsigned int checksumlen;
  memset(fmd.checksum, 0, SHA_DIGEST_LENGTH);

  char* decodebuffer=0;
  if (!SymKey::Base64Decode(checksum64, decodebuffer, checksumlen))
    return false;

  memcpy(fmd.checksum,decodebuffer,SHA_DIGEST_LENGTH);
  free(decodebuffer);

  fmd.magic           = strtoull(env.Get("mgm.fmd.magic"),0,10);
  fmd.sequencetrailer = strtoul(env.Get("mgm.fmd.sequenceheader"),0,10);
  fmd.fid             = strtoull(env.Get("mgm.fmd.fid"),0,10);
  fmd.cid             = strtoull(env.Get("mgm.fmd.cid"),0,10);
  fmd.fsid            = strtoul(env.Get("mgm.fmd.fsid"),0,10);
  fmd.ctime           = strtoul(env.Get("mgm.fmd.ctime"),0,10);
  fmd.ctime_ns        = strtoul(env.Get("mgm.fmd.ctime_ns"),0,10);
  fmd.mtime           = strtoul(env.Get("mgm.fmd.mtime"),0,10);
  fmd.mtime_ns        = strtoul(env.Get("mgm.fmd.mtime_ns"),0,10);
  fmd.size            = strtoull(env.Get("mgm.fmd.size"),0,10);
  fmd.lid             = strtoul(env.Get("mgm.fmd.lid"),0,10);
  fmd.uid             = (uid_t) strtoul(env.Get("mgm.fmd.uid"),0,10);
  fmd.gid             = (gid_t) strtoul(env.Get("mgm.fmd.gid"),0,10);
  if (env.Get("mgm.fmd.name")) {
    strncpy(fmd.name,      env.Get("mgm.fmd.name"),255);
  } else {
    fmd.name[0]=0;
  }

  if (env.Get("mgm.fmd.container")) {
    strncpy(fmd.container, env.Get("mgm.fmd.container"),255);
  } else {
    fmd.container[0]=0;
  }
  fmd.crc32           = strtoul(env.Get("mgm.fmd.crc32"),NULL,0);
  fmd.sequencetrailer = strtoul(env.Get("mgm.fmd.sequencetrailer"),NULL,0);
   
  return true;
}

/*----------------------------------------------------------------------------*/
int
FmdHandler::GetRemoteFmd(ClientAdmin* admin, const char* serverurl, const char* shexfid, const char* sfsid, struct Fmd::FMD &fmd)
{
  char result[64*1024]; result[0]=0;
  int  result_size=64*1024;

  XrdOucString fmdquery="/?fst.pcmd=getfmd&fst.getfmd.fid=";fmdquery += shexfid;
  fmdquery += "&fst.getfmd.fsid="; fmdquery += sfsid;

  if ( (!serverurl) || (!shexfid) || (!sfsid) ) {
    return EINVAL;
  }

  int rc=0;
  admin->Lock();
  admin->GetAdmin()->Connect();
  admin->GetAdmin()->GetClientConn()->ClearLastServerError();
  admin->GetAdmin()->GetClientConn()->SetOpTimeLimit(10);
  admin->GetAdmin()->Query(kXR_Qopaquf,
                           (kXR_char *) fmdquery.c_str(),
                           (kXR_char *) result, result_size);
  
  if (!admin->GetAdmin()->LastServerResp()) {
    eos_static_err("Unable to retrieve meta data from server %s for fid=%s fsid=%s",serverurl, shexfid, sfsid);
    
    rc = 1;
  }
  switch (admin->GetAdmin()->LastServerResp()->status) {
  case kXR_ok:
    eos_static_debug("got replica file meta data from server %s for fid=%s fsid=%s",serverurl, shexfid, sfsid);
    rc = 0;
    break;
    
  case kXR_error:
    eos_static_err("Unable to retrieve meta data from server %s for fid=%s fsid=%s",serverurl, shexfid, sfsid);
    rc = ECOMM;
    break;
    
  default:
    rc = ECOMM;
    break;
  }
  admin->UnLock();

  if (rc) {
    return EIO;
  }

  if (!strncmp(result,"ERROR", 5)) {
    // remote side couldn't get the record
    eos_static_err("Unable to retrieve meta data on remote server %s for fid=%s fsid=%s",serverurl, shexfid, sfsid);
    return ENODATA;
  }
  // get the remote file meta data into an env hash
  XrdOucEnv fmdenv(result);

  if (!Fmd::EnvToFmd(fmdenv, fmd)) {
    int envlen;
    eos_static_err("Failed to unparse file meta data %s", fmdenv.Env(envlen));
    return EIO;
  }
  // very simple check
  if (fmd.fid != FileId::Hex2Fid(shexfid)) {
    eos_static_err("Uups! Received wrong meta data from remote server - fid is %lu instead of %lu !", fmd.fid, FileId::Hex2Fid(shexfid));
    return EIO;
  }

  return 0;
}

EOSCOMMONNAMESPACE_END


