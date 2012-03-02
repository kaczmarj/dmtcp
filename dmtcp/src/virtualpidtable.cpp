/****************************************************************************
 *   Copyright (C) 2006-2010 by Jason Ansel, Kapil Arya, and Gene Cooperman *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu              *
 *                                                                          *
 *   This file is part of the dmtcp/src module of DMTCP (DMTCP:dmtcp/src).  *
 *                                                                          *
 *  DMTCP:dmtcp/src is free software: you can redistribute it and/or        *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP:dmtcp/src is distributed in the hope that it will be useful,      *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License along with DMTCP:dmtcp/src.  If not, see                        *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/

#include "virtualpidtable.h"
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sstream>
#include <fcntl.h>
#include <sys/syscall.h>
#include "constants.h"
#include "util.h"
#include "syscallwrappers.h"
#include "protectedfds.h"
#include  "../jalib/jconvert.h"
#include  "../jalib/jfilesystem.h"

#ifdef PID_VIRTUALIZATION

static pthread_mutex_t tblLock = PTHREAD_MUTEX_INITIALIZER;

static void _do_lock_tbl()
{
  JASSERT(_real_pthread_mutex_lock(&tblLock) == 0) (JASSERT_ERRNO);
}

static void _do_unlock_tbl()
{
  JASSERT(_real_pthread_mutex_unlock(&tblLock) == 0) (JASSERT_ERRNO);
}

dmtcp::VirtualPidTable::VirtualPidTable()
{
  _do_lock_tbl();
  _pid = _real_getpid();
  _ppid = _real_getppid();
  _pidMapTable.clear();
  _pidMapTable[_pid] = _pid;
  _pidMapTable[_ppid] = _ppid;
  _do_unlock_tbl();
}

static dmtcp::VirtualPidTable *inst = NULL;
dmtcp::VirtualPidTable& dmtcp::VirtualPidTable::instance()
{
  if (inst == NULL) {
    inst = new VirtualPidTable();
  }
  return *inst;
//  static VirtualPidTable *inst = new VirtualPidTable(); return *inst;
}

bool dmtcp::VirtualPidTable::isConflictingPid( pid_t pid)
{
  /*  If pid != virtualToReal(pid), then there is a conflict because
   *    there is an virtualPid same as this pid.
   *
   *  If pid == virtualToReal(pid), then there are two cases:
   *    1. there is no mapping from some virtualPid to pid ==> no conflict
   *    2. there is a mapping from pid to pid in the table, in which case again
   *       there is not conflict because that mapping essentially is about the
   *       real pid.
   */
#if 0
  // Use a similar block to emulate tid-conflict. This can come handy in
  // finding bugs related to tid-conflict.
  if (pid >= 10000 && pid < 20000) {
    return true;
  }
#endif
  if (pid == instance().virtualToReal( pid ))
    return false;

  return true;
}

void dmtcp::VirtualPidTable::preCheckpoint()
{
  _ppid = getppid(); // refresh parent PID
}

void dmtcp::VirtualPidTable::postRestart()
{
  /*
   * PROTECTED_PIDMAP_FD corresponds to the file containg computation wide
   *  virtualPid -> realPid map to avoid pid/tid collisions.
   */
}

void dmtcp::VirtualPidTable::printPidMaps()
{
  ostringstream out;
  out << "Pid mappings\n";
  out << "      Virtual" << "  ->  " << "Real" << "\n";
  for ( pid_iterator i = _pidMapTable.begin(); i != _pidMapTable.end(); ++i ) {
    pid_t virtualPid = i->first;
    pid_t realPid  = i->second;
    out << "\t" << virtualPid << "\t->   " << realPid << "\n";
  }
  JTRACE("Virtual To Real Pid Mappings:") (_pidMapTable.size()) (out.str());
}

void dmtcp::VirtualPidTable::resetOnFork()
{
  pthread_mutex_t newlock = PTHREAD_MUTEX_INITIALIZER;
  tblLock = newlock;
  _pid = _real_getpid();
  _ppid = realToVirtual ( _real_getppid() );
  printPidMaps();
}

pid_t dmtcp::VirtualPidTable::virtualToReal(pid_t virtualPid)
{
  pid_t retVal = 0;

  if (virtualPid == -1 || virtualPid == 0) {
    return virtualPid;
  }

  /* This code is called from MTCP while the checkpoint thread is holding
     the JASSERT log lock. Therefore, don't call JTRACE/JASSERT/JINFO/etc. in
     this function. */
  _do_lock_tbl();
  pid_iterator i = _pidMapTable.find(virtualPid < -1 ? abs(virtualPid)
                                                     : virtualPid);
  if (i == _pidMapTable.end()) {
//    JWARNING(false) (virtualPid)
 //     .Text("No real pid/tid found for the given virtual pid");
//    sleep(15);
    _do_unlock_tbl();
    return virtualPid;
  }

  // retVal required: in TID conflict, first and second clone call can interfere
  retVal = virtualPid < -1 ? (-i->second) : i->second;
  _do_unlock_tbl();
  return retVal;
}

extern int __attribute__ ((weak)) mtcp_is_ptracing();
pid_t dmtcp::VirtualPidTable::realToVirtual(pid_t realPid)
{
  if (realPid == -1 || realPid == 0) {
    return realPid;
  }

  /* This code is called from MTCP while the checkpoint thread is holding
     the JASSERT log lock. Therefore, don't call JTRACE/JASSERT/JINFO/etc. in
     this function. */
  _do_lock_tbl();
  for (pid_iterator i = _pidMapTable.begin(); i != _pidMapTable.end(); ++i) {
    if ( realPid == i->second ) {
      _do_unlock_tbl();
      return i->first;
    }
  }

  _do_unlock_tbl();
  return realPid;
}

void dmtcp::VirtualPidTable::insert(pid_t virtualPid)
{
  // FIXME: Add a check for preexisting virtualPid -> curPid mapping
  updateMapping(virtualPid, virtualPid);
}

void dmtcp::VirtualPidTable::erase( pid_t virtualPid )
{
  _do_lock_tbl();
  _pidMapTable.erase( virtualPid );
  _do_unlock_tbl();
}

void dmtcp::VirtualPidTable::updateMapping( pid_t virtualPid, pid_t realPid )
{
  _do_lock_tbl();
  _pidMapTable[virtualPid] = realPid;
  _do_unlock_tbl();
}

dmtcp::vector< pid_t > dmtcp::VirtualPidTable::getPidVector( )
{
  dmtcp::vector< pid_t > pidVec;
  for ( pid_iterator i = _pidMapTable.begin(); i != _pidMapTable.end(); ++i )
    pidVec.push_back ( i->first );
  return pidVec;
}

bool dmtcp::VirtualPidTable::pidExists( pid_t pid )
{
  bool retVal = false;
  _do_lock_tbl();
  pid_iterator j = _pidMapTable.find ( pid );
  if ( j != _pidMapTable.end() )
    retVal = true;

  _do_unlock_tbl();
  return retVal;
}

void dmtcp::VirtualPidTable::serialize ( jalib::JBinarySerializer& o )
{
  JSERIALIZE_ASSERT_POINT ( "dmtcp::VirtualPidTable:" );

  serializePidMap     ( o );

  JSERIALIZE_ASSERT_POINT( "EOF" );
}


void dmtcp::VirtualPidTable::serializePidMap ( jalib::JBinarySerializer& o )
{
  size_t numMaps = _pidMapTable.size();
  serializeEntryCount(o, numMaps);

  pid_t virtualPid;
  pid_t realPid;

  if ( o.isWriter() )
  {
    for ( pid_iterator i = _pidMapTable.begin(); i != _pidMapTable.end(); ++i ) {
      virtualPid = i->first;
      realPid  = i->second;
      serializePidMapEntry ( o, virtualPid, realPid );
    }
  }
  else
  {
    for (size_t i = 0; i < numMaps; i++) {
      serializePidMapEntry ( o, virtualPid, realPid );
      _pidMapTable[virtualPid] = realPid;
    }
  }

  JTRACE ("Serializing PidMap Table") (numMaps) (o.filename());
  printPidMaps();
}

void dmtcp::VirtualPidTable::serializePidMapEntry(jalib::JBinarySerializer& o,
                                                  pid_t& virtualPid,
                                                  pid_t& realPid)
{
  JSERIALIZE_ASSERT_POINT("PidMap:[");
  o & virtualPid;
  JSERIALIZE_ASSERT_POINT(":");
  o & realPid;
  JSERIALIZE_ASSERT_POINT("]");

  JTRACE("PidMap") (virtualPid) (realPid);
}

void dmtcp::VirtualPidTable::serializeEntryCount ( jalib::JBinarySerializer& o,
                                                   size_t& count )
{
  JSERIALIZE_ASSERT_POINT ( "NumEntries:[" );
  o & count;
  JSERIALIZE_ASSERT_POINT ( "]" );
  JTRACE("Num PidMaps:")(count);
}

void dmtcp::VirtualPidTable::InsertIntoPidMapFile(pid_t virtualPid,
                                                  pid_t realPid)
{
  dmtcp::string pidMapFile = "/proc/self/fd/"
                             + jalib::XToString ( PROTECTED_PIDMAP_FD );
  dmtcp::string pidMapCountFile = "/proc/self/fd/"
                                  + jalib::XToString ( PROTECTED_PIDMAPCNT_FD );

  pidMapFile =  jalib::Filesystem::ResolveSymlink ( pidMapFile );
  pidMapCountFile =  jalib::Filesystem::ResolveSymlink ( pidMapCountFile );
  JASSERT ( pidMapFile.length() > 0 && pidMapCountFile.length() > 0 )
    ( pidMapFile )( pidMapCountFile ).Text("Failed to resolve symlink.");

  // Create Serializers
  jalib::JBinarySerializeWriterRaw mapwr( pidMapFile, PROTECTED_PIDMAP_FD );
  jalib::JBinarySerializeWriterRaw countwr(pidMapCountFile, PROTECTED_PIDMAPCNT_FD );
  jalib::JBinarySerializeReaderRaw countrd(pidMapCountFile, PROTECTED_PIDMAPCNT_FD );

  // Lock fileset before any operations
  Util::lockFile(PROTECTED_PIDMAP_FD);
  _do_lock_tbl();

  // Read old number of saved pid maps
  countrd.rewind();
  size_t numMaps;
  serializeEntryCount (countrd,numMaps);
  // Serialize new pair
  serializePidMapEntry (mapwr, virtualPid, realPid );

  // Commit changes into map count file
  countwr.rewind();
  numMaps++;
  serializeEntryCount (countwr,numMaps);

  _do_unlock_tbl();
  Util::unlockFile(PROTECTED_PIDMAP_FD);
}

void dmtcp::VirtualPidTable::readPidMapsFromFile()
{
  dmtcp::string pidMapFile = "/proc/self/fd/"
                             + jalib::XToString ( PROTECTED_PIDMAP_FD );
  pidMapFile =  jalib::Filesystem::ResolveSymlink ( pidMapFile );
  dmtcp::string pidMapCountFile = "/proc/self/fd/"
                                  + jalib::XToString ( PROTECTED_PIDMAPCNT_FD );
  pidMapCountFile =  jalib::Filesystem::ResolveSymlink ( pidMapCountFile );
  JASSERT ( pidMapFile.length() > 0 && pidMapCountFile.length() > 0 )
    ( pidMapFile )( pidMapCountFile );

  JTRACE ( "Read PidMaps from file" ) ( pidMapCountFile ) ( pidMapFile );

  _real_close( PROTECTED_PIDMAP_FD );
  _real_close( PROTECTED_PIDMAPCNT_FD );

  jalib::JBinarySerializeReader maprd( pidMapFile);
  jalib::JBinarySerializeReader countrd(pidMapCountFile);

  // Read number of PID mappings
  size_t numMaps;
  serializeEntryCount (countrd,numMaps);

  // Read pidMapping content
  pid_t virtualPid;
  pid_t realPid;
  while ( numMaps-- > 0 ){
    serializePidMapEntry ( maprd, virtualPid, realPid );
    _pidMapTable[virtualPid] = realPid;
  }
  printPidMaps();
}

#endif
