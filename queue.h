#ifndef QUEUE_H
#define QUEUE_H
#include <stdlib.h>
#include <stdbool.h>

// Job Pool Header
typedef struct JobQueueNode {
	int client_sock;
	struct JobQueueNode* next;
} JobQueueNode;

JobQueueNode* JobQueueNode_create();

typedef struct JobQueue {
	JobQueueNode* root;
	JobQueueNode* tail;
	void (*enqueue)(struct JobQueue* this, int client_sock);
	int (*dequeue)(struct JobQueue* this);
	bool (*empty)(struct JobQueue* this);
} JobQueue;

JobQueue* JobQueue_create();
void JobQueue_enqueue(JobQueue* this, int client_sock);
int JobQueue_dequeue(JobQueue* this);
void JobQueue_destroy(JobQueue* this);
JobQueueNode* JobQueue_destroy_helper(JobQueueNode* this);
bool JobQueue_empty(JobQueue* this);


#endif