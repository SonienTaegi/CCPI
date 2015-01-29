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

	unsigned int				nSizeY, nSizeU, nSizeV;
	OMXsonien_BUFFERMANAGER*	pManagerCamera;
	OMXsonien_BUFFERMANAGER*	pManagerRender;
	OMX_BUFFERHEADERTYPE*		pBufferCameraCurrent;

	OMX_BOOL					isValid;
	pthread_t					thread_fps;
	pthread_t					thread_pump_camera;
	unsigned int				nFrameCaptured;
} CONTEXT;

/* Application variant */
CONTEXT mContext;
sem_t sem_pump_camera;

/* Performance measurer */
struct timespec ts_checkAt;
void check_in() {
	clock_gettime(CLOCK_REALTIME, &ts_checkAt);
}
void check_out() {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long nGap = ( ts.tv_sec - ts_checkAt.tv_sec ) * 1000000000 + ( ts.tv_nsec - ts_checkAt.tv_nsec );
	printf("Time elapsed %8ld us\n", nGap / 1000 );
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
// int nFrames = 0;
OMX_ERRORTYPE onFillBufferDone (
		OMX_OUT OMX_HANDLETYPE hComponent,
		OMX_OUT OMX_PTR pAppData,
		OMX_OUT OMX_BUFFERHEADERTYPE* pBuffer) {
//	if(pBuffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME && ++nFrames == 30) {
//		nFrames = 0;
//		printf("PERIOD : ");
//		check_out();
//		check_in();
//	}

	mContext.pBufferCameraCurrent = pBuffer;
	return OMX_ErrorNone;
}

/* Callback : Render-in buffer is emptied */
/* Callback : Error detection callback of OMXsonien */
OMX_ERRORTYPE onEmptyBufferDone(
		OMX_IN OMX_HANDLETYPE hComponent,
		OMX_IN OMX_PTR pAppData,
		OMX_IN OMX_BUFFERHEADERTYPE* pBuffer) {

	OMXsonienBufferPut(mContext.pManagerRender, pBuffer);
	return OMX_ErrorNone;
}

void onOMXsonienError(OMX_ERRORTYPE err) {
	printf("Error : 0x%08x\n", err);
	terminate();
	exit(-1);
}

void onSignal(int signal) {
	mContext.isValid = OMX_FALSE;
}

void* thread_fps_counter(void* data) {
	unsigned int nFrameTracked = 0;

	while(mContext.isValid) {
		usleep(1000 * 1000);

		int nFrameNow = mContext.nFrameCaptured;
		printf("FPS : %d\n", nFrameNow - nFrameTracked);
		nFrameTracked = nFrameNow;
	}

	pthread_exit(NULL);
}

void* thread_pump_camera(void* data) {
	while(mContext.isValid) {
		while(1) {
			OMX_BUFFERHEADERTYPE* pBuffer = OMXsonienBufferGet(mContext.pManagerCamera);
			if(pBuffer == NULL) break;

			OMX_FillThisBuffer(mContext.pCamera, pBuffer);
		}
		sem_wait(&sem_pump_camera);
	}
}

void terminate() {
	print_log("On terminating...");

	if(mContext.thread_fps) {
		pthread_join(mContext.thread_fps, NULL);
	}

	if(mContext.thread_pump_camera) {
		sem_post(&sem_pump_camera);
		pthread_join(mContext.thread_pump_camera, NULL);
		sem_close(&sem_pump_camera);
	}

	OMX_STATETYPE state;
	OMX_BOOL bWaitForCamera, bWaitForRender;

	// Execute -> Idle
	bWaitForCamera = bWaitForRender = OMX_FALSE;
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
	if(bWaitForCamera) wait_for_state_change(OMX_StateLoaded, mContext.pCamera, NULL);
	if(bWaitForRender) wait_for_state_change(OMX_StateLoaded, mContext.pRender, NULL);

	// Loaded -> Free
	if(isState(mContext.pCamera, OMX_StateLoaded)) OMX_FreeHandle(mContext.pCamera);
	if(isState(mContext.pRender, OMX_StateLoaded)) OMX_FreeHandle(mContext.pRender);

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
	portDef.nBufferCountMin = 3;
	portDef.nBufferCountActual = 3;

	print_log("Set up parameters of video format of #71.");
	formatVideo = &portDef.format.video;
	formatVideo->eColorFormat 	= OMX_COLOR_FormatYUV420PackedPlanar;
	formatVideo->nFrameWidth	= mContext.nWidth;
	formatVideo->nFrameHeight	= mContext.nHeight;
	formatVideo->xFramerate		= mContext.nFramerate << 16;	// Fixed point. 1
	formatVideo->nStride		= formatVideo->nFrameWidth;		// Stride 0 -> Raise segment fault.
	OMXsonienCheckError(OMX_SetParameter(mContext.pCamera, OMX_IndexParamPortDefinition, &portDef));

	OMX_GetParameter(mContext.pCamera, OMX_IndexParamPortDefinition, &portDef);
	formatVideo = &portDef.format.video;
	mContext.nSizeY = formatVideo->nFrameWidth * formatVideo->nSliceHeight;
	mContext.nSizeU	= mContext.nSizeY / 4;
	mContext.nSizeV	= mContext.nSizeY / 4;
	print_log("%d %d %d", mContext.nSizeY, mContext.nSizeU, mContext.nSizeV);

	OMX_CONFIG_MIRRORTYPE	mirrorType;
	OMX_INIT_STRUCTURE(mirrorType);
	mirrorType.nPortIndex 	= 71;
	mirrorType.eMirror 		= OMX_MirrorVertical;
	OMX_SetConfig(mContext.pCamera, OMX_IndexConfigCommonMirror, &mirrorType);

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

int main(void) {
	/* Temporary variables */
	OMX_ERRORTYPE	err;
	OMX_PARAM_PORTDEFINITIONTYPE	portDef;

	/* Initialize application variables */
	memset(&mContext, 0, (size_t)sizeof(mContext));
	mContext.nWidth 	= 1024;
	mContext.nHeight 	= 768;
	mContext.nFramerate	= 30;
	mContext.isValid	= OMX_TRUE;

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

	if(!wait_for_state_change(OMX_StateExecuting, mContext.pCamera, mContext.pRender, NULL)) {
		print_log("FAIL");
		terminate();
		exit(-1);
	}
	print_log("STATE : EXECUTING OK!");

	pthread_create(&mContext.thread_fps, NULL, thread_fps_counter, NULL);

	// Start camera buffer pump thread : put the first buffer thread
	pthread_create(&mContext.thread_pump_camera, NULL, thread_pump_camera, NULL);

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

	OMX_U8*			pY = NULL;
	OMX_U8*			pU = NULL;
	OMX_U8*			pV = NULL;
	unsigned int	nOffsetU 	= mContext.nWidth * mContext.nHeight;
	unsigned int 	nOffsetV 	= nOffsetU * 5 / 4;
	OMX_BUFFERHEADERTYPE* pBufferRenderCurrent = OMXsonienBufferGet(mContext.pManagerRender);
	while(mContext.isValid) {
		if(mContext.pBufferCameraCurrent) {
			OMX_BUFFERHEADERTYPE* pBufferCamera = mContext.pBufferCameraCurrent;
			mContext.pBufferCameraCurrent = NULL;

			if(pBufferRenderCurrent->nFilledLen == 0) {
				pY = pBufferRenderCurrent->pBuffer;
				pU = pY + nOffsetU;
				pV = pY + nOffsetV;
			}

			OMX_U8*	pSrc = pBufferCamera->pBuffer;
			memcpy(pY, pSrc, mContext.nSizeY);	pY += mContext.nSizeY;	 pSrc += mContext.nSizeY;
			memcpy(pU, pSrc, mContext.nSizeU);	pU += mContext.nSizeU;	 pSrc += mContext.nSizeU;
			memcpy(pV, pSrc, mContext.nSizeV);	pV += mContext.nSizeV;	 // pSrc += mContext.nSizeV;

			pBufferRenderCurrent->nFilledLen += pBufferCamera->nFilledLen;
			if(pBufferCamera->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
				OMX_EmptyThisBuffer(mContext.pRender, pBufferRenderCurrent);
				pBufferRenderCurrent = OMXsonienBufferGet(mContext.pManagerRender);
				mContext.nFrameCaptured++;
			}
			OMXsonienBufferPut(mContext.pManagerCamera, pBufferCamera);
			check_in();
			sem_post(&sem_pump_camera);
			check_out();
		}
		else {
			usleep(10);
		}
	}

	signal(SIGINT, 	SIG_DFL);
	signal(SIGTSTP, SIG_DFL);
	signal(SIGTERM, SIG_DFL);

	portCapturing.bEnabled = OMX_FALSE;
	OMX_SetConfig(mContext.pCamera, OMX_IndexConfigPortCapturing, &portCapturing);
	print_log("Capture stop.");

	terminate();
}

