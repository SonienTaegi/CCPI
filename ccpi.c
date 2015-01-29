/*
 ============================================================================
 Name        : ccpi.c
 Author      : SonienTaegi ( https://github.com/SonienTaegi/rpi-omx-tutorial )
 Version     :
 Copyright   : GPLv2
 Description : This main component for CCPI.
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <bcm_host.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>

#include <IL/OMX_Core.h>
#include <IL/OMX_Component.h>
#include <IL/OMX_Video.h>
#include <IL/OMX_Broadcom.h>

#include <curl/curl.h>

#include "common.h"
#include "CircularQueue.h"
#include "CircularBuffer.h"
#include "OMXsonien.h"
#include "arm_simd.h"

#define CAPTURE_WIDTH      640
#define CAPTURE_HEIGHT     480
#define CAPTURE_FRAMERATES 15

#define	COMPONENT_CAMERA "OMX.broadcom.camera"
#define COMPONENT_RENDER "OMX.broadcom.video_render"
#define COMPONENT_ENCODE "OMX.broadcom.video_encode"
#define FIFO_PATH        "fifo.fifo"
// #define FIFO

typedef struct {
	// General Setting
	unsigned int nWidth;
	unsigned int nHeight;
	unsigned int nFramerate;

	// Handles
	OMX_HANDLETYPE pCamera;
	OMX_HANDLETYPE pRender;
	OMX_HANDLETYPE pEncode;

	// Application validity and monitoring
	OMX_BOOL       isValid;
	unsigned int   nFrameCaptured;

	// Variants for camera
	OMXsonien_BUFFERMANAGER* pManagerCamera;
	unsigned int nSliceSizeY, nSliceSizeU, nSliceSizeV;
	unsigned int nOffsetY, nOffsetU, nOffsetV;
	OMX_BOOL     isCameraReady;

	// Variants for encoder : Output
	OMXsonien_BUFFERMANAGER* pManagerEncOut;
	CB_INSTANCE* pBufferStreamOut;
	pthread_t    thread_stream;
	sem_t        sem_stream;
	FILE*        fp_stream; // Option for FIFO
	int          f_stream;  // Option for FIFO

	// Variants for encoder : Input
	OMXsonien_BUFFERMANAGER* pManagerEncIn;
	CQ_INSTANCE* pQueueEncIn;
	pthread_t    thread_encIn;
	sem_t        sem_encIn;

	// Varuabts for Renderer
	OMXsonien_BUFFERMANAGER* pManagerRender;
	OMX_BUFFERHEADERTYPE*    pCurrentCanvas;
	OMX_U8*                  pCurrentY;
	OMX_U8*                  pCurrentU;
	OMX_U8*                  pCurrentV;
	CQ_INSTANCE*             pQueueCameraBuffer;
	pthread_t                thread_engine;
	sem_t                    sem_engine;
} CONTEXT;

/* Application variant */
CONTEXT mContext;

/* Performance measurer */
struct timespec ts_checkAt;
void check_in() {
	clock_gettime(CLOCK_REALTIME, &ts_checkAt);
}
long check_out() {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long nGap = ( ts.tv_sec - ts_checkAt.tv_sec ) * 1000000000 + ( ts.tv_nsec - ts_checkAt.tv_nsec );
	// printf("Time elapsed %8ld us\n", nGap / 1000 );

	return nGap;
}

/* Event Handler : OMX Event */
OMX_ERRORTYPE onOMXevent (
		OMX_IN OMX_HANDLETYPE hComponent,
		OMX_IN OMX_PTR pAppData,
		OMX_IN OMX_EVENTTYPE eEvent,
		OMX_IN OMX_U32 nData1,
		OMX_IN OMX_U32 nData2,
		OMX_IN OMX_PTR pEventData) {

	print_event(hComponent, eEvent, nData1, nData2);

	switch(eEvent) {
	case OMX_EventParamOrConfigChanged :
		if(nData2 == OMX_IndexParamCameraDeviceNumber) {
			((CONTEXT*)pAppData)->isCameraReady = OMX_TRUE;
			print_log("Camera device is ready.");
		}
		break;
	default :
		break;
	}
	return OMX_ErrorNone;
}

OMX_BOOL isSPS = OMX_TRUE;
/* Callback : Camera-out buffer is filled */
OMX_ERRORTYPE onFillBufferDone (
		OMX_OUT OMX_HANDLETYPE hComponent,
		OMX_OUT OMX_PTR pAppData,
		OMX_OUT OMX_BUFFERHEADERTYPE* pBuffer) {
	if(hComponent == mContext.pCamera) {
		CQput(mContext.pQueueCameraBuffer, pBuffer);
		sem_post(&mContext.sem_engine);
	}
	else if(hComponent == mContext.pEncode) {
		if(isSPS) {
			printf("SPS : %d bytes\n", pBuffer->nFilledLen);
			for(int i = 0; i < pBuffer->nFilledLen; i++) {
				printf("%02x", *(pBuffer->pBuffer + i));
			}
			printf("\n");
			isSPS = OMX_FALSE;
		}
#ifdef FIFO
		fwrite(pBuffer->pBuffer, pBuffer->nFilledLen, 1, mContext.fp_stream);
#endif
		// write(mContext.f_stream, pBuffer->pBuffer, pBuffer->nFilledLen);
		CBput(mContext.pBufferStreamOut, pBuffer->pBuffer, pBuffer->nFilledLen);

//		int fetchSize = CBget(mContext.pBufferStreamOut, pBuffer->pBuffer, 10000);
//		printf("%5d bytes = %5d ", pBuffer->nFilledLen, fetchSize);
//		while(fetchSize) {
//			fetchSize = CBget(mContext.pBufferStreamOut, pBuffer->pBuffer + fetchSize, 10000);
//			// printf("System hacked!! Chase me. From Keith");
//			printf("+ %5d ", fetchSize);
//		}
		OMX_FillThisBuffer(hComponent, pBuffer);
	}
	return OMX_ErrorNone;
}

/* Callback : Render-in buffer is emptied */
OMX_ERRORTYPE onEmptyBufferDone(
		OMX_IN OMX_HANDLETYPE hComponent,
		OMX_IN OMX_PTR pAppData,
		OMX_IN OMX_BUFFERHEADERTYPE* pBuffer) {
	if(hComponent == mContext.pRender) {
		OMXsonienBufferPut(mContext.pManagerRender, pBuffer);
	}
	else if(hComponent == mContext.pEncode) {
		OMXsonienBufferPut(mContext.pManagerEncIn, pBuffer);
	}

	return OMX_ErrorNone;
}

/* Callback : Error detection callback of OMXsonien */
void onOMXsonienError(OMX_ERRORTYPE err) {
	printf("Error : 0x%08x\n", err);
	exit(-1);
}

void onSignal(int signal) {
	mContext.isValid = OMX_FALSE;
}

void terminate() {
	print_log("On terminating...");

	OMX_STATETYPE state;
	OMX_BOOL bWaitForCamera, bWaitForEncode, bWaitForRender;

	// Engine Stop
	if(mContext.thread_engine) {
		sem_post(&mContext.sem_engine);
		pthread_join(mContext.thread_engine, NULL);
		mContext.thread_engine = NULL;
		sem_close(&mContext.sem_engine);
	}

	// Encoder In Pump Stop
	if(mContext.thread_encIn) {
		sem_post(&mContext.sem_encIn);
		pthread_join(mContext.thread_encIn, NULL);
		mContext.thread_encIn = NULL;
		sem_close(&mContext.sem_encIn);
	}

	// Stream Stop
	if(mContext.thread_stream) {
		// 이야호
		mContext.thread_stream = NULL;
	}

	// Execute -> Idle
	bWaitForCamera = bWaitForEncode = bWaitForRender = OMX_FALSE;
	if(isState(mContext.pCamera, OMX_StateExecuting)) {
		OMX_SendCommand(mContext.pCamera, OMX_CommandStateSet, OMX_StateIdle, NULL);
		bWaitForCamera = OMX_TRUE;
	}
	if(isState(mContext.pRender, OMX_StateExecuting)) {
		OMX_SendCommand(mContext.pRender, OMX_CommandStateSet, OMX_StateIdle, NULL);
		bWaitForRender = OMX_TRUE;
	}
	if(isState(mContext.pEncode, OMX_StateExecuting)) {
		OMX_SendCommand(mContext.pEncode, OMX_CommandStateSet, OMX_StateIdle, NULL);
		bWaitForEncode = OMX_TRUE;
	}
	if(bWaitForCamera) wait_for_state_change(OMX_StateIdle, mContext.pCamera, NULL);
	if(bWaitForRender) wait_for_state_change(OMX_StateIdle, mContext.pRender, NULL);
	if(bWaitForEncode) wait_for_state_change(OMX_StateIdle, mContext.pEncode, NULL);

	// Idle -> Loaded
	bWaitForCamera = bWaitForEncode = bWaitForRender = OMX_FALSE;
	if(isState(mContext.pCamera, OMX_StateIdle)) {
		OMX_SendCommand(mContext.pCamera, OMX_CommandStateSet, OMX_StateLoaded, NULL);
		bWaitForCamera = OMX_TRUE;
	}
	if(isState(mContext.pRender, OMX_StateIdle)) {
		OMX_SendCommand(mContext.pRender, OMX_CommandStateSet, OMX_StateLoaded, NULL);
		bWaitForRender = OMX_TRUE;
	}
	if(isState(mContext.pEncode, OMX_StateIdle)) {
		OMX_SendCommand(mContext.pEncode, OMX_CommandStateSet, OMX_StateLoaded, NULL);
		bWaitForEncode = OMX_TRUE;
	}

	// Buffer release
	if(mContext.pQueueCameraBuffer) {
		CQdestroy(mContext.pQueueCameraBuffer);
	}
	if(mContext.pQueueEncIn) {
		CQdestroy(mContext.pQueueEncIn);
	}
	if(mContext.pBufferStreamOut) {
		CBdestroy(mContext.pBufferStreamOut);
	}

	if(mContext.fp_stream) {
		fclose(mContext.fp_stream);
	}
	if(mContext.f_stream) {
		close(mContext.f_stream);
	}

	OMXsonienFreeBuffer(mContext.pManagerCamera);
	OMXsonienFreeBuffer(mContext.pManagerRender);
	OMXsonienFreeBuffer(mContext.pManagerEncIn);
	OMXsonienFreeBuffer(mContext.pManagerEncOut);

	if(bWaitForCamera) wait_for_state_change(OMX_StateLoaded, mContext.pCamera, NULL);
	if(bWaitForRender) wait_for_state_change(OMX_StateLoaded, mContext.pRender, NULL);
	if(bWaitForEncode) wait_for_state_change(OMX_StateLoaded, mContext.pEncode, NULL);

	// Loaded -> Free
	if(isState(mContext.pCamera, OMX_StateLoaded)) OMX_FreeHandle(mContext.pCamera);
	if(isState(mContext.pRender, OMX_StateLoaded)) OMX_FreeHandle(mContext.pRender);
	if(isState(mContext.pEncode, OMX_StateLoaded)) OMX_FreeHandle(mContext.pEncode);

	OMXsonienDeinit();
	OMX_Deinit();

	print_log("Press enter to terminate.");
	getchar();
}

void componentLoad(OMX_CALLBACKTYPE* pCallbackOMX) {
	OMX_ERRORTYPE err;

	// Loading component
	print_log("Load %s", COMPONENT_CAMERA);
	OMXsonienCheckError(OMX_GetHandle(&mContext.pCamera, COMPONENT_CAMERA, &mContext, pCallbackOMX));
	print_log("Handler address : 0x%08x", mContext.pCamera);

	print_log("Load %s", COMPONENT_RENDER);
	OMXsonienCheckError(OMX_GetHandle(&mContext.pRender, COMPONENT_RENDER, &mContext, pCallbackOMX));
	print_log("Handler address : 0x%08x", mContext.pRender);

	print_log("Load %s", COMPONENT_ENCODE);
	OMXsonienCheckError(OMX_GetHandle(&mContext.pEncode, COMPONENT_ENCODE, &mContext, pCallbackOMX));
	print_log("Handler address : 0x%08x", mContext.pEncode);
}

void componentConfigure() {
	OMX_ERRORTYPE err;
	OMX_PARAM_PORTDEFINITIONTYPE portDef;
	OMX_VIDEO_PORTDEFINITIONTYPE* formatVideo;

	// Disable any unused ports
	OMX_SendCommand(mContext.pCamera, OMX_CommandPortDisable, 70, NULL);
	OMX_SendCommand(mContext.pCamera, OMX_CommandPortDisable, 72, NULL);
	OMX_SendCommand(mContext.pCamera, OMX_CommandPortDisable, 73, NULL);

	// Configure OMX_IndexParamCameraDeviceNumber callback enable to ensure whether camera is initialized properly.
	print_log("Configure DeviceNumber callback enable.");
	OMX_CONFIG_REQUESTCALLBACKTYPE configCameraCallback;
	OMX_INIT_STRUCTURE(configCameraCallback);
	configCameraCallback.nPortIndex	= OMX_ALL;	// Must Be OMX_ALL
	configCameraCallback.nIndex 	= OMX_IndexParamCameraDeviceNumber;
	configCameraCallback.bEnable 	= OMX_TRUE;
	OMXsonienCheckError(OMX_SetConfig(mContext.pCamera, OMX_IndexConfigRequestCallback, &configCameraCallback));

	// OMX CameraDeviceNumber set -> will trigger Camera Ready callback
	print_log("Set CameraDeviceNumber parameter.");
	OMX_PARAM_U32TYPE deviceNumber;
	OMX_INIT_STRUCTURE(deviceNumber);
	deviceNumber.nPortIndex = OMX_ALL;
	deviceNumber.nU32 = 0;	// Mostly zero
	OMXsonienCheckError(OMX_SetParameter(mContext.pCamera, OMX_IndexParamCameraDeviceNumber, &deviceNumber));

	// Set video format of #71 port.
	print_log("Set video format of the camera : Using #71.");
	OMX_INIT_STRUCTURE(portDef);
	portDef.nPortIndex = 71;

	print_log("Get non-initialized definition of #71.");
	OMX_GetParameter(mContext.pCamera, OMX_IndexParamPortDefinition, &portDef);

	print_log("Set up parameters of video format of #71.");
	formatVideo = &portDef.format.video;
	formatVideo->eColorFormat 	= OMX_COLOR_FormatYUV420PackedPlanar;
	formatVideo->nFrameWidth	= mContext.nWidth;
	formatVideo->nFrameHeight	= mContext.nHeight;
	formatVideo->xFramerate		= mContext.nFramerate << 16;	// Fixed point. 1
	formatVideo->nStride		= formatVideo->nFrameWidth;		// Stride 0 -> Raise segment fault.
	formatVideo->nSliceHeight	= mContext.nHeight;
	portDef.nBufferCountMin = 3;
	portDef.nBufferCountActual = 3;
	OMXsonienCheckError(OMX_SetParameter(mContext.pCamera, OMX_IndexParamPortDefinition, &portDef));

	OMX_GetParameter(mContext.pCamera, OMX_IndexParamPortDefinition, &portDef);
	formatVideo = &portDef.format.video;
	mContext.nSliceSizeY = formatVideo->nFrameWidth * formatVideo->nSliceHeight;
	mContext.nSliceSizeU = mContext.nSliceSizeY / 4;
	mContext.nSliceSizeV = mContext.nSliceSizeY / 4;
	print_log("%d %d %d", mContext.nSliceSizeY, mContext.nSliceSizeU, mContext.nSliceSizeV);

	mContext.nOffsetY = 0;
	mContext.nOffsetU = mContext.nWidth * mContext.nHeight;
	mContext.nOffsetV = mContext.nWidth * mContext.nHeight * 5 / 4;

//	OMX_CONFIG_MIRRORTYPE	mirrorType;
//	OMX_INIT_STRUCTURE(mirrorType);
//	mirrorType.nPortIndex 	= 71;
//	mirrorType.eMirror 		= OMX_MirrorVertical;
//	OMX_SetConfig(mContext.pCamera, OMX_IndexConfigCommonMirror, &mirrorType);

	// Set video format of #90 port.
	print_log("Set video format of the render : Using #90.");
	OMX_INIT_STRUCTURE(portDef);
	portDef.nPortIndex = 90;

	print_log("Get default definition of #90.");
	OMX_GetParameter(mContext.pRender, OMX_IndexParamPortDefinition, &portDef);
	portDef.nBufferCountMin = 3;
	portDef.nBufferCountActual = 3;

	print_log("Set up parameters of video format of #90.");
	formatVideo = &portDef.format.video;
	formatVideo->eColorFormat 		= OMX_COLOR_FormatYUV420PackedPlanar;
	formatVideo->eCompressionFormat	= OMX_VIDEO_CodingUnused;
	formatVideo->nFrameWidth		= mContext.nWidth;
	formatVideo->nFrameHeight		= mContext.nHeight;
	formatVideo->nStride			= mContext.nWidth;
	formatVideo->nSliceHeight		= mContext.nHeight;
	formatVideo->xFramerate			= mContext.nFramerate << 16;
	OMXsonienCheckError(OMX_SetParameter(mContext.pRender, OMX_IndexParamPortDefinition, &portDef));

	// Set Encoder input format of #200 port.
	print_log("Set video format of the encoder input : Using #200.");
	OMX_INIT_STRUCTURE(portDef);
	portDef.nPortIndex = 200;

	print_log("Get default definition of #200.");
	OMX_GetParameter(mContext.pEncode, OMX_IndexParamPortDefinition, &portDef);
	portDef.nBufferCountMin = 3;
	portDef.nBufferCountActual = 3;

	print_log("Set up parameters of video format of #200.");
	formatVideo = &portDef.format.video;
	formatVideo->eColorFormat 		= OMX_COLOR_FormatYUV420PackedPlanar;
	formatVideo->eCompressionFormat = OMX_VIDEO_CodingUnused;
	formatVideo->nFrameWidth		= mContext.nWidth;
	formatVideo->nFrameHeight		= mContext.nHeight;
	formatVideo->nStride			= mContext.nWidth;
	formatVideo->nSliceHeight	    = mContext.nHeight;
	formatVideo->xFramerate			= mContext.nFramerate << 16;
	OMXsonienCheckError(OMX_SetParameter(mContext.pEncode, OMX_IndexParamPortDefinition, &portDef));

	// Set Encoder output format of #201 port.
	print_log("Set video format of the encoder output : Using #201.");
	OMX_INIT_STRUCTURE(portDef);
	portDef.nPortIndex = 201;

	print_log("Get default definition of #201.");
	OMX_GetParameter(mContext.pEncode, OMX_IndexParamPortDefinition, &portDef);
	// portDef.nBufferCountMin = 3;
	// portDef.nBufferCountActual = 3;

	print_log("Set up parameters of video format of #201.");
	formatVideo = &portDef.format.video;
	formatVideo->nFrameWidth		= mContext.nWidth;
	formatVideo->nFrameHeight		= mContext.nHeight;
	formatVideo->nStride			= 0; //mContext.nWidth;
	formatVideo->nSliceHeight		= 0; //mContext.nHeight;
	formatVideo->eCompressionFormat	= OMX_VIDEO_CodingAVC; // OMX_VIDEO_CodingMPEG4
	formatVideo->eColorFormat		= OMX_COLOR_FormatUnused;
	formatVideo->nBitrate = 10000000;
	OMXsonienCheckError(OMX_SetParameter(mContext.pEncode, OMX_IndexParamPortDefinition, &portDef));

	// Set Encoder target bitrate
	OMX_VIDEO_PARAM_BITRATETYPE bitrateType;
	OMX_INIT_STRUCTURE(bitrateType);
	bitrateType.nPortIndex = 201;
	bitrateType.eControlRate = OMX_Video_ControlRateVariable;
	bitrateType.nTargetBitrate = 10000000;
	OMXsonienCheckError(OMX_SetParameter(mContext.pEncode, OMX_IndexParamVideoBitrate, &bitrateType));

//	AVC Setting. Broadway decoder doesn't allows B frame nor Weighted PPrediction
//	ex :
//	ffmpeg -coder 0 -bf 0 -wpredp 0 ...

	OMX_VIDEO_PARAM_AVCTYPE param_avc;
	OMX_INIT_STRUCTURE(param_avc);
	param_avc.nPortIndex = 201;
	OMX_GetParameter(mContext.pEncode, OMX_IndexParamVideoAvc, &param_avc);
	param_avc.bWeightedPPrediction = OMX_FALSE;
	param_avc.eProfile = OMX_VIDEO_AVCProfileBaseline;
	param_avc.nBFrames = 0;
	OMXsonienCheckError(OMX_SetParameter(mContext.pEncode, OMX_IndexParamVideoAvc, &param_avc));

	// Configure rendering region
	OMX_CONFIG_DISPLAYREGIONTYPE displayRegion;
	OMX_INIT_STRUCTURE(displayRegion);
	displayRegion.nPortIndex = 90;
	displayRegion.dest_rect.width 	= mContext.nWidth;
	displayRegion.dest_rect.height 	= mContext.nHeight;
	displayRegion.set = OMX_DISPLAY_SET_NUM | OMX_DISPLAY_SET_FULLSCREEN | OMX_DISPLAY_SET_MODE | OMX_DISPLAY_SET_DEST_RECT;
	displayRegion.mode = OMX_DISPLAY_MODE_FILL;
	displayRegion.fullscreen = OMX_FALSE;
	displayRegion.num = 0;
	OMXsonienCheckError(OMX_SetConfig(mContext.pRender, OMX_IndexConfigDisplayRegion, &displayRegion));

	// Wait up for camera being ready.
	while(!mContext.isCameraReady) {
		print_log("Waiting until camera device is ready.");
		usleep(100 * 1000);
	}
	print_log("Camera is ready.");
}

void componentPrepare() {
	OMX_ERRORTYPE err;
	OMX_PARAM_PORTDEFINITIONTYPE portDef;

	// Request state of components to be IDLE.
	// The command will turn the component into waiting mode.
	// After allocating buffer to all enabled ports than the component will be IDLE.
	print_log("STATE : CAMERA - IDLE request");
	OMXsonienCheckError(OMX_SendCommand(mContext.pCamera, OMX_CommandStateSet, OMX_StateIdle, NULL));

	print_log("STATE : RENDER - IDLE request");
	OMXsonienCheckError(OMX_SendCommand(mContext.pRender, OMX_CommandStateSet, OMX_StateIdle, NULL));

	print_log("STATE : ENCODE - IDLE request");
	OMXsonienCheckError(OMX_SendCommand(mContext.pEncode, OMX_CommandStateSet, OMX_StateIdle, NULL));

	// Allocate buffers to render
	print_log("Allocate buffer to renderer #90 for input.");
	OMX_INIT_STRUCTURE(portDef);
	portDef.nPortIndex = 90;
	OMX_GetParameter(mContext.pRender, OMX_IndexParamPortDefinition, &portDef);
	print_log("Size of predefined buffer : %d * %d", portDef.nBufferSize, portDef.nBufferCountActual);
	mContext.pManagerRender = OMXsonienAllocateBuffer(mContext.pRender, 90, &mContext, 0, 0);

	// Allocate buffer to camera
	OMX_INIT_STRUCTURE(portDef);
	portDef.nPortIndex = 71;
	OMX_GetParameter(mContext.pCamera, OMX_IndexParamPortDefinition, &portDef);
	print_log("Size of predefined buffer : %d * %d", portDef.nBufferSize, portDef.nBufferCountActual);
	mContext.pManagerCamera = OMXsonienAllocateBuffer(mContext.pCamera, 71, &mContext, 0, 0);

	// Allocate buffer to encoder in
	OMX_INIT_STRUCTURE(portDef);
	portDef.nPortIndex = 200;
	OMX_GetParameter(mContext.pEncode, OMX_IndexParamPortDefinition, &portDef);
	print_log("Size of predefined buffer : %d * %d", portDef.nBufferSize, portDef.nBufferCountActual);
	mContext.pManagerEncIn = OMXsonienAllocateBuffer(mContext.pEncode, 200, &mContext, 0, 0);

	// Allocate buffer to encoder out
	OMX_INIT_STRUCTURE(portDef);
	portDef.nPortIndex = 201;
	OMX_GetParameter(mContext.pEncode, OMX_IndexParamPortDefinition, &portDef);
	print_log("Size of predefined buffer : %d * %d", portDef.nBufferSize, portDef.nBufferCountActual);
	mContext.pManagerEncOut = OMXsonienAllocateBuffer(mContext.pEncode, 201, &mContext, 0, 0);

	// Wait up for component being idle.
	if(!wait_for_state_change(OMX_StateIdle, mContext.pRender, mContext.pEncode, mContext.pCamera, NULL)) {
		print_log("FAIL");
		terminate();
		exit(-1);
	}
	print_log("STATE : IDLE OK!");
}

void prepareNextCanvas() {
	mContext.pCurrentCanvas = OMXsonienBufferGet(mContext.pManagerRender);
	while(!mContext.pCurrentCanvas) {
		usleep(100);
		// printf("Wait for next canvas\n");
		mContext.pCurrentCanvas = OMXsonienBufferGet(mContext.pManagerRender);
	}
	mContext.pCurrentY	= mContext.pCurrentCanvas->pBuffer;
	mContext.pCurrentU	= mContext.pCurrentY + mContext.nOffsetU;
	mContext.pCurrentV	= mContext.pCurrentY + mContext.nOffsetV;
}

size_t stream_callback(void *dst, size_t size, size_t nmemb, void *context) {
	CONTEXT* pContext = (CONTEXT*)context;
	int nbyte = 0;
	while(pContext->isValid) {
		nbyte = CBget(pContext->pBufferStreamOut, dst, size * nmemb);
		if(nbyte) break;
		usleep(1000);
	}

//	if(nbyte) {
//		printf("SEND : %5d / %5d bytes\n", nbyte, size * nmemb);
//	}
	return (size_t)nbyte;
}

void* thread_stream(void* data) {
	char url[1000];
	sprintf(&url, "http://127.0.0.1:8082/kovcam/%d/%d", mContext.nWidth, mContext.nHeight);

	curl_global_init(CURL_GLOBAL_ALL);
	CURL *curl = curl_easy_init();
	if(!curl) return NULL;

	curl_easy_setopt(curl, CURLOPT_READFUNCTION, stream_callback);
	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	// curl_easy_setopt(curl, CURLOPT_PUT, 1L);
	curl_easy_setopt(curl, CURLOPT_URL, &url);
	curl_easy_setopt(curl, CURLOPT_READDATA, &mContext);
	curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)-1);
	printf("Broadcast stream : START\n");
	curl_easy_perform(curl);
	printf("Broadcast stream : FINISH\n");
	curl_easy_cleanup(curl);
}

void* thread_encIn(void* data) {
	while(mContext.isValid) {
		sem_wait(&mContext.sem_encIn);
		OMX_BUFFERHEADERTYPE* pBuffer = CQget(mContext.pQueueEncIn);
		if(pBuffer == NULL) {
			continue;
		}

		OMX_EmptyThisBuffer(mContext.pEncode, pBuffer);
	}
}

void* thread_engine(void* data) {
	while(mContext.isValid) {
		sem_wait(&mContext.sem_engine);
		OMX_BUFFERHEADERTYPE* pBuffer = CQget(mContext.pQueueCameraBuffer);
		if(pBuffer == NULL) {
			continue;
		}

		// print_log("HAHA! 0x%08x 0x%08x 0x%08x", mContext.pCurrentY, mContext.pCurrentU, mContext.pCurrentV);
		// 영상처리
		OMX_U8* pCursor = pBuffer->pBuffer;
		memcpy(mContext.pCurrentY, pCursor, mContext.nSliceSizeY);
		mContext.pCurrentY += mContext.nSliceSizeY;
		pCursor += mContext.nSliceSizeY;

		memcpy(mContext.pCurrentU, pCursor, mContext.nSliceSizeU);
		mContext.pCurrentU += mContext.nSliceSizeU;
		pCursor += mContext.nSliceSizeU;

		memcpy(mContext.pCurrentV, pCursor, mContext.nSliceSizeV);
		mContext.pCurrentV += mContext.nSliceSizeV;

		mContext.pCurrentCanvas->nFilledLen += pBuffer->nFilledLen;
		if(pBuffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
			// Request Encoding
			OMX_BUFFERHEADERTYPE* pBufferEncIn = OMXsonienBufferGet(mContext.pManagerEncIn);
			if(pBufferEncIn) {
				memcpy(pBufferEncIn->pBuffer, mContext.pCurrentCanvas->pBuffer, mContext.pCurrentCanvas->nFilledLen);
				pBufferEncIn->nFilledLen = mContext.pCurrentCanvas->nFilledLen;
				CQput(mContext.pQueueEncIn, pBufferEncIn);
				sem_post(&mContext.sem_encIn);
			}
			else {
				printf("[WARN] Skip a frame\n");
			}
			OMX_EmptyThisBuffer(mContext.pRender, mContext.pCurrentCanvas);
			prepareNextCanvas();

			mContext.nFrameCaptured++;
		}

		// 버퍼 반납
		OMX_FillThisBuffer(mContext.pCamera, pBuffer);
	}
}

int main(void) {
	/* Temporary variables */
	OMX_ERRORTYPE	err;
	OMX_PARAM_PORTDEFINITIONTYPE	portDef;

	/* Initialize application variables */
	memset(&mContext, 0, (size_t)sizeof(mContext));
	mContext.nWidth 	= CAPTURE_WIDTH;
	mContext.nHeight 	= CAPTURE_HEIGHT;
	mContext.nFramerate	= CAPTURE_FRAMERATES;
	mContext.isValid	= OMX_TRUE;
	sem_init(&mContext.sem_engine, 0, 0);
	sem_init(&mContext.sem_encIn, 0, 0);
	sem_init(&mContext.sem_stream, 0, 0);
#ifdef FIFO
	mContext.fp_stream = fopen(FIFO_PATH, "wb");
#endif
	// mContext.f_stream = open(FIFO_PATH, O_RDWR);

	// RPI initialize.
	bcm_host_init();

	// OMX initialize.
	print_log("Initialize OMX");
	if((err = OMX_Init()) != OMX_ErrorNone) {
		print_omx_error(err, "FAIL");
		OMX_Deinit();
		exit(-1);
	}

	// OMXsonien helper initialize
	OMXsonienInit();
	OMXsonienSetErrorCallback(onOMXsonienError);

	// For loading component, Callback shall provide.
	OMX_CALLBACKTYPE callbackOMX;
	callbackOMX.EventHandler	= onOMXevent;
	callbackOMX.EmptyBufferDone	= onEmptyBufferDone;
	callbackOMX.FillBufferDone	= onFillBufferDone;

	componentLoad(&callbackOMX);
	componentConfigure();
	componentPrepare();

	// Request state of component to be EXECUTE.
	print_log("STATE : CAMERA - EXECUTING request");
	OMXsonienCheckError(OMX_SendCommand(mContext.pCamera, OMX_CommandStateSet, OMX_StateExecuting, NULL));

	print_log("STATE : RENDER - EXECUTING request");
	OMXsonienCheckError(OMX_SendCommand(mContext.pRender, OMX_CommandStateSet, OMX_StateExecuting, NULL));

	print_log("STATE : ENCODE - EXECUTING request");
	OMXsonienCheckError(OMX_SendCommand(mContext.pEncode, OMX_CommandStateSet, OMX_StateExecuting, NULL));

	if(!wait_for_state_change(OMX_StateExecuting, mContext.pCamera, mContext.pRender, NULL)) {
		print_log("FAIL");
		terminate();
		exit(-1);
	}
	print_log("STATE : EXECUTING OK!");

	// Queue and Buffer 준비
	mContext.pBufferStreamOut = CBcreateInstance(1024 * 1024);
	mContext.pQueueCameraBuffer = CQcreateInstance(3);
	mContext.pQueueEncIn = CQcreateInstance(3);

	// 쓰기 버퍼 준비
	{
		OMX_BUFFERHEADERTYPE* buffer = NULL;
		while((buffer = OMXsonienBufferGet(mContext.pManagerEncOut))) {
			OMX_FillThisBuffer(mContext.pEncode, buffer);
		}

		buffer = NULL;
		while((buffer = OMXsonienBufferGet(mContext.pManagerCamera))) {
			OMX_FillThisBuffer(mContext.pCamera, buffer);
		}
	}

	usleep(20 * 1000);	// Wait for buffer fully filled.

	// Ready for render
	prepareNextCanvas();

	// Stream start
	pthread_create(&mContext.thread_stream, NULL, thread_stream, NULL);

	// Engine Start
	pthread_create(&mContext.thread_engine, NULL, thread_engine, NULL);

	// Encoder In pump Start
	pthread_create(&mContext.thread_encIn, NULL, thread_encIn, NULL);

	// Since #71 is capturing port, needs capture signal like other handy capture devices
	print_log("Capture start.");
	OMX_CONFIG_PORTBOOLEANTYPE	portCapturing;
	OMX_INIT_STRUCTURE(portCapturing);
	portCapturing.nPortIndex = 71;
	portCapturing.bEnabled = OMX_TRUE;
	OMX_SetConfig(mContext.pCamera, OMX_IndexConfigPortCapturing, &portCapturing);

	// Set signal interrupt handler
	signal(SIGINT, 	onSignal);
	signal(SIGTSTP, onSignal);
	signal(SIGTERM, onSignal);

	long nFrameCaptured = 0;
	while(mContext.isValid) {
		usleep(1000000);

		long nFrames = mContext.nFrameCaptured;
		print_log("%2d frame captured", nFrames - nFrameCaptured);
		nFrameCaptured = nFrames;
	}

	signal(SIGINT, 	SIG_DFL);
	signal(SIGTSTP, SIG_DFL);
	signal(SIGTERM, SIG_DFL);

	portCapturing.bEnabled = OMX_FALSE;
	OMX_SetConfig(mContext.pCamera, OMX_IndexConfigPortCapturing, &portCapturing);
	print_log("Capture stop.");

	terminate();
}

