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
#include <pthread.h>
#include <semaphore.h>

#include <IL/OMX_Core.h>
#include <IL/OMX_Component.h>
#include <IL/OMX_Video.h>
#include <IL/OMX_Broadcom.h>

#include "common.h"
#include "CircularQueue.h"
#include "OMXsonien.h"
#include "arm_simd.h"

#define	COMPONENT_CAMERA	"OMX.broadcom.camera"
#define COMPONENT_RENDER	"OMX.broadcom.video_render"

typedef struct {
	OMX_HANDLETYPE				pCamera;
	OMX_HANDLETYPE				pRender;
	OMX_BOOL					isCameraReady;

	unsigned int				nWidth;
	unsigned int				nHeight;
	unsigned int				nFramerate;

	unsigned int				nSliceSizeY, nSliceSizeU, nSliceSizeV;
	unsigned int				nOffsetY, nOffsetU, nOffsetV;
	OMXsonien_BUFFERMANAGER*	pManagerCamera;
	OMXsonien_BUFFERMANAGER*	pManagerRender;
	OMX_BUFFERHEADERTYPE*		pCurrentCanvas;
	OMX_U8*						pCurrentY;
	OMX_U8*						pCurrentU;
	OMX_U8*						pCurrentV;
	CQ_INSTANCE*				pQueueCameraBuffer;
	pthread_t					thread_engine;
	sem_t						sem_engine;
	OMX_BOOL					isValid;
	unsigned int				nFrameCaptured;
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

/* Callback : Camera-out buffer is filled */
OMX_ERRORTYPE onFillCameraOut (
		OMX_OUT OMX_HANDLETYPE hComponent,
		OMX_OUT OMX_PTR pAppData,
		OMX_OUT OMX_BUFFERHEADERTYPE* pBuffer) {
	CQput(mContext.pQueueCameraBuffer, pBuffer);
	sem_post(&mContext.sem_engine);

	return OMX_ErrorNone;
}

/* Callback : Render-in buffer is emptied */
OMX_ERRORTYPE onEmptyRenderIn(
		OMX_IN OMX_HANDLETYPE hComponent,
		OMX_IN OMX_PTR pAppData,
		OMX_IN OMX_BUFFERHEADERTYPE* pBuffer) {
	OMXsonienBufferPut(mContext.pManagerRender, pBuffer);
	return OMX_ErrorNone;
}

/* Callback : Error detection callback of OMXsonien */
void onOMXsonienError(OMX_ERRORTYPE err) {
	printf("Error : 0x%08x\n", err);
	terminate();
	exit(-1);
}

void onSignal(int signal) {
	mContext.isValid = OMX_FALSE;
}

void terminate() {
	print_log("On terminating...");

	OMX_STATETYPE state;
	OMX_BOOL bWaitForCamera, bWaitForScheduler, bWaitForRender;

	// Engine Stop
	if(mContext.thread_engine) {
		sem_post(&mContext.sem_engine);
		pthread_join(mContext.thread_engine, NULL);
		mContext.thread_engine = NULL;
		sem_close(&mContext.sem_engine);
	}

	// Execute -> Idle
	bWaitForCamera = bWaitForScheduler = bWaitForRender = OMX_FALSE;
	if(isState(mContext.pCamera, OMX_StateExecuting)) {
		OMX_SendCommand(mContext.pCamera, OMX_CommandStateSet, OMX_StateIdle, NULL);
		bWaitForCamera = OMX_TRUE;
	}
	if(isState(mContext.pRender, OMX_StateExecuting)) {
		OMX_SendCommand(mContext.pRender, OMX_CommandStateSet, OMX_StateIdle, NULL);
		bWaitForRender = OMX_TRUE;
	}
	if(bWaitForCamera) wait_for_state_change(OMX_StateIdle, mContext.pCamera, NULL);
	if(bWaitForRender) wait_for_state_change(OMX_StateIdle, mContext.pRender, NULL);

	// Idle -> Loaded
	bWaitForCamera = bWaitForRender = OMX_FALSE;
	if(isState(mContext.pCamera, OMX_StateIdle)) {
		OMX_SendCommand(mContext.pCamera, OMX_CommandStateSet, OMX_StateLoaded, NULL);
		bWaitForCamera = OMX_TRUE;
	}
	if(isState(mContext.pRender, OMX_StateIdle)) {
		OMX_SendCommand(mContext.pRender, OMX_CommandStateSet, OMX_StateLoaded, NULL);
		bWaitForRender = OMX_TRUE;
	}

	// Buffer release
	OMXsonienFreeBuffer(mContext.pManagerCamera);
	OMXsonienFreeBuffer(mContext.pManagerRender);

	if(bWaitForCamera) wait_for_state_change(OMX_StateLoaded, mContext.pCamera, NULL);
	if(bWaitForRender) wait_for_state_change(OMX_StateLoaded, mContext.pRender, NULL);

	// Loaded -> Free
	if(isState(mContext.pCamera, OMX_StateLoaded)) OMX_FreeHandle(mContext.pCamera);
	if(isState(mContext.pRender, OMX_StateLoaded)) OMX_FreeHandle(mContext.pRender);

	if(mContext.pQueueCameraBuffer) {
		CQdestroy(mContext.pQueueCameraBuffer);
	}

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
//	portDef.nBufferCountMin = 3;
//	portDef.nBufferCountActual = 3;

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

	// Wait up for component being idle.
	if(!wait_for_state_change(OMX_StateIdle, mContext.pRender, mContext.pCamera, NULL)) {
		print_log("FAIL");
		terminate();
		exit(-1);
	}
	print_log("STATE : IDLE OK!");
}

void prepareNextCanvas() {
	mContext.pCurrentCanvas = OMXsonienBufferGet(mContext.pManagerRender);
	mContext.pCurrentY	= mContext.pCurrentCanvas->pBuffer;
	mContext.pCurrentU	= mContext.pCurrentY + mContext.nOffsetU;
	mContext.pCurrentV	= mContext.pCurrentY + mContext.nOffsetV;
}

//int  nFrameTemp 	= 0;
//long time_elapse 	= 0;
//long time_elapsed 	= 0;

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
			OMX_EmptyThisBuffer(mContext.pRender, mContext.pCurrentCanvas);
			prepareNextCanvas();

			mContext.nFrameCaptured++;
			/*
			if( ++nFrameTemp == 30) {
				print_log("Elapsed time for fill buffer %d ms", (time_elapse - time_elapsed) / 1000000 );
				time_elapsed = time_elapse;
				nFrameTemp = 0;
			}
			*/
		}

		// 버퍼 반납
//		check_in();
		OMX_FillThisBuffer(mContext.pCamera, pBuffer);
//		time_elapse += check_out();
	}
}

int main(void) {
	/* Temporary variables */
	OMX_ERRORTYPE	err;
	OMX_PARAM_PORTDEFINITIONTYPE	portDef;

	/* Initialize application variables */
	memset(&mContext, 0, (size_t)sizeof(mContext));
	mContext.nWidth 	= 1280;
	mContext.nHeight 	= 960;
	mContext.nFramerate	= 30;
	mContext.isValid	= OMX_TRUE;
	sem_init(&mContext.sem_engine, 0, 0);

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
	callbackOMX.EmptyBufferDone	= onEmptyRenderIn;
	callbackOMX.FillBufferDone	= onFillCameraOut;

	componentLoad(&callbackOMX);
	componentConfigure();
	componentPrepare();

	// Request state of component to be EXECUTE.
	print_log("STATE : CAMERA - EXECUTING request");
	OMXsonienCheckError(OMX_SendCommand(mContext.pCamera, OMX_CommandStateSet, OMX_StateExecuting, NULL));

	print_log("STATE : RENDER - EXECUTING request");
	OMXsonienCheckError(OMX_SendCommand(mContext.pRender, OMX_CommandStateSet, OMX_StateExecuting, NULL));

	if(!wait_for_state_change(OMX_StateExecuting, mContext.pCamera, mContext.pRender, NULL)) {
		print_log("FAIL");
		terminate();
		exit(-1);
	}
	print_log("STATE : EXECUTING OK!");

	// Camera Callback 용 Queue 생성
	mContext.pQueueCameraBuffer = CQcreateInstance(3);
	OMX_BUFFERHEADERTYPE* bufferScheduler;
	while((bufferScheduler = OMXsonienBufferGet(mContext.pManagerCamera))) {
		OMX_FillThisBuffer(mContext.pCamera, bufferScheduler);
	}

	usleep(20 * 1000);	// Wait for buffer fully filled.

	// Ready for render
	prepareNextCanvas();

	// Engine Start
	pthread_create(&mContext.thread_engine, NULL, thread_engine, NULL);

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

