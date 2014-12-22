/*
 ============================================================================
 Name        : OMXsonien.c
 Author      : SonienTaegi ( https://github.com/SonienTaegi/rpi-omx-tutorial )
 Version     :
 Copyright   : GPLv2
 Description : OMXsonien is helper for OMX and make developer much easier to
               handle buffer and error cases.
 ============================================================================
 */
#include "OMXsonien.h"
#define BUFFER_MANAGER_MAX	8

OMXsonien_BUFFERMANAGER* bufferManagerRefs[256];
void (*OMXsonienErrorCallback)(OMX_ERRORTYPE);

void OMXsonienErrorCallbackDefault(OMX_ERRORTYPE err) {
	printf("OMX > ERROR [0x%08x]\n", err);
}

void OMXsonienInit() {
	memset(bufferManagerRefs, 0x00, sizeof(OMXsonien_BUFFERMANAGER*) * BUFFER_MANAGER_MAX);
	OMXsonienSetErrorCallback(OMXsonienErrorCallbackDefault);
}

void OMXsonienDeinit() {
	for(int i = 0; i < BUFFER_MANAGER_MAX; i++) {
		if(bufferManagerRefs[i] != NULL) {
			free(bufferManagerRefs[i]);
			bufferManagerRefs[i] = NULL;
		}
	}
}

void OMXsonienSetErrorCallback(void (*callback)(OMX_ERRORTYPE)) {
	OMXsonienErrorCallback = callback;
}

OMX_ERRORTYPE OMXsonienCheckError(OMX_ERRORTYPE err) {
	if(err != OMX_ErrorNone) {
		OMXsonienErrorCallback(err);
	}

	return err;
}

OMXsonien_BUFFERMANAGER* OMXsonienAllocateBuffer(
		OMX_IN OMX_HANDLETYPE hComponent,
        OMX_IN OMX_U32 nPortIndex,
        OMX_IN OMX_PTR pAppPrivate,
		OMX_IN OMX_U32 nSize,
        OMX_IN OMX_U32 nCount) {

	int refID	= -1;
	for(int i = 0; i < BUFFER_MANAGER_MAX; i++) {
		if(bufferManagerRefs[i] != NULL) {
			continue;
		}
		else {
			refID = i;
			break;
		}
	}
	bufferManagerRefs[refID] = malloc(sizeof(OMXsonien_BUFFERMANAGER));
	OMXsonien_BUFFERMANAGER* pBufferManager = bufferManagerRefs[refID];
	pBufferManager->hComponent 		= hComponent;
	pBufferManager->nPortIndex		= nPortIndex;
	pBufferManager->eBufferSetType	= AllocateBuffer;	// Currently the only supported type

	if(!nSize || !nCount) {
		OMX_PARAM_PORTDEFINITIONTYPE 	portDef;
		OMX_INIT_STRUCTURE(portDef);

		portDef.nPortIndex = nPortIndex;
		OMX_GetParameter(hComponent, OMX_IndexParamPortDefinition, &portDef);

		if(!nSize) {
			nSize = portDef.nBufferSize;
		}

		if(!nCount) {
			nCount = portDef.nBufferCountActual;
		}
	}
	printf("0x%08x : Buffer Size = %d / Count = %d\n", hComponent, nSize, nCount);
	pBufferManager->queue = CQcreateInstance(nCount);
	for(int i = 0; i < nCount; i++) {
		OMX_BUFFERHEADERTYPE*	pBufferHeader;
		printf("0x%08x : New Buffer #%d\n", hComponent, i);
		OMXsonienCheckError(OMX_AllocateBuffer(hComponent, &pBufferHeader, nPortIndex, pAppPrivate, nSize));
		printf("0x%08x : At 0x%08x\n", hComponent, pBufferHeader->pBuffer);
		CQput(pBufferManager->queue, pBufferHeader);
	}
	return pBufferManager;
}

void OMXsonienFreeBuffer(
		OMX_IN OMXsonien_BUFFERMANAGER* pManager) {
	OMX_BUFFERHEADERTYPE* pBuffer = CQget(pManager->queue);
	while(pBuffer) {
		OMX_FreeBuffer(pManager->hComponent, pManager->nPortIndex, pBuffer);
		pBuffer = CQget(pManager->queue);
	}
	CQdestroy(pManager->queue);
	pManager->queue = NULL;
}

OMX_BUFFERHEADERTYPE* OMXsonienBufferGet(
		OMX_IN OMXsonien_BUFFERMANAGER* pManager) {

	return CQget(pManager->queue);
}

void OMXsonienBufferPut(
		OMX_IN OMXsonien_BUFFERMANAGER* pManager,
		OMX_BUFFERHEADERTYPE* pBuffer) {
	CQput(pManager->queue, pBuffer);
}

OMX_BUFFERHEADERTYPE* OMXsonienBufferNow(
		OMX_IN OMXsonien_BUFFERMANAGER* pManager) {
	return pManager->queue->head;
}

