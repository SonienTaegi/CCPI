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

void YUV422to420(unsigned char* pSrc, unsigned char* pDst, unsigned int width, unsigned int height) {
	unsigned char* pCur = pSrc;
	unsigned char* pEOF = pSrc + width * height * 2;
	unsigned char* pY = pDst;
	unsigned char* pU = pDst + width * height;
	unsigned char* pV = pDst + width * height * 5 / 4;

	while(pCur < pEOF) {
		*pY++ = *pCur;
		pCur += 2;
	}

	for(int j = 0; j < height; j+=2) {
	for(int k = 0; k < width; k+=2) {
		unsigned char* pUV = pSrc + ( j * width + k ) * 2;
		*pU++ = *(pUV + 1);
		*pV++ = *(pUV + 3);
	}
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
	printf("%dus\n", check_out() / 1000);

	check_in();
	for(int i = HEIGHT / SLICE_HEIGHT; i--; ){
		memcpy(pDst, pSrc, nSize);
	}
	printf("%dus\n", check_out() / 1000);

	// YUVY -> YUV420 Convert Test
	unsigned char* pRaw = malloc(WIDTH * HEIGHT * 2);
	unsigned char* pYuv = malloc(WIDTH * HEIGHT * 3 / 2);

	{
		check_in();
		unsigned char* pCur = pRaw;
		unsigned char* pEOF = pRaw + WIDTH * HEIGHT * 2;
		unsigned char* pY = pYuv;
		unsigned char* pU = pY + WIDTH * HEIGHT;
		unsigned char* pV = pU + WIDTH * HEIGHT / 4;

		int isEvenRow = 1;
		for(int y = 0; y < HEIGHT; y++) {
			isEvenRow = y & 0x01;
			for(int x = 0; x < WIDTH; x +=2 ) {
				*pY++ = *pCur++;
				if(isEvenRow) 	pCur++;
				else 			*pU++ = *pCur++;
				*pY++ = *pCur++;
				if(isEvenRow) 	pCur++;
				else 			*pV++ = *pCur++;
			}
		}
		printf("%dus\n", check_out() / 1000);
	}

	{
		check_in();
		YUV422to420(pRaw, pYuv, WIDTH, HEIGHT);
		printf("%dus\n", check_out() / 1000);
	}
	free(pSrc);
	free(pDst);
	free(pRaw);
	free(pYuv);


	return 0;
}
