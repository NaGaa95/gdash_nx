#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pthr.h"

static int checks;
static atomic_int native_joins, forbidden_detaches, fail_next_create, tls_blocks, worker_error;
static _Thread_local void *host_tls;
typedef struct { size_t bytes; max_align_t align; } Allocation;
static void *tracked_malloc(size_t bytes) {
  Allocation *a=malloc(sizeof(*a)+bytes);if(!a)abort();a->bytes=bytes;
  if(bytes==512)atomic_fetch_add(&tls_blocks,1);return a+1;
}
static void *tracked_calloc(size_t count,size_t bytes) {
  void *p=tracked_malloc(count*bytes);memset(p,0,count*bytes);return p;
}
static void tracked_free(void *p) {
  if(!p)return;Allocation *a=(Allocation *)p-1;
  if(a->bytes==512)atomic_fetch_sub(&tls_blocks,1);free(a);
}
static int sdk_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *v) {
  if(atomic_exchange(&fail_next_create,0))return EAGAIN;
  return pthread_create(t,a,f,v);
}
static int sdk_join(pthread_t t,void **v) {
  int ret=pthread_join(t,v);if(!ret)atomic_fetch_add(&native_joins,1);return ret;
}
static int sdk_detach(pthread_t t) {
  (void)t;atomic_fetch_add(&forbidden_detaches,1);return ENOSYS;
}
void pthr_install_fake_tls(void) { host_tls=tracked_calloc(1,512); }
static void *armGetTlsRw(void) { return host_tls; }
static void assign_core_bg(void) { }
#define malloc tracked_malloc
#define calloc tracked_calloc
#define free tracked_free
#define pthread_create sdk_create
#define pthread_join sdk_join
#define pthread_detach sdk_detach
/* PRODUCTION_THREADS */
#undef pthread_create
#undef pthread_join
#undef pthread_detach
#undef malloc
#undef calloc
#undef free
#define CHECK(condition) do { checks++;if(!(condition)) { \
  fprintf(stderr,"line %d: %s\n",__LINE__,#condition);return 1; \
} } while(0)
static void pause_briefly(void) { struct timespec t={0,1000000};nanosleep(&t,NULL); }
static int remaining_threads(void) {
  pthread_mutex_lock(&thread_lock);int count=0;
  for(ThreadStart *s=threads;s;s=s->next)count++;
  pthread_mutex_unlock(&thread_lock);return count;
}
static int wait_remaining(int expected) {
  for(int n=0;n<5000;n++) {
    if(remaining_threads()==expected && atomic_load(&tls_blocks)==expected)return 1;
    pause_briefly();
  }
  return 0;
}
static int wait_finished(pthread_t t) {
  for(int n=0;n<5000;n++) {
    pthread_mutex_lock(&thread_lock);ThreadStart *s=find_thread(t);int ready=s && s->finished;
    pthread_mutex_unlock(&thread_lock);if(ready)return 1;pause_briefly();
  }
  return 0;
}
static atomic_int release_blocked;
static void *blocked_worker(void *value) {
  while(!atomic_load(&release_blocked))pause_briefly();return value;
}
static void *worker(void *value) { if(!host_tls)atomic_store(&worker_error,1);return value; }
static void *self_detached_worker(void *value) {
  if(pthread_detach_soloader(pthread_self()))atomic_store(&worker_error,1);return value;
}
static void *exiting_worker(void *value) { pthread_exit_soloader(value); }
int main(void) {
  pthread_t t,other;void *value=NULL;int marker=123;
  CHECK(pthread_create_soloader(NULL,NULL,worker,NULL)==EINVAL);
  CHECK(pthread_create_soloader(&t,NULL,NULL,NULL)==EINVAL);
  CHECK(pthread_join_soloader((pthread_t)0,NULL)==ESRCH);
  CHECK(pthread_join_soloader(pthread_self(),NULL)==EDEADLK);
  CHECK(pthread_detach_soloader((pthread_t)0)==ESRCH);
  CHECK(pthread_create_soloader(&t,NULL,worker,&marker)==0);
  CHECK(pthread_join_soloader(t,&value)==0 && value==&marker);
  CHECK(wait_remaining(0) && atomic_load(&native_joins)==1);
  CHECK(pthread_create_soloader(&t,NULL,blocked_worker,&marker)==0);
  atomic_store(&fail_next_create,1);
  CHECK(pthread_detach_soloader(t)==EAGAIN); // Failed reaper creation keeps the worker joinable.
  CHECK(pthread_detach_soloader(t)==0);
  CHECK(pthread_detach_soloader(t)==EINVAL);
  CHECK(pthread_join_soloader(t,NULL)==EINVAL);
  CHECK(pthread_create_soloader(&other,NULL,worker,&marker)==0);
  CHECK(pthread_detach_soloader(other)==0);
  CHECK(wait_remaining(1)); // Finished worker reclaimed while the first detached worker is still running.
  CHECK(atomic_load(&native_joins)==2);
  atomic_store(&release_blocked,1);CHECK(wait_remaining(0));
  CHECK(pthread_create_soloader(&t,NULL,worker,&marker)==0);
  CHECK(wait_finished(t));CHECK(pthread_detach_soloader(t)==0);CHECK(wait_remaining(0));
  pthread_attr_t_bionic attr={0};
  CHECK(pthread_attr_init_soloader(&attr)==0);
  CHECK(pthread_attr_setdetachstate_soloader(&attr,2)==EINVAL);
  CHECK(pthread_attr_setdetachstate_soloader(&attr,1)==0);
  CHECK(pthread_create_soloader(&t,&attr,worker,&marker)==0);CHECK(wait_remaining(0));
  CHECK(pthread_attr_destroy_soloader(&attr)==0);
  CHECK(pthread_create_soloader(&t,NULL,self_detached_worker,&marker)==0);CHECK(wait_remaining(0));
  CHECK(pthread_create_soloader(&t,NULL,exiting_worker,&marker)==0);
  CHECK(pthread_join_soloader(t,&value)==0 && value==&marker);CHECK(wait_remaining(0));
  CHECK(pthread_create_soloader(&t,NULL,exiting_worker,&marker)==0);
  CHECK(pthread_detach_soloader(t)==0);CHECK(wait_remaining(0));
  atomic_store(&fail_next_create,1);
  CHECK(pthread_create_soloader(&t,NULL,worker,&marker)==EAGAIN);CHECK(wait_remaining(0));
  for(int n=0;n<32;n++) {
    CHECK(pthread_create_soloader(&t,NULL,worker,&marker)==0);
    CHECK(pthread_detach_soloader(t)==0);
  }
  CHECK(wait_remaining(0));
  CHECK(atomic_load(&worker_error)==0);
  CHECK(atomic_load(&native_joins)==40);
  CHECK(atomic_load(&forbidden_detaches)==0); // Never call the SDK ENOSYS stub.
  printf("PASS: detached save workers and thread reclamation (%d checks)\n",checks);return 0;
}
