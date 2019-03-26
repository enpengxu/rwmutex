#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>

//#define RW_CONDS
//#define RW_DEBUG

#ifdef RW_DEBUG
#define rwlog(...)				\
	printf("[ %x ]", (int)pthread_self());	\
	printf(__VA_ARGS__)
#else
#define rwlog(...)
#endif

#define LIST_INIT(head)				\
	(head)->next = (head);			\
	(head)->prev = (head)

#define LIST_INSERT(prv, cur)		\
	(cur)->next = (prv)->next;		\
	(cur)->prev = (prv);			\
	(prv)->next->prev = (cur);		\
	(prv)->next = (cur)

#define LIST_APPEND(head, cur)		\
	(cur)->prev = (head)->prev;		\
	(cur)->next = (head);			\
	(head)->prev->next = (cur);		\
	(head)->prev = (cur)

#define LIST_REMOVE(cur)			\
	(cur)->prev->next = (cur)->next;	\
	(cur)->next->prev = (cur)->prev

#define LIST_EMPTY(head)	((head)->next == (head))

#define rwlist_empty(rw)	\
	LIST_EMPTY(&(rw)->head) && (rw)->head.num == 0

#define rwlist_single_reader(rw)	\
	LIST_EMPTY(&(rw)->head) && \
	(rw)->head.type == 0 && (rw)->head.num > 0

#define RW_READER  0
#ifdef RW_CONDS
#define RW_WRITER  1
#else
#define RW_WRITER  0
#endif

struct rwitem {
	int type; // 0 : reader, 1: writer
	int num;
	unsigned seq;
	struct rwitem * prev;
	struct rwitem * next;
};

struct rwmutex {
	unsigned seq;
	struct rwitem head;
	pthread_mutex_t mutex;
	pthread_cond_t conds[2];  // use a cond pool to avoid thundering herd
};

static void
rwitem_dump(struct rwitem * item)
{
	rwlog("\trwitem: %p {.prev = %p, .next=%p, .type=%d, .num=%d }\n",
	      item, item->prev, item->next, item->type, item->num);
}

static void
rwmutex_dump(struct rwmutex* rw)
{
	struct rwitem * item = rw->head.next;
	rwlog("\trwmutex: %p {\n", rw);
	while(item != &rw->head) {
		rwitem_dump(item);
		item = item->next;
	}
	rwlog("\t}\n");
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

	rc = pthread_cond_init(&rw->conds[0], NULL);
	assert(rc == 0);
	if (rc) {
		return rc;
	}
	rc = pthread_cond_init(&rw->conds[1], NULL);
	assert(rc == 0);
	if (rc) {
		return rc;
	}
	rw->seq = 0;
	rw->head.seq = 0;
	rw->head.num = 0;
	return 0;
}

int
rwmutex_rlock(struct rwmutex * rw)
{
	int rc;
	struct rwitem item = {
		.type = 0,
	};
	rc = pthread_mutex_lock(&rw->mutex);
	assert(rc == 0);

	struct rwitem * reader, * tail;
	tail = rw->head.prev;

	rwlog("rlock enter:\n");

	if (rwlist_empty(rw)) {
		// it's safe to run reader now, passthrough it.
		reader = NULL;
		rw->head.num = 1;
		rw->head.type = 0;
		rw->head.seq = rw->seq;
	} else if (rwlist_single_reader(rw)) {
		reader = NULL;
		rw->head.num ++;
		rw->head.type = 0;
	} else if (!rwlist_empty(rw) && (tail->type == 0)) {
		// merge into the reader block
		reader = tail;
		assert(tail->num > 0);
		rwlog("\t rlock merge into tail %p : ", tail);
		rwitem_dump(tail);
	} else {
		reader = &item;
		reader->num = 0;
		reader->seq = rw->seq;
		LIST_APPEND(&rw->head, reader);
		rwlog("\t rlock insert reader: %p\n", reader);
	}

	rw->seq ++;

	if (reader) {
		reader->num ++;
		unsigned seq = reader->seq;
		while(seq != rw->head.seq) {
			// wait for writer to wake up reader
			pthread_cond_wait(&rw->conds[RW_READER], &rw->mutex);
		}
	}
	assert(rw->head.num > 0 && rw->head.type == 0);

	rwlog("rlock leav\n");
	rc = pthread_mutex_unlock(&rw->mutex);
	assert(rc == 0);

	return rc;

}

int
rwmutex_wlock(struct rwmutex * rw)
{
	int rc;
	struct rwitem item = {
		.num  = 1,
		.type = 1,
	};

	rc = pthread_mutex_lock(&rw->mutex);
	assert(rc == 0);

	rwlog("wlock enter:\n");

	struct rwitem * writer = NULL, * prev;

	prev = rw->head.prev;
	if (rwlist_empty(rw)) {
		assert(rw->head.next == &rw->head);
		rw->head.num = 1;
		rw->head.type = 1;
		rw->head.seq = rw->seq;
	} else {
		// create a new writer block & insert to tail
		//writer = rwitem_alloc(rw, 1);
		//assert(writer);
		writer = &item;
		LIST_APPEND(&rw->head, writer);
		writer->seq = rw->seq;
	}

	rw->seq ++;

	rwlog("\t wlock insert writer: %p\n", writer);

	if (writer) {
		while(writer->seq != rw->head.seq) {
			// wait for reader to wakeup writer
			pthread_cond_wait(&rw->conds[RW_WRITER], &rw->mutex);
		}
	}
	rwlog("wlock leav\n");

	// now remove writer ptr & move info to head
	assert(rw->head.num && rw->head.type == 1);
	
	rc = pthread_mutex_unlock(&rw->mutex);
	assert(rc == 0);

	return rc;
}

int
rwmutex_unlock(struct rwmutex * rw)
{
	int rc = 0;
	rc = pthread_mutex_lock(&rw->mutex);
	assert(rc == 0);

	rwlog("unlock enter\n");

	struct rwitem * head = &rw->head;

	rwlog("\t unlock: item : %p \n", head);
	rwitem_dump(head);

	assert(head->type == 0 || head->type == 1);
	assert(head->num > 0);

	if (head->type == 1) {
		assert(head->num == 1);
	}

	if (!--head->num) {
		struct rwitem * next = head->next;
		if (next != head) {
			// wakeup next
			assert(next->num > 0);
			assert(next->type == 0 || next->type == 1);

			rwlog("\t unlock: wakeup next: %p \n", head->next);
			rwitem_dump(next);

			head->type = next->type;
			head->seq  = next->seq;
			head->num  = next->num;

			LIST_REMOVE(next);

			if (next->type == 0) {
				// next is reader, so wakup reader.
				pthread_cond_broadcast(&rw->conds[RW_READER]);
			} else {
				// next is writer, so wakup writer
				pthread_cond_broadcast(&rw->conds[RW_WRITER]);
			}
		} else {
			head->type = -1;
			head->seq  = 0;
			head->num  = 0;
			assert(head->prev == head && head->next == head);
		}
	}
	rwlog("unlock leave\n");
	rc = pthread_mutex_unlock(&rw->mutex);
	assert(rc == 0);
	return rc;
}

// test codes
// 0 : regular mutex
// 1 : pthread_rwlock
// 2 : rwmutex

#define RW_LOCK  2

#if RW_LOCK == 0
  static struct rwmutex rw;
  #define RW_INIT(rw)    rwmutex_init(rw)
  #define RW_RLOCK(rw)   pthread_mutex_lock(&(rw)->mutex)
  #define RW_WLOCK(rw)   pthread_mutex_lock(&(rw)->mutex)
  #define RW_UNLOCK(rw)  pthread_mutex_unlock(&(rw)->mutex);
#elif RW_LOCK == 1
  static pthread_rwlock_t rw;
#define RW_INIT(rw)    pthread_rwlock_init(rw, NULL)
  #define RW_RLOCK(rw)   pthread_rwlock_rdlock(rw)
  #define RW_WLOCK(rw)   pthread_rwlock_wrlock(rw)
  #define RW_UNLOCK(rw)  pthread_rwlock_unlock(rw)
#else
  static struct rwmutex rw;
  #define RW_INIT(rw)    rwmutex_init(rw)
  #define RW_RLOCK(rw)   rwmutex_rlock(rw)
  #define RW_WLOCK(rw)   rwmutex_wlock(rw)
  #define RW_UNLOCK(rw)  rwmutex_unlock(rw)
#endif

// sleep from 0 to 200us
#define rw_delay()   usleep(random()%200)

#define NUM_READER  150
#define NUM_WRITER  15

#define val_orig    50000
static unsigned val = val_orig;
static pthread_t treaders[NUM_READER];
static pthread_t twriters[NUM_WRITER];

static unsigned writer_info[NUM_WRITER] = { 0 };
static unsigned reader_info[NUM_READER] = { 0 };


static void *
thread_reader(void *arg)
{
	int abort = 0;
	int idx = (int)(uintptr_t)arg;

	while(!abort) {
		int rc = RW_RLOCK(&rw);
		assert(rc == 0);

		reader_info[idx] ++;

		rw_delay();

		if (val == 0) {
			abort = 1;
		}
		rc = RW_UNLOCK(&rw);
		assert(rc == 0);
	}

	return NULL;
}

static void *
thread_writer(void *arg)
{
	static int count = 0;
	int abort = 0;
	int idx = (int)(uintptr_t)arg;

	while(!abort) {
		int rc = RW_WLOCK(&rw);
		assert(rc == 0);

		writer_info[idx] ++;

		rw_delay();
		//usleep(random()%10);

		if (!val)
			abort = 1;
		else {
			val --;
		}
		if (++count == 500) {
			//fprintf(stderr, "val = %d / %d \n", val, val_orig);
			count = 0;
		}

		rc = RW_UNLOCK(&rw);
		assert(rc == 0);
	}

	return NULL;
}

int
main(void) {
	int i, rc = RW_INIT(&rw);
	assert(rc == 0);

	for(i =0; i< NUM_READER; i++) {
		rc = pthread_create(&treaders[i], NULL, thread_reader, (void *)(uintptr_t)i);
		assert(rc == 0);
	}

	for(i =0; i< NUM_WRITER; i++) {
		rc = pthread_create(&twriters[i], NULL, thread_writer, (void *)(uintptr_t)i);
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
