#include <stdio.h>
#include <stdlib.h>
#include "so_scheduler.h"

typedef struct thread {
	unsigned int priority;
	pthread_t tid;
	pthread_mutex_t mutex_pl;
	unsigned int time_quantum;
	struct thread *urm;
} ThreadInfo;

typedef struct {
	unsigned int time_quantum;
	unsigned int io;
} scheduler;

scheduler *sch;
ThreadInfo **readyQueue;
ThreadInfo **blockingQueue;
ThreadInfo *running;
ThreadInfo *terminated;
ThreadInfo *last_thread;

pthread_key_t *myThread;

void insertQueueLast(ThreadInfo *ti)
{
	ThreadInfo **tl = &readyQueue[ti->priority];

	//pun ti la finalul listei
	for (; *tl != NULL; tl = &((*tl)->urm))
		;
	ti->urm = *tl;
	*tl = ti;
}

void insertQueueFirst(ThreadInfo *ti)
{
	ThreadInfo **tl = &readyQueue[ti->priority];

	//pun ti la inceputul listei
	ti->urm = *tl;
	*tl = ti;
}

ThreadInfo *extractQueue(void)
{
	ThreadInfo *p;
	int i, ok = 0;

	//extrag primul element din lista cu prio cea mai mare
	for (i = SO_MAX_PRIO; i >= 0; i--) {
		if (readyQueue[i] != NULL) {
			ok = 1;
			break;
		}
	}
	//daca nu gasesc nimic (READY e gol) intorc NULL
	if (ok == 0)
		return NULL;
	p = readyQueue[i];
	readyQueue[i] = readyQueue[i]->urm;
	return p;
}

void reschedule(void)
{
	ThreadInfo *thread = pthread_getspecific(*myThread), *extr;

	thread->time_quantum--;
	if (thread->time_quantum > 0) {
		//daca a venit un thread cu prioritate mai mare
		ThreadInfo *extr = extractQueue();

		if (extr != NULL) {
			if (running->priority < extr->priority) {
				ThreadInfo *t = running;

				running = extr;
				insertQueueLast(t);
			} else {
				insertQueueFirst(extr);
			}
			pthread_mutex_unlock(&running->mutex_pl);
			pthread_mutex_lock(&thread->mutex_pl);
		}
		return;
	}

	//daca thread-ului curent i-a expirat cuanta
	thread->time_quantum = sch->time_quantum;

	//apare in coada READY o prioritate mai mare
	extr = extractQueue();
	if (extr != NULL) {
		if (running->priority <= extr->priority) {
			ThreadInfo *t = running;

			running = extr;
			insertQueueLast(t);
		} else {
			insertQueueFirst(extr);
		}
	}
	//este planificat noul thread din running
	pthread_mutex_unlock(&running->mutex_pl);
	//se asteapta ca thread-ul curent sa fie planificat
	pthread_mutex_lock(&thread->mutex_pl);
}


int so_init(unsigned int time_quantum, unsigned int io)
{
	ThreadInfo *ti;

	if (sch != NULL)
		return -1;
	sch = malloc(sizeof(scheduler));
	if (!sch)
		return -1;
	sch->time_quantum = time_quantum;
	if (time_quantum == 0 || io > SO_MAX_NUM_EVENTS) {
		free(sch);
		sch = NULL;
		return -1;
	}
	sch->io = io;

	ti = malloc(sizeof(ThreadInfo));
	if (ti == NULL) {
		free(sch);
		sch = NULL;
	}
	ti->priority = 0;
	ti->tid = pthread_self();
	ti->urm = NULL;
	ti->time_quantum = sch->time_quantum;
	pthread_mutex_init(&ti->mutex_pl, NULL);

	running = ti;

	myThread = malloc(sizeof(pthread_key_t));
	pthread_key_create(myThread, NULL);
	pthread_setspecific(*myThread, ti);

	readyQueue = calloc(1 + SO_MAX_PRIO, sizeof(ThreadInfo *));
	blockingQueue = calloc(io, sizeof(ThreadInfo *));
	reschedule();
	return 0;
}

void destroy_thread(ThreadInfo *thread)
{
	pthread_mutex_destroy(&thread->mutex_pl);
	free(thread);
}

void destroy_scheduler(scheduler *sch)
{
	free(sch);
}

void so_end(void)
{
	ThreadInfo *thread, *t;

	if (sch == NULL)
		return;

	//last_thread asteapta ca READY sa se goleasca
	thread = pthread_getspecific(*myThread);
	last_thread = thread;

	running = extractQueue();
	if (running != NULL) {
		pthread_mutex_unlock(&running->mutex_pl);
		pthread_mutex_lock(&thread->mutex_pl);
	}

	t = terminated;
	while (t != NULL) {
		ThreadInfo *p = t;

		t = t->urm;
		pthread_join(p->tid, NULL);
		destroy_thread(p);
	}

	destroy_thread(thread);

	pthread_key_delete(*myThread);
	free(myThread);

	destroy_scheduler(sch);
	free(readyQueue);
	free(blockingQueue);
	sch = NULL;
}

void so_exec(void)
{
	ThreadInfo *thread = pthread_getspecific(*myThread);

	pthread_mutex_unlock(&thread->mutex_pl);
	pthread_mutex_lock(&thread->mutex_pl);
	reschedule();
}

int so_wait(unsigned int io)
{
	ThreadInfo *thread = pthread_getspecific(*myThread);

	pthread_mutex_unlock(&thread->mutex_pl);
	pthread_mutex_lock(&thread->mutex_pl);

	if (io < 0 || io >= sch->io)
		return -1;

	//inserare thread in blockingQueue[io]
	thread->urm = blockingQueue[io];
	blockingQueue[io] = thread;
	
	//se blocheaza thread-ul curent si se pune
	//altul in starea running
	running = extractQueue();
	pthread_mutex_unlock(&running->mutex_pl);

	pthread_mutex_lock(&thread->mutex_pl);
	return 0;
}

typedef struct {
	so_handler *func;
	unsigned int priority;
	ThreadInfo *t;
} param;

void *start_thread(void *arg)
{
	//asteapta planificare
	param *p = (param *)arg;

	pthread_setspecific(*myThread, p->t);

	ThreadInfo *thread = pthread_getspecific(*myThread);
	
	//asteapta sa fie planificat
	pthread_mutex_lock(&thread->mutex_pl);
	p->func(p->priority);

	free(p);

	running = extractQueue();

	thread->urm = terminated;
	terminated = thread;

	if (running != NULL) {
		pthread_mutex_unlock(&running->mutex_pl);
	} else {
		//se poate reactiva thread-ul care a apelat so_end
		pthread_mutex_unlock(&last_thread->mutex_pl);
	}
	return NULL;
}

tid_t so_fork(so_handler *func, unsigned int priority)
{

	ThreadInfo *thread = pthread_getspecific(*myThread);
	param *p;

	pthread_mutex_unlock(&thread->mutex_pl);
	if (func == NULL || priority > SO_MAX_PRIO)
		return INVALID_TID;
	ThreadInfo *t = malloc(sizeof(ThreadInfo));

	if (!t)
		return INVALID_TID;
	t->priority = priority;
	t->time_quantum = sch->time_quantum;
	pthread_mutex_init(&t->mutex_pl, NULL);
	pthread_mutex_lock(&t->mutex_pl);

	p = malloc(sizeof(param));
	p->func = func;
	p->priority = priority;
	p->t = t;

	pthread_create(&t->tid, NULL, start_thread, p);

	//pun thread-ul in READY
	insertQueueLast(t);

	pthread_mutex_lock(&thread->mutex_pl);
	reschedule();
	return t->tid;
}

int so_signal(unsigned int io)
{
	ThreadInfo *thread = pthread_getspecific(*myThread);
	int nr = 0;

	pthread_mutex_unlock(&thread->mutex_pl);

	if (io < 0 || io >= sch->io)
		return -1;
	ThreadInfo *p = blockingQueue[io];

	while (p != NULL) {
		ThreadInfo *aux = p;

		p = p->urm;
		insertQueueLast(aux);
		nr++;
	}
	blockingQueue[io] = NULL;

	pthread_mutex_lock(&thread->mutex_pl);
	reschedule();
	return nr;
}
