// #define __CQ_LOG

#include <stdio.h>
#include "CircularQueue.h"

inline int getRemain(CQ_INSTANCE* queue) {
	return ( queue->head > queue->tail ?
				queue->head - queue->tail :
				( queue->end - queue->begin ) - ( queue->tail - queue->head ) ) - 1 ;
}

CQ_INSTANCE* CQcreateInstance(int MAX_CQ_SIZE) {
	CQ_INSTANCE* queue = malloc(sizeof(CQ_INSTANCE));

	queue->begin = malloc(sizeof(void*) * ( MAX_CQ_SIZE + 1 ));
	queue->end   = queue->begin + MAX_CQ_SIZE + 1;
	queue->head  = queue->begin;
	queue->tail  = queue->begin;
#ifdef __CQ_LOG
	printf("0x%08x : INT 0x%08x - 0x%08x\n", queue->begin, queue->begin, queue->end);
#endif
	pthread_mutex_init(&queue->mutex, NULL);
	return queue;
}

void CQdestroy(CQ_INSTANCE* queue) {
	pthread_mutex_destroy(&queue->mutex);
	queue->end   = NULL;
	queue->head  = NULL;
	queue->tail  = NULL;
	free(queue->begin);
	queue->begin = NULL;
}

int CQput(CQ_INSTANCE* queue, CQ_PTR item) {
	pthread_mutex_lock(&queue->mutex);
	if(getRemain(queue) == 0) {
		pthread_mutex_unlock(&queue->mutex);
		return -1;
	}

	*(queue->tail) = item;
	queue->tail++;
	if(queue->tail == queue->end) {
		queue->tail = queue->begin;
	}
#ifdef __CQ_LOG
	printf("0x%08x : PUT 0x%08x - 0x%08x\n", queue->begin, queue->head, queue->tail);
#endif
	pthread_mutex_unlock(&queue->mutex);
	return 0;
}

CQ_PTR CQget(CQ_INSTANCE* queue) {
	pthread_mutex_lock(&queue->mutex);

	if(queue->head == queue->tail) {
		pthread_mutex_unlock(&queue->mutex);
		return NULL;
	}

	CQ_PTR item = *(queue->head);
	queue->head++;
	if(queue->head == queue->end) {
		queue->head = queue->begin;
	}
#ifdef __CQ_LOG
	printf("0x%08x : GET 0x%08x - 0x%08x\n", queue->begin, queue->head, queue->tail);
#endif
	pthread_mutex_unlock(&queue->mutex);
	return item;
}
