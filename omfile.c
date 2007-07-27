/* omfile.c
 * This is the implementation of the build-in file output module.
 *
 * Handles: eTypeCONSOLE, eTypeTTY, eTypeFILE, eTypePIPE
 *
 * NOTE: read comments in module-template.h to understand how this file
 *       works!
 *
 * File begun on 2007-07-21 by RGerhards (extracted from syslogd.c)
 * This file is under development and has not yet arrived at being fully
 * self-contained and a real object. So far, it is mostly an excerpt
 * of the "old" message code without any modifications. However, it
 * helps to have things at the right place one we go to the meat of it.
 *
 * Copyright 2007 Rainer Gerhards and Adiscon GmbH.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/file.h>

#include "rsyslog.h"
#include "syslogd.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "template.h"
#include "outchannel.h"
#include "omfile.h"
#include "module-template.h"

/* internal structures
 */
typedef struct _instanceData {
	char	f_fname[MAXFNAME];/* file or template name (display only) */
	short	fd;		  /* file descriptor for (current) file */
	enum {
		eTypeFILE,
		eTypeTTY,
		eTypeCONSOLE,
		eTypePIPE
	} fileType;	
	struct template *pTpl;	/* pointer to template object */
	char	bDynamicName;	/* 0 - static name, 1 - dynamic name (with properties) */
	int	fCreateMode;	/* file creation mode for open() */
	int	fDirCreateMode;	/* creation mode for mkdir() */
	int	bCreateDirs;	/* auto-create directories? */
	int	bSyncFile;	/* should the file by sync()'ed? 1- yes, 0- no */
	uid_t	fileUID;	/* IDs for creation */
	uid_t	dirUID;
	gid_t	fileGID;
	gid_t	dirGID;
	int	bFailOnChown;	/* fail creation if chown fails? */
	int	iCurrElt;	/* currently active cache element (-1 = none) */
	int	iCurrCacheSize;	/* currently cache size (1-based) */
	int	iDynaFileCacheSize; /* size of file handle cache */
	/* The cache is implemented as an array. An empty element is indicated
	 * by a NULL pointer. Memory is allocated as needed. The following
	 * pointer points to the overall structure.
	 */
	dynaFileCacheEntry **dynCache;
	off_t	f_sizeLimit;		/* file size limit, 0 = no limit */
	char	*f_sizeLimitCmd;	/* command to carry out when size limit is reached */
} instanceData;


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURERepeatedMsgReduction)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
	if(pData->bDynamicName) {
		printf("[dynamic]\n\ttemplate='%s'\n"
		       "\tfile cache size=%d\n"
		       "\tcreate directories: %s\n"
		       "\tfile owner %d, group %d\n"
		       "\tdirectory owner %d, group %d\n"
		       "\tfail if owner/group can not be set: %s\n",
			pData->f_fname,
			pData->iDynaFileCacheSize,
			pData->bCreateDirs ? "yes" : "no",
			pData->fileUID, pData->fileGID,
			pData->dirUID, pData->dirGID,
			pData->bFailOnChown ? "yes" : "no"
			);
	} else { /* regular file */
		printf("%s", pData->f_fname);
		if (pData->fd == -1)
			printf(" (unused)");
	}
ENDdbgPrintInstInfo


/* Helper to cfline(). Parses a output channel name up until the first
 * comma and then looks for the template specifier. Tries
 * to find that template. Maps the output channel to the 
 * proper filed structure settings. Everything is stored in the
 * filed struct. Over time, the dependency on filed might be
 * removed.
 * rgerhards 2005-06-21
 */
static rsRetVal cflineParseOutchannel(selector_t *f, instanceData *pData, uchar* p)
{
	rsRetVal iRet = RS_RET_OK;
	size_t i;
	struct outchannel *pOch;
	char szBuf[128];	/* should be more than sufficient */

	/* this must always be a file, because we can not set a size limit
	 * on a pipe...
	 * rgerhards 2005-06-21: later, this will be a separate type, but let's
	 * emulate things for the time being. When everything runs, we can
	 * extend it...
	 */
	pData->fileType = eTypeFILE;

	++p; /* skip '$' */
	i = 0;
	/* get outchannel name */
	while(*p && *p != ';' && *p != ' ' &&
	      i < sizeof(szBuf) / sizeof(char)) {
	      szBuf[i++] = *p++;
	}
	szBuf[i] = '\0';

	/* got the name, now look up the channel... */
	pOch = ochFind(szBuf, i);

	if(pOch == NULL) {
		char errMsg[128];
		errno = 0;
		snprintf(errMsg, sizeof(errMsg)/sizeof(char),
			 "outchannel '%s' not found - ignoring action line",
			 szBuf);
		logerror(errMsg);
		return RS_RET_NOT_FOUND;
	}

	/* check if there is a file name in the outchannel... */
	if(pOch->pszFileTemplate == NULL) {
		char errMsg[128];
		errno = 0;
		snprintf(errMsg, sizeof(errMsg)/sizeof(char),
			 "outchannel '%s' has no file name template - ignoring action line",
			 szBuf);
		logerror(errMsg);
		return RS_RET_ERR;
	}

	/* OK, we finally got a correct template. So let's use it... */
	strncpy(pData->f_fname, pOch->pszFileTemplate, MAXFNAME);
	pData->f_sizeLimit = pOch->uSizeLimit;
	/* WARNING: It is dangerous "just" to pass the pointer. As we
	 * never rebuild the output channel description, this is acceptable here.
	 */
	pData->f_sizeLimitCmd = pOch->cmdOnSizeLimit;

	/* back to the input string - now let's look for the template to use
	 * Just as a general precaution, we skip whitespace.
	 */
	while(*p && isspace((int) *p))
		++p;
	if(*p == ';')
		++p; /* eat it */

	if((iRet = cflineParseTemplateName(&p, szBuf,
	                        sizeof(szBuf) / sizeof(char))) == RS_RET_OK) {
		if(szBuf[0] == '\0')	/* no template? */
			strcpy(szBuf, " TradFmt"); /* use default! */

		iRet = cflineSetTemplateAndIOV(f, szBuf);
		
		dprintf("[outchannel]filename: '%s', template: '%s', size: %lu\n", pData->f_fname, szBuf,
			pData->f_sizeLimit);
	}

	return(iRet);
}


/* rgerhards 2005-06-21: Try to resolve a size limit
 * situation. This first runs the command, and then
 * checks if we are still above the treshold.
 * returns 0 if ok, 1 otherwise
 * TODO: consider moving the initial check in here, too
 */
int resolveFileSizeLimit(instanceData *pData)
{
	uchar *pParams;
	uchar *pCmd;
	uchar *p;
	off_t actualFileSize;
	assert(pData != NULL);

	if(pData->f_sizeLimitCmd == NULL)
		return 1; /* nothing we can do in this case... */
	
	/* the execProg() below is probably not great, but at least is is
	 * fairly secure now. Once we change the way file size limits are
	 * handled, we should also revisit how this command is run (and
	 * with which parameters).   rgerhards, 2007-07-20
	 */
	/* we first check if we have command line parameters. We assume this, 
	 * when we have a space in the program name. If we find it, everything after
	 * the space is treated as a single argument.
	 */
	if((pCmd = (uchar*)strdup((char*)pData->f_sizeLimitCmd)) == NULL) {
		/* there is not much we can do - we make syslogd close the file in this case */
		glblHadMemShortage = 1;
		return 1;
		}

	for(p = pCmd ; *p && *p != ' ' ; ++p) {
		/* JUST SKIP */
	}

	if(*p == ' ') {
		*p = '\0'; /* pretend string-end */
		pParams = p+1;
	} else
		pParams = NULL;

	execProg(pCmd, 1, pParams);

	pData->fd = open(pData->f_fname, O_WRONLY|O_APPEND|O_CREAT|O_NOCTTY,
			pData->fCreateMode);

	actualFileSize = lseek(pData->fd, 0, SEEK_END);
	if(actualFileSize >= pData->f_sizeLimit) {
		/* OK, it didn't work out... */
		return 1;
		}

	return 0;
}


/* This function deletes an entry from the dynamic file name
 * cache. A pointer to the cache must be passed in as well
 * as the index of the to-be-deleted entry. This index may
 * point to an unallocated entry, in whcih case the
 * function immediately returns. Parameter bFreeEntry is 1
 * if the entry should be free()ed and 0 if not.
 */
static void dynaFileDelCacheEntry(dynaFileCacheEntry **pCache, int iEntry, int bFreeEntry)
{
	assert(pCache != NULL);

	if(pCache[iEntry] == NULL)
		return;

	dprintf("Removed entry %d for file '%s' from dynaCache.\n", iEntry,
		pCache[iEntry]->pName == NULL ? "[OPEN FAILED]" : (char*)pCache[iEntry]->pName);
	/* if the name is NULL, this is an improperly initilized entry which
	 * needs to be discarded. In this case, neither the file is to be closed
	 * not the name to be freed.
	 */
	if(pCache[iEntry]->pName != NULL) {
		close(pCache[iEntry]->fd);
		free(pCache[iEntry]->pName);
		pCache[iEntry]->pName = NULL;
	}

	if(bFreeEntry) {
		free(pCache[iEntry]);
		pCache[iEntry] = NULL;
	}
}


/* This function frees the dynamic file name cache.
 */
static void dynaFileFreeCache(instanceData *pData)
{
	register int i;
	assert(pData != NULL);

	for(i = 0 ; i < pData->iCurrCacheSize ; ++i) {
		dynaFileDelCacheEntry(pData->dynCache, i, 1);
	}

	free(pData->dynCache);
}


/* This function handles dynamic file names. It generates a new one
 * based on the current message, checks if that file is already open
 * and, if not, does everything needed to switch to the new one.
 * Function returns 0 if all went well and non-zero otherwise.
 * On successful return pData->fd must point to the correct file to
 * be written.
 * This is a helper to writeFile(). rgerhards, 2007-07-03
 */
static int prepareDynFile(selector_t *f, instanceData *pData)
{
	uchar *newFileName;
	time_t ttOldest; /* timestamp of oldest element */
	int iOldest;
	int i;
	int iFirstFree;
	dynaFileCacheEntry **pCache;

	assert(f != NULL);
	assert(pData != NULL);
	if((newFileName = tplToString(pData->pTpl, f->f_pMsg)) == NULL) {
		/* memory shortage - there is nothing we can do to resolve it.
		 * We silently ignore it, this is probably the best we can do.
		 */
		glblHadMemShortage = TRUE;
		dprintf("prepareDynfile(): could not create file name, discarding this request\n");
		return -1;
	}

	pCache = pData->dynCache;

	/* first check, if we still have the current file
	 * I *hope* this will be a performance enhancement.
	 */
	if(   (pData->iCurrElt != -1)
	   && !strcmp((char*) newFileName,
	              (char*) pCache[pData->iCurrElt])) {
	   	/* great, we are all set */
		free(newFileName);
		pCache[pData->iCurrElt]->lastUsed = time(NULL); /* update timestamp for LRU */
		return 0;
	}

	/* ok, no luck. Now let's search the table if we find a matching spot.
	 * While doing so, we also prepare for creation of a new one.
	 */
	iFirstFree = -1; /* not yet found */
	iOldest = 0; /* we assume the first element to be the oldest - that will change as we loop */
	ttOldest = time(NULL) + 1; /* there must always be an older one */
	for(i = 0 ; i < pData->iCurrCacheSize ; ++i) {
		if(pCache[i] == NULL) {
			if(iFirstFree == -1)
				iFirstFree = i;
		} else { /* got an element, let's see if it matches */
			if(!strcmp((char*) newFileName, (char*) pCache[i]->pName)) {
				/* we found our element! */
				pData->fd = pCache[i]->fd;
				pData->iCurrElt = i;
				free(newFileName);
				pCache[i]->lastUsed = time(NULL); /* update timestamp for LRU */
				return 0;
			}
			/* did not find it - so lets keep track of the counters for LRU */
			if(pCache[i]->lastUsed < ttOldest) {
				ttOldest = pCache[i]->lastUsed;
				iOldest = i;
				}
		}
	}

	/* we have not found an entry */
	if(iFirstFree == -1 && (pData->iCurrCacheSize < pData->iDynaFileCacheSize)) {
		/* there is space left, so set it to that index */
		iFirstFree = pData->iCurrCacheSize++;
	}

	if(iFirstFree == -1) {
		dynaFileDelCacheEntry(pCache, iOldest, 0);
		iFirstFree = iOldest; /* this one *is* now free ;) */
	} else {
		/* we need to allocate memory for the cache structure */
		pCache[iFirstFree] = (dynaFileCacheEntry*) calloc(1, sizeof(dynaFileCacheEntry));
		if(pCache[iFirstFree] == NULL) {
			glblHadMemShortage = TRUE;
			dprintf("prepareDynfile(): could not alloc mem, discarding this request\n");
			free(newFileName);
			return -1;
		}
	}

	/* Ok, we finally can open the file */
	if(access((char*)newFileName, F_OK) == 0) {
		/* file already exists */
		pData->fd = open((char*) newFileName, O_WRONLY|O_APPEND|O_CREAT|O_NOCTTY,
				pData->fCreateMode);
	} else {
		/* file does not exist, create it (and eventually parent directories */
		if(pData->bCreateDirs) {
			/* we fist need to create parent dirs if they are missing
			 * We do not report any errors here ourselfs but let the code
			 * fall through to error handler below.
			 */
			if(makeFileParentDirs(newFileName, strlen((char*)newFileName),
			     pData->fDirCreateMode, pData->dirUID,
			     pData->dirGID, pData->bFailOnChown) == 0) {
				pData->fd = open((char*) newFileName, O_WRONLY|O_APPEND|O_CREAT|O_NOCTTY,
						pData->fCreateMode);
				if(pData->fd != -1) {
					/* check and set uid/gid */
					if(pData->fileUID != (uid_t)-1 || pData->fileGID != (gid_t) -1) {
						/* we need to set owner/group */
						if(fchown(pData->fd, pData->fileUID,
						          pData->fileGID) != 0) {
							if(pData->bFailOnChown) {
								int eSave = errno;
								close(pData->fd);
								pData->fd = -1;
								errno = eSave;
							}
							/* we will silently ignore the chown() failure
							 * if configured to do so.
							 */
						}
					}
				}
			}
		}
	}

	/* file is either open now or an error state set */
	if(pData->fd == -1) {
		/* do not report anything if the message is an internally-generated
		 * message. Otherwise, we could run into a never-ending loop. The bad
		 * news is that we also lose errors on startup messages, but so it is.
		 */
		if(f->f_pMsg->msgFlags & INTERNAL_MSG)
			dprintf("Could not open dynaFile, discarding message\n");
		else
			logerrorSz("Could not open dynamic file '%s' - discarding message", (char*)newFileName);
		free(newFileName);
		dynaFileDelCacheEntry(pCache, iFirstFree, 1);
		return -1;
	}

	pCache[iFirstFree]->fd = pData->fd;
	pCache[iFirstFree]->pName = newFileName;
	pCache[iFirstFree]->lastUsed = time(NULL);
	pData->iCurrElt = iFirstFree;
	dprintf("Added new entry %d for file cache, file '%s'.\n",
		iFirstFree, newFileName);

	return 0;
}


/* rgerhards 2004-11-11: write to a file output. This
 * will be called for all outputs using file semantics,
 * for example also for pipes.
 */
static rsRetVal writeFile(selector_t *f, uchar *pMsg, instanceData *pData)
{
	off_t actualFileSize;
	rsRetVal iRet = RS_RET_OK;

	assert(f != NULL);
	assert(pData != NULL);

	/* first check if we have a dynamic file name and, if so,
	 * check if it still is ok or a new file needs to be created
	 */
	if(pData->bDynamicName) {
		if(prepareDynFile(f, pData) != 0)
			return RS_RET_ERR;
	}

	/* create the message based on format specified */
again:
	/* check if we have a file size limit and, if so,
	 * obey to it.
	 */
	if(pData->f_sizeLimit != 0) {
		actualFileSize = lseek(pData->fd, 0, SEEK_END);
		if(actualFileSize >= pData->f_sizeLimit) {
			char errMsg[256];
			/* for now, we simply disable a file once it is
			 * beyond the maximum size. This is better than having
			 * us aborted by the OS... rgerhards 2005-06-21
			 */
			(void) close(pData->fd);
			/* try to resolve the situation */
			if(resolveFileSizeLimit(pData) != 0) {
				/* didn't work out, so disable... */
				snprintf(errMsg, sizeof(errMsg),
					 "no longer writing to file %s; grown beyond configured file size of %lld bytes, actual size %lld - configured command did not resolve situation",
					 pData->f_fname, (long long) pData->f_sizeLimit, (long long) actualFileSize);
				errno = 0;
				logerror(errMsg);
				return RS_RET_DISABLE_ACTION;
			} else {
				snprintf(errMsg, sizeof(errMsg),
					 "file %s had grown beyond configured file size of %lld bytes, actual size was %lld - configured command resolved situation",
					 pData->f_fname, (long long) pData->f_sizeLimit, (long long) actualFileSize);
				errno = 0;
				logerror(errMsg);
			}
		}
	}

	if (write(pData->fd, pMsg, strlen((char*)pMsg)) < 0) {
		int e = errno;

		/* If a named pipe is full, just ignore it for now
		   - mrn 24 May 96 */
		if (pData->fileType == eTypePIPE && e == EAGAIN)
			return RS_RET_OK;

		/* If the filesystem is filled up, just ignore
		 * it for now and continue writing when possible
		 * based on patch for sysklogd by Martin Schulze on 2007-05-24
		 */
		if (pData->fileType == eTypeFILE && e == ENOSPC)
			return RS_RET_OK;

		(void) close(pData->fd);
		/*
		 * Check for EBADF on TTY's due to vhangup()
		 * Linux uses EIO instead (mrn 12 May 96)
		 */
		if ((pData->fileType == eTypeTTY || pData->fileType == eTypeCONSOLE)
#ifdef linux
			&& e == EIO) {
#else
			&& e == EBADF) {
#endif
			pData->fd = open(pData->f_fname, O_WRONLY|O_APPEND|O_NOCTTY);
			if (pData->fd < 0) {
				iRet = RS_RET_DISABLE_ACTION;
				logerror(pData->f_fname);
			} else {
				untty();
				goto again;
			}
		} else {
			iRet = RS_RET_DISABLE_ACTION;
			errno = e;
			logerror(pData->f_fname);
		}
	} else if (pData->bSyncFile)
		fsync(pData->fd);
	return(iRet);
}


BEGINcreateInstance
CODESTARTcreateInstance
ENDcreateInstance


BEGINfreeInstance
CODESTARTfreeInstance
	if(pData->bDynamicName) {
		dynaFileFreeCache(pData);
	} else 
		close(pData->fd);
ENDfreeInstance


BEGINonSelectReadyWrite
CODESTARTonSelectReadyWrite
ENDonSelectReadyWrite


BEGINneedUDPSocket
CODESTARTneedUDPSocket
ENDneedUDPSocket


BEGINgetWriteFDForSelect
CODESTARTgetWriteFDForSelect
ENDgetWriteFDForSelect


BEGINdoAction
CODESTARTdoAction
	dprintf(" (%s)\n", pData->f_fname);
	/* pData->fd == -1 is an indicator that the we couldn't
	 * open the file at startup. For dynaFiles, this is ok,
	 * all others are doomed.
	 */
	if(pData->bDynamicName || (pData->fd != -1))
		iRet = writeFile(f, pMsg, pData);
ENDdoAction


BEGINparseSelectorAct
CODESTARTparseSelectorAct
	/* yes, the if below is redundant, but I need it now. Will go away as
	 * the code further changes.  -- rgerhards, 2007-07-25
	 */
	if(*p == '$' || *p == '?' || *p == '|' || *p == '/' || *p == '-') {
		if((iRet = createInstance(&pData)) != RS_RET_OK)
			return iRet;
	} else {
		/* this is not clean, but we need it for the time being
		 * TODO: remove when cleaning up modularization 
		 */
		return RS_RET_CONFLINE_UNPROCESSED;
	}

	if (*p == '-') {
		pData->bSyncFile = 0;
		p++;
	} else
		pData->bSyncFile = 1;

	pData->f_sizeLimit = 0; /* default value, use outchannels to configure! */

	switch (*p)
	{
        case '$':
		/* rgerhards 2005-06-21: this is a special setting for output-channel
		 * definitions. In the long term, this setting will probably replace
		 * anything else, but for the time being we must co-exist with the
		 * traditional mode lines.
		 * rgerhards, 2007-07-24: output-channels will go away. We keep them
		 * for compatibility reasons, but seems to have been a bad idea.
		 */
		if((iRet = cflineParseOutchannel(f, pData, p)) == RS_RET_OK) {
			pData->bDynamicName = 0;
			pData->fCreateMode = fCreateMode; /* preserve current setting */
			pData->fDirCreateMode = fDirCreateMode; /* preserve current setting */
			pData->fd = open(pData->f_fname, O_WRONLY|O_APPEND|O_CREAT|O_NOCTTY,
					 pData->fCreateMode);
		}
		break;

	case '?': /* This is much like a regular file handle, but we need to obtain
		   * a template name. rgerhards, 2007-07-03
		   */
		++p; /* eat '?' */
		if((iRet = cflineParseFileName(f, p, (uchar*) pData->f_fname)) != RS_RET_OK)
			break;
		pData->pTpl = tplFind((char*)pData->f_fname,
					       strlen((char*) pData->f_fname));
		if(pData->pTpl == NULL) {
			logerrorSz("Template '%s' not found - dynaFile deactivated.", pData->f_fname);
			iRet = RS_RET_NOT_FOUND; /* that's it... :( */
			break;
		}

		pData->bDynamicName = 1;
		pData->iCurrElt = -1;		  /* no current element */
		pData->fCreateMode = fCreateMode; /* freeze current setting */
		pData->fDirCreateMode = fDirCreateMode; /* preserve current setting */
		pData->bCreateDirs = bCreateDirs;
		pData->bFailOnChown = bFailOnChown;
		pData->fileUID = fileUID;
		pData->fileGID = fileGID;
		pData->dirUID = dirUID;
		pData->dirGID = dirGID;
		pData->iDynaFileCacheSize = iDynaFileCacheSize; /* freeze current setting */
		/* we now allocate the cache table. We use calloc() intentionally, as we 
		 * need all pointers to be initialized to NULL pointers.
		 */
		if((pData->dynCache = (dynaFileCacheEntry**)
		    calloc(iDynaFileCacheSize, sizeof(dynaFileCacheEntry*))) == NULL) {
			iRet = RS_RET_OUT_OF_MEMORY;
			dprintf("Could not allocate memory for dynaFileCache - selector disabled.\n");
		}
		break;

        case '|':
	case '/':
		/* rgerhards, 2007-0726: first check if file or pipe */
		if(*p == '|') {
			pData->fileType = eTypePIPE;
			++p;
		} else {
			pData->fileType = eTypeFILE;
		}
		/* rgerhards 2004-11-17: from now, we need to have different
		 * processing, because after the first comma, the template name
		 * to use is specified. So we need to scan for the first coma first
		 * and then look at the rest of the line.
		 */
		if((iRet = cflineParseFileName(f, p, (uchar*) pData->f_fname)) != RS_RET_OK)
			break;

		pData->bDynamicName = 0;
		pData->fCreateMode = fCreateMode; /* preserve current setting */
		if(pData->fileType == eTypePIPE) {
			pData->fd = open(pData->f_fname, O_RDWR|O_NONBLOCK);
	        } else {
			pData->fd = open(pData->f_fname, O_WRONLY|O_APPEND|O_CREAT|O_NOCTTY,
					 pData->fCreateMode);
		}
		        
	  	if ( pData->fd < 0 ){
			pData->fd = -1;
			dprintf("Error opening log file: %s\n", pData->f_fname);
			logerror(pData->f_fname);
			break;
		}
		if (isatty(pData->fd)) {
			pData->fileType = eTypeTTY;
			untty();
		}
		if (strcmp((char*) p, ctty) == 0)
			pData->fileType = eTypeCONSOLE;
		break;
	default:
		iRet = RS_RET_CONFLINE_UNPROCESSED;
		break;
	}
ENDparseSelectorAct


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
ENDqueryEtryPt


BEGINmodInit(File)
CODESTARTmodInit
	*ipIFVersProvided = 1; /* so far, we only support the initial definition */
ENDmodInit


/*
 * vi:set ai:
 */