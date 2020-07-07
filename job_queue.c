#include "queue.h"
#include <stdlib.h>

JobQueueNode* JobQueueNode_create()
{
	JobQueueNode* this = (JobQueueNode*)malloc(sizeof(JobQueueNode));
	this->client_sock = -1;
	this->next = NULL;
	return this;
}



JobQueue* JobQueue_create()
{
	JobQueue* this = (JobQueue*)malloc(sizeof(JobQueue));
	this->enqueue = JobQueue_enqueue;
	this->dequeue = JobQueue_dequeue;
	this->empty = JobQueue_empty;
	this->root = NULL;
	this->tail = NULL;
	return this;
}

void JobQueue_enqueue(JobQueue* this, int client_sock)
{
	if (this == NULL)
	{
		return;
	}
	if (this->root == NULL)
	{
		this->root = JobQueueNode_create();
		this->tail = this->root;
		this->tail->client_sock = client_sock;
	}
	else
	{
		this->tail->next = JobQueueNode_create();
		this->tail = this->tail->next;
		this->tail->client_sock = client_sock;
	}
}

int JobQueue_dequeue(JobQueue* this)
{
	if ((this == NULL) || (this->root == NULL))
	{
		return -1;
	}
	// If the there's only 1 item, tail must be updated
	if (this->root == this->tail)
	{
		this->tail = NULL;
	}
	JobQueueNode* temp = this->root->next;
	int client_sock = this->root->client_sock;
	free(this->root);
	this->root = temp;
	return client_sock;
}

void JobQueue_destroy(JobQueue* this)
{
	if (this == NULL)
	{
		return;
	}
	this->root = JobQueue_destroy_helper(this->root);
	this->tail = NULL;
	free(this);
}

JobQueueNode* JobQueue_destroy_helper(JobQueueNode* this)
{
	if (this == NULL)
	{
		return NULL;
	}
	this->next = JobQueue_destroy_helper(this->next);
	free(this);
	return NULL;
}

bool JobQueue_empty(JobQueue* this)
{
	if (this->root == NULL)
	{
		return true;
	}
	return false;
}
