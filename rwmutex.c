#include <pthread.h>
#include <stdio.h>
#include <assert.h>

#define LIST_INIT(head)			\
	(head)->next = (head);			\
	(head)->prev = (head)

#define LIST_INSERT(prv, cur)			\
	(cur)->prev = (prv);			\
	(cur)->next = (prv)->prev;		\
	(prv)->next = (cur)

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
struct rwmuex {
	struct rwitem head;
	unsigned time;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

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
	pthread_mutex_lock(&rw->mutex);
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
		LIST_ADD(&rw->head.prev, reader);
	}

	reader->num ++;
	rw->time ++;

	if (reader->prev != &rw.head) {
		// there are some writer/readers ahead, reader has to wait
		struct rwitem * writer = reader->prev;
		while(writer->num) {
			pthread_cond_wait(&rw->cond, &rw->mutex);
		}
		// remove prev
		LIST_REMOVE(writer);
		rwitem_free(writer);
	}
	pthread_mutex_unlock(&rw->mutex);

	return 0;

}

int
rwmutex_wlock(struct rwmutex * rw)
{
	pthread_mutex_lock(&rw->mutex);

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
		LIST_ADD(&rw->head.prev, writer);
	}

	writer->num ++;
	rw->time ++;

	if (writer->prev != &rw.head) {
		// there are some readers ahead, writer has to wait
		struct rwitem * reader = writer->prev;
		while(reader->num) {
			pthread_cond_wait(&rw->cond, &rw->mutex);
		}
		// remove prev
		LIST_REMOVE(reader);
		rwitem_free(reader);
	}
	pthread_mutex_unlock(&rw->mutex);

	return 0;

}

int
rwmutex_unlock(struct rwmutex * rw)
{
	int rc = 0;
	pthread_mutex_lock(&rw->mutex);
	struct rwitem * item = rw->head.next;

	if (item == &rw->head) {
		rc = -1;
	} else {
		if (!--item->num && item->next != &rw->head) {
			// wakeup next item
			pthread_cond_broadcast(&rw->cond, &rw->mutex);
		}
	}
	pthread_mutex_unlock(&rw->mutex);
	return rc;
}


static struct rwmutex rw;
static pthread_t treaders[128];
static pthread_t twriters[6];

int
main(void) {
	int rc = rwmutex_init(&rw);
	assert(rc == 0);

	
	pthread_create(
	
}
