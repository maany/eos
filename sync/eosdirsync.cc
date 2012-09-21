// ----------------------------------------------------------------------
// File: eosdirsync.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

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

#define PROGNAME "eosdirsync"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#include "XrdOuc/XrdOucString.hh"
#include "XrdSys/XrdSysTimer.hh"
#include "XrdClient/XrdClient.hh"
#include "XrdClient/XrdClientAdmin.hh"
#include "XrdClient/XrdClientEnv.hh"
#include "common/Logging.hh"

void usage() {
  fprintf(stderr,"usage: %s <src-dir> <dst-url-dir> [--debug]\n", PROGNAME);
  exit(-1);
}

#define TRANSFERBLOCKSIZE 1024*1024*4

bool forwardFile(XrdOucString &filename, XrdOucString &destfilename) {
  EnvPutInt(NAME_READCACHESIZE,0);
  EnvPutInt(NAME_MAXREDIRECTCOUNT,5);
  EnvPutInt(NAME_DATASERVERCONN_TTL,3600);
  XrdClient* client = new XrdClient(destfilename.c_str());

  if (!client) {
    eos_static_err("cannot create XrdClient object");
    return false;
  }

  XrdOucString destfile = destfilename;
  int pos1 = destfile.find("//");
  int pos2 = destfile.find("//",pos1+2);
  if (pos2 == STR_NPOS) {
    eos_static_crit("illegal destination specified %s", destfilename.c_str());
    exit(-1);
  }
      
  destfile.erase(0,pos2+1);

  XrdClientAdmin* admin = new XrdClientAdmin(destfilename.c_str());
  if (!admin) {
    eos_static_crit("cannot create client admin to %s\n", destfilename.c_str());
    exit(-1);
  } else {
    admin->Connect();
  }

  long id;
  long long size;
  long flags;
  long modtime;
  XrdClientStatInfo dststat;  
  bool isopen = false;
  // to check if the file exists already
  if (!admin->Stat(destfile.c_str(), id, size, flags,modtime)) {
    isopen = client->Open(kXR_ur | kXR_uw | kXR_gw | kXR_gr | kXR_or , kXR_mkpath | kXR_new, false);
  } else {
    isopen = client->Open(kXR_ur | kXR_uw | kXR_gw | kXR_gr | kXR_or , kXR_mkpath | kXR_open_updt , false);
  }
  delete admin;

  if (!isopen) {
    delete client;
    eos_static_err("cannot open remote file %s\n", destfilename.c_str());
    return false;
  }

  struct stat srcstat;


  bool success=true;

  int fd = open(filename.c_str(),O_RDONLY);
  if (fd<0) {
    eos_static_err("cannot open source file %s - errno=%d ", filename.c_str(),errno);
    success=false;
  } else {
    if (fstat(fd, &srcstat)) {
      eos_static_err("cannot stat source file %s - errno=%d ", filename.c_str(),errno);
      success = false;
    } else {
      if (!client->Stat(&dststat, true)) {
        eos_static_err("cannot stat destination file %s", destfilename.c_str());
        delete client;
        close(fd);
        return false;
      }

      if (dststat.size == srcstat.st_size) {
        // if the file exists already with the correct size we don't need to copoy
        delete client;
        close(fd);
        return true;
      }

      if (!client->Truncate(0)) {
        eos_static_err("cannot truncate remote file");
        success = false;
      } else {
        char* copyptr = (char*) mmap(NULL, srcstat.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (!copyptr) {
          eos_static_err("cannot map source file ");
          success = false;
        } else {
          for (unsigned long long offset = 0; offset < (unsigned long long)srcstat.st_size; offset += (TRANSFERBLOCKSIZE)) {
            unsigned long long length;
            if ( (srcstat.st_size -offset) > TRANSFERBLOCKSIZE) {
              length = TRANSFERBLOCKSIZE;
            } else {
              length = srcstat.st_size-offset;
            }
                
            if (!client->Write(copyptr + offset, offset,length )) {
              eos_static_err("cannot write remote block at %llu/%lu\n", (unsigned long long)offset, (unsigned long)length);
              success = false;
              break;
            }
          }
          munmap(copyptr,srcstat.st_size);
        }
      }
    }
    close(fd);
  }
  delete client;
  return success;
}

int main (int argc, char* argv[]) {
  if (argc < 3) {
    usage();
  }
  
  eos::common::Logging::Init();
  eos::common::Logging::SetUnit("eosdirsync");
  eos::common::Logging::SetLogPriority(LOG_NOTICE);
  XrdOucString debugstring="";

  if (argc==4) {
    debugstring = argv[3];
  }
  
  if ( (debugstring == "--debug") || (debugstring == "-d") ) {
    eos::common::Logging::SetLogPriority(LOG_DEBUG);
  }

  eos_static_notice("starting %s=>%s", argv[1],argv[2]);

  XrdOucString sourcedir = argv[1];
  XrdOucString dsturl = argv[2];
  struct stat laststat;
  struct stat presentstat;
  
  memset(&laststat   ,0,sizeof(struct stat));
  memset(&presentstat,0,sizeof(struct stat));

  do {
    if (stat(sourcedir.c_str(), &presentstat)) {
      eos_static_err("cannot stat source directory %s - errno=%d - retry in 1 minute ...", sourcedir.c_str(),errno);
      XrdSysTimer sleeper;
      sleeper.Wait(60000);
      continue;
    }

    if (presentstat.st_mtime != laststat.st_mtime) {
      // yes, there are modifications, loop over the contents in that directory
      DIR* dir = opendir(sourcedir.c_str());
      if (!dir) {
        eos_static_err("cannot open source directory %s - errno=%d - retry in 1 minute ...", sourcedir.c_str(),errno);
	XrdSysTimer sleeper;
	sleeper.Wait(60000);
        continue;
      }
      struct dirent *entry;

      while ( (entry = readdir(dir)) ) {
        XrdOucString dstfile = dsturl;
        XrdOucString sentry = sourcedir;
        XrdOucString file = entry->d_name;
        sentry += "/";
        sentry += entry->d_name;
        dstfile += "/";
        dstfile += entry->d_name;

        struct stat entrystat;
        if (stat(sentry.c_str(), &entrystat)) {
          eos_static_err("cannot stat file %s", sentry.c_str());
        } else {
          if (!S_ISREG(entrystat.st_mode)) {
            eos_static_info("skipping %s [not a file]", sentry.c_str());
          } else {
            if (!forwardFile(sentry, dstfile)) {
              eos_static_err("cannot sync file %s => %s", sentry.c_str(),dsturl.c_str()); 
            }
          }
        }
      }
      closedir(dir);
    }

    memcpy(&laststat, &presentstat, sizeof(struct stat));
    usleep(10000000);
  } while (1);
}

  
