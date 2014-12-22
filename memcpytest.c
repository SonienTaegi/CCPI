#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#define WIDTH		1920
#define	HEIGHT		1080
#define SLICE_HEIGHT	16

long nCheckAt;

void check_in() {
	struct timespec tp;
	clock_gettime(CLOCK_REALTIME, &tp);
	nCheckAt = tp.tv_nsec;
}

void check_out() {
	struct timespec tp;
	clock_gettime(CLOCK_REALTIME, &tp);
	long nNow = tp.tv_nsec;
	printf("Time elapsed %8d us\n", ( nNow - nCheckAt ) / 1000 );
}

void memcpyi(int* pDst, int* pSrc, int size) {
	int i = 0;
//	for(i = 0; i < size; i++) {
//	while(size--) {
	while(i++ < size) {
		*pDst++ = *pSrc++;
	}
}

int main(void) {
	int nSize = WIDTH * SLICE_HEIGHT;
	unsigned char* pSrc	= malloc(nSize);
	unsigned char* pDst	= malloc(nSize);

	check_in();
	for(int i = HEIGHT / SLICE_HEIGHT; i--; ){
		memcpyi(pDst, pSrc, nSize/4);
	}
	check_out();

	check_in();
	for(int i = HEIGHT / SLICE_HEIGHT; i--; ){
		memcpy(pDst, pSrc, nSize);
	}
	check_out();

	return 0;
}
