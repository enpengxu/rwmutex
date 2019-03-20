#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

#define LIST_INIT(head)			\
	(head)->next = (head);			\
	(head)->prev = (head)

#define LIST_INSERT(prv, cur)			\
	(prv)->next->prev = (cur);		\
	(cur)->next = (prv)->next;		\
	(prv)->next = (cur);			\
	(cur)->prev = (prv)

#define LIST_REMOVE(cur)			\
	(cur)->prev->next = (cur)->next;	\
	(cur)->next->prev = (cur)->prev


struct rwitem {
	unsigned type; // 0 : reader, 1: writer
	unsigned num;
	unsigned range[2];

	struct cond_mutex * cm;

	struct rwitem * prev;
	struct rwitem * next;
};

struct cond_mutex {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};
struct rwmutex {
	struct rwitem head;
	unsigned time;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

static void
rwitem_free(struct rwitem * item)
{
	free(item);
}

int
rwmutex_init(struct rwmutex * rw)
{
	int rc = 0;
	LIST_INIT(&rw->head);
	rc = pthread_mutex_init(&rw->mutex, NULL);
	assert(rc == 0);
	if (rc) {
		return rc;
	}
	rc = pthread_cond_init(&rw->cond, NULL);
	assert(rc == 0);
	if (rc) {
		return rc;
	}
	rw->time = 0;
	return 0;
}

int
rwmutex_rlock(struct rwmutex * rw)
{
	int rc;
	rc = pthread_mutex_lock(&rw->mutex);
	assert(rc == 0);
	// put into
	struct rwitem * reader, * tail;
	tail = rw->head.prev ;
	if (tail != &rw->head && tail->type == 0) {
		// merge into a reader block
		tail->range[1] = rw->time;
		reader = tail;
	} else {
		// create a new reader block & insert to tail
		reader = malloc(sizeof(*reader));
		reader->num = 0;
		reader->type = 0;
		reader->range[0] = reader->range[1] = rw->time;
		LIST_INSERT(rw->head.prev, reader);
	}

	reader->num ++;
	rw->time ++;

	if (reader->prev != &rw->head) {
		// there are some writer/readers ahead, reader has to wait
		struct rwitem * writer = reader->prev;
		while(writer->num) {
			pthread_cond_wait(&rw->cond, &rw->mutex);
		}
		// remove prev
		LIST_REMOVE(writer);
		rwitem_free(writer);
	}
	rc = pthread_mutex_unlock(&rw->mutex);
	assert(rc == 0);

	return 0;

}

int
rwmutex_wlock(struct rwmutex * rw)
{
	int rc;
	rc = pthread_mutex_lock(&rw->mutex);
	assert(rc == 0);

	struct rwitem * writer, * tail;
	tail = rw->head.prev ;
	if (tail != &rw->head && tail->type == 1) {
		// merge into a reader block
		tail->range[1] = rw->time;
		writer = tail;
	} else {
		// create a new writer block & insert to tail
		writer = malloc(sizeof(*writer));
		writer->num = 0;
		writer->type = 1;
		writer->range[0] = writer->range[1] = rw->time;
		LIST_INSERT(rw->head.prev, writer);
	}

	writer->num ++;
	rw->time ++;

	if (writer->prev != &rw->head) {
		// there are some readers ahead, writer has to wait
		struct rwitem * reader = writer->prev;
		while(reader->num) {
			pthread_cond_wait(&rw->cond, &rw->mutex);
		}
		// remove prev
		LIST_REMOVE(reader);
		rwitem_free(reader);
	}
	rc = pthread_mutex_unlock(&rw->mutex);
	assert(rc == 0);

	return 0;

}

int
rwmutex_unlock(struct rwmutex * rw)
{
	int rc = 0;
	rc = pthread_mutex_lock(&rw->mutex);
	assert(rc == 0);

	struct rwitem * item = rw->head.next;

	if (item == &rw->head) {
		rc = -1;
	} else {
		if (!--item->num) {
			if (item->next != &rw->head) {
				// wakeup next item
				pthread_cond_broadcast(&rw->cond);
			} else {
				// nothing left, free itself
				LIST_REMOVE(item);
				rwitem_free(item);
			}
		}
	}
	pthread_mutex_unlock(&rw->mutex);
	return rc;
}


#define NUM_READER   12
#define NUM_WRITER   1

static unsigned val = 10000;
static struct rwmutex rw;
static pthread_t treaders[NUM_READER];
static pthread_t twriters[NUM_WRITER];

static unsigned writer_info[NUM_WRITER] = { 0 };
static unsigned reader_info[NUM_READER] = { 0 };


static void *
thread_reader(void *arg)
{
	int abort = 0;
	int idx = (int)arg;

	while(!abort) {
		int rc = rwmutex_rlock(&rw);
		assert(rc == 0);

		reader_info[idx] ++;

		usleep(1);

		if (val == 0) {
			abort = 1;
		}
		rc = rwmutex_unlock(&rw);
		assert(rc == 0);
	}

	return NULL;
}

static void *
thread_writer(void *arg)
{
	int abort = 0;
	int idx = (int)arg;

	while(!abort) {
		int rc = rwmutex_wlock(&rw);
		assert(rc == 0);

		writer_info[idx] ++;

		usleep(1);

		if (!val)
			abort = 1;
		else {
			val --;
		}

		rc = rwmutex_unlock(&rw);
		assert(rc == 0);
	}

	return NULL;
}

int
main(void) {
	int i, rc = rwmutex_init(&rw);
	assert(rc == 0);

	for(i =0; i< NUM_READER; i++) {
		rc = pthread_create(&treaders[i], NULL, thread_reader, (void *)i);
		assert(rc == 0);
	}

	for(i =0; i< NUM_WRITER; i++) {
		rc = pthread_create(&twriters[i], NULL, thread_writer, (void *)i);
		assert(rc == 0);
	}


	for(i =0; i< NUM_WRITER; i++) {
		rc = pthread_join(twriters[i], NULL);
		assert(rc == 0);
		fprintf(stderr, "writer[%d] : %d\n", i, writer_info[i]);
	}

	for(i =0; i< NUM_READER; i++) {
		rc = pthread_join(treaders[i], NULL);
		assert(rc == 0);
		fprintf(stderr, "reader[%d] : %d\n", i, reader_info[i]);
	}

	return 0;
}
