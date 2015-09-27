/*
 * @file ParallelProcess.cpp
 * Copyright 2015 René J.V. Bertin
 * This code is made available under the CPOL License
 * http://www.codeproject.com/info/cpol10.aspx
 */

#include "ParallelProcess_p.h"
#include "ParallelProcess.h"

#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/mach_port.h>

// ================================= FileEntry methods =================================

FileEntry::FileEntry()
{
	folderInfo = NULL;
	freeFolderInfo = false;
}

FileEntry::FileEntry( const char *name, const struct stat *finfo, FolderInfo *dinfo, const bool ownInfo )
{
	fileName = name;
	fileInfo = *finfo;
	folderInfo = (ownInfo)? new FolderInfo(dinfo) : dinfo;
	freeFolderInfo = ownInfo;
}

FileEntry::FileEntry( const char *name, const struct stat *finfo, FolderInfo &dinfo )
{
	FileEntry( name, finfo, new FolderInfo(dinfo), true );
}

FileEntry::FileEntry(const FileEntry &ref)
{
	*this = ref;
}

FileEntry::~FileEntry()
{
	if( freeFolderInfo && folderInfo ){
//		   fprintf( stderr, "~FileEntry(%p): freeing \"%s\" with folderInfo %p\n", this, fileName.c_str(), folderInfo );
		if( folderInfo->filetypeslist != NULL ){
			free(folderInfo->filetypeslist);
		}
		delete folderInfo;
		folderInfo = NULL;
	}
}

FileEntry &FileEntry::operator =(const FileEntry &ref)
{
	if( this == &ref ){
		return *this;
	}
	fileName = ref.fileName;
	fileInfo = ref.fileInfo;
	if( ref.freeFolderInfo ){
		folderInfo = new FolderInfo(ref.folderInfo);
//		   fprintf( stderr, "FileEntry(FileEntry %p): duplicated folderInfo %p -> %p\n", &ref, ref.folderInfo, folderInfo );
	}
	else{
		folderInfo = ref.folderInfo;
	}
	freeFolderInfo = ref.freeFolderInfo;
	return *this;
}

void FileEntry::compress(FileProcessor *worker, ParallelFileProcessor *PP)
{
	if( PP->verbose() > 2){
		fprintf( stderr, "[%d] %s", worker->processorID(), fileName.c_str() ); fflush(stderr);
	}
	compressFile( fileName.c_str(), &fileInfo, folderInfo, worker );
	if( PP->verbose() > 2){
		fputs( " .", stderr ); fflush(stderr);
	}
	if( PP->verbose() ){
		compressedSize = (PP)? process_file( fileName.c_str(), NULL, &fileInfo, &PP->jobInfo ) : 0;
		if( PP->verbose() > 2){
			fputs( " .\n", stderr ); fflush(stderr);
		}
	}
}

// ================================= ParallelFileProcessor methods =================================

ParallelFileProcessor::ParallelFileProcessor(const int n, const int verbose)
{
	threadPool.clear();
	nJobs = n;
	nProcessed = 0;
	allDoneEvent = NULL;
	ioLock = new CRITSECTLOCK(4000);
	ioLockedFlag = false;
	ioLockingThread = 0;
	verboseLevel = verbose;
	memset( &jobInfo, 0, sizeof(jobInfo) );
}

ParallelFileProcessor *createParallelProcessor(const int n, const int verboseLevel)
{
	return new ParallelFileProcessor(n, verboseLevel);
}

// attempt to lock the ioLock; returns a success value
bool ParallelFileProcessor::lockIO()
{
	if( ioLock ){
		ioLock->Lock(ioLockedFlag);
//		   fprintf( stderr, "lockIO() returning %d\n", ioLockedFlag );
	}
	if( ioLockedFlag ){
		ioLockingThread = GetCurrentThreadId();
	}
	return ioLockedFlag;
}

// unlock the ioLock
bool ParallelFileProcessor::unLockIO()
{
	if( ioLock ){
		ioLock->Unlock(ioLockedFlag);
		ioLockedFlag = ioLock->IsLocked();
//		   fprintf( stderr, "unLockIO() returning %d\n", ioLockedFlag );
	}
	if( !ioLockedFlag && ioLockingThread == GetCurrentThreadId() ){
		ioLockingThread = 0;
	}
	return ioLockedFlag;
}

int ParallelFileProcessor::run()
{ FileEntry entry;
  int i, nRequested = nJobs;
  double N = size(), prevPerc = 0;
	if( nJobs >= 1 ){
		allDoneEvent = CreateEvent( NULL, false, false, NULL );
	}
	for( i = 0 ; i < nJobs ; ++i ){
	 FileProcessor *thread = new FileProcessor(this, i);
		if( thread ){
			threadPool.push_back(thread);
		}
		else{
			nJobs -= 1;
		}
	}
	if( nJobs != nRequested ){
		fprintf( stderr, "Parallel processing with %ld instead of %d threads\n", nJobs, nRequested );
	}
	for( i = 0 ; i < nJobs ; ++i ){
		threadPool[i]->Start();
	}
	if( allDoneEvent ){
	 DWORD waitResult = ~WAIT_OBJECT_0;
		while( nJobs >= 1 && !quitRequested() && size() > 0 && waitResult != WAIT_OBJECT_0 ){
			waitResult = WaitForSingleObject( allDoneEvent, 2000 );
			if( nJobs ){
			 double perc = 100.0 * nProcessed / N;
				 if( perc >= prevPerc + 10 ){
					 fprintf( stderr, "%s %d%%", (prevPerc > 0)? " .." : "", int(perc + 0.5) );
					 if( verboseLevel > 1 ){
					   double cpuUsage = 0;
						for( i = 0 ; i < nJobs ; ++i ){
							if( threadPool[i]->nProcessed ){
								cpuUsage += threadPool[i]->cpuUsage / threadPool[i]->nProcessed;
							}
						}
						fprintf( stderr, " [%0.2lf%%]", cpuUsage );
					 }
					 fflush(stderr);
					 prevPerc = perc;
				 }
			}
			if( quitRequested() && !threadPool.empty() ){
				// the WaitForSingleObject() call above was interrupted by the signal that
				// led to quitRequested() being set and as a result the workers haven't yet
				// had the chance to exit cleanly. Give them that chance now.
				waitResult = WaitForSingleObject( allDoneEvent, 2000 );
			}
		}
		fputc( '\n', stderr );
		CloseHandle(allDoneEvent);
		allDoneEvent = NULL;
	}
	i = 0;
	while( !threadPool.empty() ){
	 FileProcessor *thread = threadPool.front();
		if( thread->GetExitCode() == (THREAD_RETURN)STILL_ACTIVE ){
			fprintf( stderr, "Stopping worker thread #%d that is still active!\n", i );
			thread->Stop(true);
		}
		if( thread->nProcessed ){
			fprintf( stderr, "Worker thread #%d processed %ld files",
					i, thread->nProcessed );
			if( verboseLevel ){
				fprintf( stderr, ", %0.2lf Kb [%0.2lf Kb] -> %0.2lf Kb [%0.2lf Kb] (%0.2lf%%)",
					thread->runningTotalRaw/1024.0, thread->runningTotalRaw/1024.0/thread->nProcessed,
					thread->runningTotalCompressed/1024.0, thread->runningTotalCompressed/1024.0/thread->nProcessed,
					100.0 * double(thread->runningTotalRaw - thread->runningTotalCompressed) / double(thread->runningTotalRaw) );
				if( verboseLevel > 1 ){
					fputc( '\n', stderr );
					mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
					thread_basic_info_data_t info;
					int kr = thread_info( mach_thread_self(), THREAD_BASIC_INFO, (thread_info_t) &info, &count);
					if( kr == KERN_SUCCESS ){
						fprintf( stderr, "\t%gs user + %gs system; %ds slept",
							info.user_time.seconds + info.user_time.microseconds * 1e-6,
							info.system_time.seconds + info.system_time.microseconds * 1e-6,
							info.sleep_time );
						if( thread->cpuUsage ){
							fprintf( stderr, "; %0.2lf%% CPU", thread->cpuUsage / thread->nProcessed );
						}
					}
				}
			}
			fputc( '\n', stderr );
		}
		delete thread;
		threadPool.pop_front();
		i++;
	}
	return nProcessed;
}

int ParallelFileProcessor::workerDone(FileProcessor *worker)
{ CRITSECTLOCK::Scope scope(threadLock);
  char name[17];
	nJobs -= 1;
	pthread_getname_np( (pthread_t) GetThreadId(worker->GetThread()), name, sizeof(name) );
//	   fprintf( stderr, "workerDone(): worker \"%s\" is done\n", name );
	if( nJobs <= 0 ){
		if( allDoneEvent ){
			SetEvent(allDoneEvent);
		}
	}
	return nJobs;
}

// ================================= FileProcessor methods =================================

DWORD FileProcessor::Run(LPVOID arg)
{
	if( PP ){
	 FileEntry entry;
		nProcessed = 0;
		while( !PP->quitRequested() && PP->getFront(entry) ){
		 // create a scoped lock without closing it immediately
		 CRITSECTLOCK::Scope scp(PP->ioLock, 0);
			scope = &scp;
			entry.compress( this, PP );
			_InterlockedIncrement(&PP->nProcessed);
			runningTotalRaw += entry.fileInfo.st_size;
			runningTotalCompressed += (entry.compressedSize > 0)? entry.compressedSize : entry.fileInfo.st_size;
			if( PP->verbose() > 1 ){
				mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
				thread_basic_info_data_t info;
				int kr = thread_info( mach_thread_self(), THREAD_BASIC_INFO, (thread_info_t) &info, &count);
				if( kr == KERN_SUCCESS ){
					 cpuUsage += info.cpu_usage/10.0;
				}
			}
			nProcessed += 1;
			scope = NULL;
		}
	}
	return DWORD(nProcessed);
}

void FileProcessor::InitThread()
{ // pthread_t thread = (pthread_t) GetThreadId(GetThread());
  char name[16];
//	extern int pthread_setname_np(const char *);
	snprintf( name, 16, "FilePr #%d", procID );
	pthread_setname_np(name);
}

inline bool FileProcessor::lockScope()
{
	if( scope ){
		PP->ioLockedFlag = scope->Lock();
	}
	return PP->ioLockedFlag;
}

inline bool FileProcessor::unLockScope()
{
	if( scope ){
		scope->Unlock();
		PP->ioLockedFlag = *scope;
	}
	return PP->ioLockedFlag;
}

// ================================= C interface functions =================================

void releaseParallelProcessor(ParallelFileProcessor *p)
{
	delete p;
}

bool addFileToParallelProcessor(ParallelFileProcessor *p, const char *inFile, const struct stat *inFileInfo,
								struct folder_info *folderInfo, const bool ownInfo)
{
	if( p && inFile && inFileInfo && folderInfo ){
		if( ownInfo ){
			p->items().push(FileEntry( inFile, inFileInfo, new FolderInfo(*folderInfo), ownInfo ));
		}
		else{
			p->items().push(FileEntry( inFile, inFileInfo, folderInfo, ownInfo ));
		}
		return true;
	}
	else{
//		   fprintf( stderr, "Error: Processor=%p file=%p, finfo=%p dinfo=%p, own=%d\n", p, inFile, inFileInfo, folderInfo, ownInfo );
		return false;
	}
}

size_t filesInParallelProcessor(ParallelFileProcessor *p)
{
	if( p ){
		return p->itemCount();
	}
	else{
		return 0;
	}
}

// attempt to lock the ioLock; returns a success value
bool lockParallelProcessorIO(FileProcessor *worker)
{ bool locked = false;
	if( worker ){
		locked = worker->lockScope();
	}
	return locked;
}

// unlock the ioLock
bool unLockParallelProcessorIO(FileProcessor *worker)
{ bool locked = false;
	if( worker ){
		locked = worker->unLockScope();
	}
	return locked;
}

int runParallelProcessor(ParallelFileProcessor *p)
{ int ret = -1;
	if( p ){
		ret = p->run();
	}
	return ret;
}

void stopParallelProcessor(ParallelFileProcessor *p)
{
	if( p ){
		p->setQuitRequested(true);
	}
}

struct folder_info *getParallelProcessorJobInfo(ParallelFileProcessor *p)
{
	return (p)? &p->jobInfo : NULL;
}
