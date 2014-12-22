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

/* Performance measurer */
struct timespec ts_checkAt;
void check_in() {
	clock_gettime(CLOCK_REALTIME, &ts_checkAt);
}
long check_out() {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long nGap = ( ts.tv_sec - ts_checkAt.tv_sec ) * 1000000000 + ( ts.tv_nsec - ts_checkAt.tv_nsec );
	printf("Time elapsed %8ld us\n", nGap / 1000 );

	return nGap;
}

int main(void) {
	const int TEST_COUNT	= 10000;
	int counter;

	// Test : semaphore
	sem_t	sem;
	sem_init(&sem, 0, 1);

	counter = TEST_COUNT;
	long 	elapse_wait	= 0;
	long 	elapse_post	= 0;

	while(--counter) {
		check_in();
		sem_wait(&sem);
		elapse_wait += check_out();

		usleep(100);

		check_in();
		sem_post(&sem);
		elapse_post += check_out();
	}
	sem_destroy(&sem);

	// Test	: Mutext
	pthread_mutex_t mutex;
	pthread_mutex_init(&mutex, NULL);

	counter = TEST_COUNT;
	long 	elapse_lock		= 0;
	long 	elapse_unlock	= 0;

	while(--counter) {
		check_in();
		pthread_mutex_lock(&mutex);
		elapse_lock += check_out();

		usleep(100);

		check_in();
		pthread_mutex_unlock(&mutex);
		elapse_unlock += check_out();
	}

	// Result :
	printf("WAIT   : %ld\nPOST   : %ld\nLOCK   : %ld\nUNLOCK : %ld\n",
				elapse_wait, elapse_post, elapse_lock, elapse_unlock);
}

