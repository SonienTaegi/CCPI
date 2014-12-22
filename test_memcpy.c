#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#define WIDTH		1920
#define	HEIGHT		1080
#define SLICE_HEIGHT	16

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
