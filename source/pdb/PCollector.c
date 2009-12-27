
#include "PCollector.h"
#include "Log.h"
#include "Datum.h"
#include "Date.h"
#include "Pointer.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>


PCollector *PCollector_new(void)
{
	PCollector *self = calloc(1, sizeof(PCollector));
	self->out = PDB_new();
	PDB_setUseBackups_(self->out, 0);
	PDB_setHardSync_(self->out, 0);
	PDB_setPathCString_(self->out, "/tmp/db.tc"); // hack - change to a uniqueID
		
	self->savedPids = CHash_new();
	CHash_setEqualFunc_(self->savedPids, (CHashEqualFunc *)Pointer_equals_);
	CHash_setHash1Func_(self->savedPids, (CHashHashFunc *)Pointer_hash1);
	CHash_setHash2Func_(self->savedPids, (CHashHashFunc *)Pointer_hash2);
	
	self->saveQueue  = List_new();
	self->markCount = 0;
	
	self->maxStepTime = .1;
	return self;
}

void PCollector_putCallback(void *arg, const char *k, int ksize, const char *v, int vsize)
{
	PCollector *self = arg;
	if(PCollector_shouldUpdateKey_(self, k, ksize))
	{
		PDB_at_put_(self->out, k, ksize, v, vsize);
	}
}

void PCollector_catCallback(void *arg, const char *k, int ksize, const char *v, int vsize)
{
	PCollector *self = arg;
	if(PCollector_shouldUpdateKey_(self, k, ksize))
	{
		PDB_at_cat_(self->out, k, ksize, v, vsize);
	}
}

void PCollector_removeCallback(void *arg, const char *k, int ksize)
{
	PCollector *self = arg;
	if(PCollector_shouldUpdateKey_(self, k, ksize))
	{
		PDB_removeAt_(self->out, k, ksize);
	}
}

void PCollector_setIn_(PCollector *self, void *pdb)
{
	self->in = pdb;
	PDB_setDelegate_(self->in, self);
	PDB_setPutCallback_(self->in, (PDBPutCallback *)PCollector_putCallback);
	PDB_setCatCallback_(self->in, (PDBCatCallback *)PCollector_catCallback);
	PDB_setRemoveCallback_(self->in, (PDBRemoveCallback *)PCollector_removeCallback);
}

void PCollector_free(PCollector *self)
{	
	PDB_free(self->out);
	CHash_free(self->savedPids);
	List_free(self->saveQueue);
	free(self);
}

int PCollector_shouldUpdateKey_(PCollector *self, const char *k, int ksize)
{
	const char *slash = strrchr(k, '/');
	char pidString[128];
	memcpy(pidString, k, slash - k);
	long pid = atol(pidString);
	return PCollector_hasSaved_(self, pid);
}

// --------------------------------------------

void PCollector_begin(PCollector *self)
{	
	if(self->isCollecting) 
	{
		printf("PCollector_beginCollectGarbage: already in collection - ignored\n");
		return;
	}
	
	self->isCollecting = 1;

	// in db already open
	self->inNode = PDB_newNode(self->in);

	PDB_remove(self->out);	
	PDB_open(self->out);	
	self->outNode = PDB_newNode(self->out);
		
	Log_Printf_("PCollector: beginCollectGarbage %iMB before collect\n", (int)PDB_sizeInMB(self->in));
	
	PCollector_addToSaveQueue_(self, 1); // root node
}

void PCollector_step(PCollector *self)
{
	if (!self->isCollecting) return;
	
	double t1 = Date_SecondsFrom1970ToNow();
	int count = 0;
	double dt = 0;
	
	while (dt < self->maxStepTime)
	{
		long pid = (long)List_pop(self->saveQueue);
		PCollector_markPid_(self, pid);
		count ++;
		
		if (List_size(self->saveQueue) == 0)
		{
			PCollector_complete(self);
			break;
		}
		
		dt = Date_SecondsFrom1970ToNow() - t1;
	}
}

long PCollector_complete(PCollector *self)
{
	CHash_clear(self->savedPids);
	
	PNode_free(self->inNode);
	self->inNode = 0x0;
	PDB_close(self->in);

	PNode_free(self->outNode);
	self->outNode = 0x0;
	PDB_close(self->out);
	
	PDB_moveTo_(self->out, self->in);
	PDB_open(self->in);
	
	//File_remove(self->dbFile);
	//File_moveTo_(out->dbFile, self->dbFile);
		
	Log_Printf_("PCollector: completeCollectGarbage %iMB after collect\n", (int)PDB_sizeInMB(self->in));

	self->isCollecting = 0;
	return 0;
}

void PCollector_showStatus(PCollector *self)
{
	Log_Printf___(" collector queued:%i saved:%i savedPids:%0.2fM\n", 
		(int)List_size(self->saveQueue),	
		(int)self->markCount, 
		((float)self->savedPids->size)/1000000.0
	);
}

void PCollector_markNode_(PCollector *self, PNode *node)
{
	Datum *k;
	PNode_first(node);

	while ((k = PNode_key(node)))
	{
		Datum *v = PNode_value(node);
		if (Datum_data(k)[0] != '_')
		{
			long pid = Datum_asLong(v);

			if (pid)
			{
				PCollector_addToSaveQueue_(self, pid);
			}
		}
		
		PNode_next(node);
		Datum_poolFreeRefs();
	}
}

void PCollector_markPid_(PCollector *self, long pid)
{
	PNode_setPidLong_(self->inNode, pid);
	PCollector_markNode_(self, self->inNode);

	self->markCount ++;
	if (self->markCount % 1000 == 0) 
	{ 
		PDB_commit(self->out);
		PCollector_showStatus(self);
	}
}

int PCollector_isCollecting(PCollector *self)
{
	return self->isCollecting;
}

int PCollector_hasSaved_(PCollector *self, long pid)
{
	return CHash_at_(self->savedPids, (void *)pid) != 0x0;
}

void PCollector_addToSaveQueue_(PCollector *self, long pid)
{
	if (!PCollector_hasSaved_(self, pid))
	{
		List_append_(self->saveQueue, (void *)pid);
		CHash_at_put_(self->savedPids, (void *)pid, (void *)0x1);
	}
}
