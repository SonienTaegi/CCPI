// #define __CB_LOG

#include <stdio.h>
#include "CircularBuffer.h"

typedef struct CB_FREE_SPACE {
	int planeA;
	int planeB;
} CB_FREE_SPACE;

CB_FREE_SPACE getFreeSpaceSize(CB_INSTANCE* buffer) {
	CB_FREE_SPACE freeSpace;
	if(buffer-> head > buffer->tail) {
		freeSpace.planeA = buffer->head - buffer->tail - 1;
		freeSpace.planeB = 0;
	}
	else {
		freeSpace.planeA = buffer->end - buffer->tail;
		freeSpace.planeB = buffer->head - buffer->begin;
		if(buffer->head == buffer->begin) {
			freeSpace.planeA -= 1;
		}
		else {
			freeSpace.planeB -= 1;
		}
	}

	return freeSpace;
}

inline void move(CB_BYTE_PTR* pPtr, int size, CB_BYTE_PTR begin, CB_BYTE_PTR end) {
	*pPtr += size;
	int overflow = *pPtr - end;
	if(overflow >= 0) {
		*pPtr = begin + overflow;
	}
}

void moveHead(CB_INSTANCE* buffer, int size) {
	move(&(buffer->head), size, buffer->begin, buffer->end);
}

void moveTail(CB_INSTANCE* buffer, int size) {
	move(&(buffer->tail), size, buffer->begin, buffer->end);
}

CB_INSTANCE* CBcreateInstance(int MAX_CB_SIZE) {
	CB_INSTANCE* buffer = malloc(sizeof(CB_INSTANCE));

	buffer->begin = malloc( MAX_CB_SIZE + 1 );
	buffer->end   = buffer->begin + MAX_CB_SIZE + 1;
	buffer->head  = buffer->begin;
	buffer->tail  = buffer->begin;
#ifdef __CB_LOG
	printf("0x%08x : INT 0x%08x - 0x%08x\n", buffer->begin, buffer->begin, buffer->end);
#endif
	pthread_mutex_init(&buffer->mutex, NULL);
	return buffer;
}

void CBdestroy(CB_INSTANCE* buffer) {
	pthread_mutex_destroy(&buffer->mutex);
	buffer->end   = NULL;
	buffer->head  = NULL;
	buffer->tail  = NULL;
	free(buffer->begin);
	buffer->begin = NULL;
}

int CBput(CB_INSTANCE* buffer, CB_BYTE_PTR src, int size) {
	pthread_mutex_lock(&buffer->mutex);

	CB_FREE_SPACE freeSpace = getFreeSpaceSize(buffer);
	if(freeSpace.planeA + freeSpace.planeB < size) {
		pthread_mutex_unlock(&buffer->mutex);
		return -1;
	}

	if(freeSpace.planeA) {
		int sizeA = freeSpace.planeA < size ? freeSpace.planeA : size;
		memcpy(buffer->tail, src, sizeA);
		src += sizeA;
		moveTail(buffer, sizeA);
		size -= sizeA;
	}

	if(size) {
		memcpy(buffer->tail, src, size);
		moveTail(buffer, size);
	}

#ifdef __CB_LOG
	printf("0x%08x : PUT 0x%08x - 0x%08x\n", buffer->begin, buffer->head, buffer->tail);
#endif
	pthread_mutex_unlock(&buffer->mutex);
	return 0;
}

int CBget(CB_INSTANCE* buffer, CB_BYTE_PTR dst, int size) {
	pthread_mutex_lock(&buffer->mutex);
	int sizeFetch = 0;

	if(buffer->head == buffer->tail) {
		pthread_mutex_unlock(&buffer->mutex);
		return sizeFetch;
	}

	if(buffer->head > buffer->tail) {
		int sizeA = buffer->end - buffer->head;
		sizeA = size < sizeA ? size : sizeA;

		memcpy(dst, buffer->head, sizeA);
		dst += sizeA;
		moveHead(buffer, sizeA);
		size -= sizeA;
		sizeFetch += sizeA;

		if(buffer->head == buffer->tail) {
			pthread_mutex_unlock(&buffer->mutex);
			return sizeFetch;
		}
	}

	if(size) {
		int sizeB = buffer->tail - buffer->head;
		sizeB = size < sizeB ? size : sizeB;

		memcpy(dst, buffer->head, sizeB);
		moveHead(buffer, sizeB);
		sizeFetch += sizeB;
	}

#ifdef __CB_LOG
	printf("0x%08x : GET 0x%08x - 0x%08x\n", buffer->begin, buffer->head, buffer->tail);
#endif
	pthread_mutex_unlock(&buffer->mutex);
	return sizeFetch;
}

int CBfreeSpace(CB_INSTANCE* buffer) {
	CB_FREE_SPACE freeSpace;
	pthread_mutex_lock(&buffer->mutex);
	freeSpace = getFreeSpaceSize(buffer);
	pthread_mutex_unlock(&buffer->mutex);

	return freeSpace.planeA + freeSpace.planeB;
}
