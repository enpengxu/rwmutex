#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>

#define LIST_INIT(head)				\
	(head)->next = (head);			\
	(head)->prev = (head)

#define LIST_INSERT(prv, cur)			\
	(cur)->next = (prv)->next;		\
	(cur)->prev = (prv);			\
	(prv)->next->prev = (cur);		\
	(prv)->next = (cur)

#define LIST_APPEND(head, cur)			\
	(cur)->prev = (head)->prev;		\
	(cur)->next = (head);			\
	(head)->prev->next = (cur);		\
	(head)->prev = (cur)

#define LIST_REMOVE(cur)			\
	(cur)->prev->next = (cur)->next;	\
	(cur)->next->prev = (cur)->prev


#ifdef RW_DEBUG
  #define rwlog(...)  printf("[ %x ]", (int)pthread_self());	\
	printf(__VA_ARGS__)
#else
  #define rwlog(...)  
#endif

struct rwitem {
	int type; // 0 : reader, 1: writer
	int num;

	struct rwitem * prev;
	struct rwitem * next;
};

struct rwmutex {
	int time;
	struct rwitem head;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

struct rwitem *
rwitem_alloc(struct rwmutex * rw, int type)
{
	struct rwitem * item = malloc(sizeof(*item));
	if (!item) {
		assert(0 && "no memory");
		return NULL;
	}
	item->type = type;
	item->num = 0;
	return item;
}

static void
rwitem_free(struct rwitem * item)
{
	free(item);
}

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

	struct rwitem * reader, * tail;
	tail = rw->head.prev;

	rwlog("rlock enter:\n");

	if (tail != &rw->head && tail->type == 0) {
		// merge into the reader block
		reader = tail;

		rwlog("\t rlock merge into tail %p : ", tail);
		rwitem_dump(tail);

	} else {
		// create a new reader block & insert to tail
		reader = rwitem_alloc(rw, 0);
		assert(reader);
		LIST_APPEND(&rw->head, reader);
		rwlog("\t rlock insert reader: %p\n", reader);
	}

	reader->num ++;
	rw->time ++;

	while(reader->prev != &rw->head) {
		pthread_cond_wait(&rw->cond, &rw->mutex);
	}

	assert((reader == rw->head.next) && (reader->prev == &rw->head));
	
	rwlog("rlock leav\n");

	rc = pthread_mutex_unlock(&rw->mutex);
	assert(rc == 0);

	return rc;

}

int
rwmutex_wlock(struct rwmutex * rw)
{
	int rc;
	rc = pthread_mutex_lock(&rw->mutex);
	assert(rc == 0);

	rwlog("wlock enter:\n");

	struct rwitem * writer, * prev;

	// create a new writer block & insert to tail
	writer = rwitem_alloc(rw, 1);
	assert(writer);
	LIST_APPEND(&rw->head, writer);

	rwlog("\t wlock insert writer: %p\n", writer);

	writer->num ++;
	rw->time ++;

	while(writer->prev != &rw->head) {
		pthread_cond_wait(&rw->cond, &rw->mutex);
	}

	rwlog("wlock leav\n");

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

	struct rwitem * item = rw->head.next;

	if (item == &rw->head) {
		assert(0 && "rwmutex_unlock: invalid unlock");
		rc = -1;
	} else {
		rwlog("\t unlock: item : %p \n", item);
		rwitem_dump(item);

		assert(item->prev == &rw->head);
		assert(rw->head.next == item);
		assert(item->num > 0);

		if (!--item->num) {
			if (item->next != &rw->head) {
				rwlog("\t unlock: wakeup next: %p \n", item->next);
				rwitem_dump(item->next);
				// wakeup next item
				pthread_cond_broadcast(&rw->cond);
			}
			LIST_REMOVE(item);
			rwitem_free(item);
		}
	}
	rwlog("unlock leave\n");
	pthread_mutex_unlock(&rw->mutex);
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

// sleep from 0 to 5ms
#define rw_delay()   usleep(random()%3000)

#define NUM_READER   50
#define NUM_WRITER   5

#define val_orig 4000
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
		if (++count == 20) {
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
