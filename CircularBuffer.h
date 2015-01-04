#include <pthread.h>

#ifndef __CIRCULAR_BUFFER_H
#define __CIRCULAR_BUFFER_H

typedef unsigned char 	CB_BYTE;
typedef CB_BYTE*		CB_BYTE_PTR;
typedef struct CB_INSTANCE {
	CB_BYTE* begin;
	CB_BYTE* end;
	CB_BYTE* head;
	CB_BYTE* tail;
	pthread_mutex_t mutex;
} CB_INSTANCE;

CB_INSTANCE* CBcreateInstance(int MAX_CB_SIZE);

void CBdestroy(CB_INSTANCE* buffer);

int CBput(CB_INSTANCE* buffer, CB_BYTE_PTR src, int size);

int CBget(CB_INSTANCE* buffer, CB_BYTE_PTR dst, int size);

int CBfreeSpace(CB_INSTANCE* buffer);

#endif
