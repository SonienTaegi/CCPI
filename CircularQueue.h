#include <pthread.h>

#ifndef __CIRCULAR_QUEUE_H
#define __CIRCULAR_QUEUE_H

typedef void*	CQ_PTR;
typedef struct CQ_INSTANCE {
	CQ_PTR* begin;
	CQ_PTR* end;
	CQ_PTR* head;
	CQ_PTR* tail;
	pthread_mutex_t mutex;
} CQ_INSTANCE;

CQ_INSTANCE* CQcreateInstance(int MAX_CQ_SIZE);

void CQdestroy(CQ_INSTANCE* queue);

int CQput(CQ_INSTANCE* queue, CQ_PTR item);

void* CQget(CQ_INSTANCE* queue);

#endif
