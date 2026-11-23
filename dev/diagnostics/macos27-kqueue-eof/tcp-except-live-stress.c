// macOS 27 live TCP/kqueue saturation diagnostic; not a production SwiftNIO change.
// Public EVFILT_EXCEPT / NOTE_LOWAT only. No private EVFILT_SOCK symbols.
// One thread sends a SO_RCVBUF-sized bulk payload, then continuous single bytes;
// a separate thread simultaneously polls raw kqueue events without reading the peer.
#define _DARWIN_C_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { MAX_EVENTS = 12000 };
typedef struct {
    int sender;
    int receiver;
} Pair;
typedef struct {
    int sender;
    size_t bulk_target;
    atomic_bool stop;
    atomic_uint_fast64_t sent;
    atomic_int error;
} Producer;
typedef struct {
    uint64_t non_eof;
    uint64_t eof;
    uint64_t errors;
    int64_t first_non_eof_data;
    uint16_t first_non_eof_flags;
    int64_t max_non_eof_data;
    int max_queued;
    bool cap;
} Observation;

static void die(const char *what) {
    fprintf(stderr,"FATAL %s: %s, errno=%d\n",what,strerror(errno),errno);
    exit(2);
}
static int64_t clock_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) die("clock_gettime");
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void nap_ms(int ms) {
    struct timespec t={.tv_sec=ms/1000,.tv_nsec=(ms%1000)*1000000L};
    (void)nanosleep(&t,NULL);
}
static void make_nonblocking(int fd) {
    int flags=fcntl(fd,F_GETFL);
    if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) die("fcntl nonblocking");
}
static int queued(int fd) {
    int amount=0;
    if(ioctl(fd,FIONREAD,&amount)<0) die("ioctl FIONREAD");
    return amount;
}
static int rcv_capacity(int fd) {
    int amount=0; socklen_t length=sizeof(amount);
    if(getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&amount,&length)<0) die("getsockopt SO_RCVBUF");
    return amount;
}
static Pair connect_pair(int family, bool larger_buffer) {
    Pair p={.sender=-1,.receiver=-1};
    int listener=socket(family,SOCK_STREAM,0);
    if(listener<0) die("socket listener");
    struct sockaddr_storage dest;
    memset(&dest,0,sizeof(dest));
    socklen_t length=0;
    if(family==AF_INET) {
        struct sockaddr_in a={.sin_family=AF_INET,.sin_port=0};
        a.sin_len=(uint8_t)sizeof(a);
        a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        memcpy(&dest,&a,sizeof(a));
        length=sizeof(a);
    } else {
        struct sockaddr_in6 a={.sin6_family=AF_INET6,.sin6_port=0};
        a.sin6_len=(uint8_t)sizeof(a);
        a.sin6_addr=in6addr_loopback;
        memcpy(&dest,&a,sizeof(a));
        length=sizeof(a);
        int v6only=1;
        if(setsockopt(listener,IPPROTO_IPV6,IPV6_V6ONLY,&v6only,sizeof(v6only))<0)
            die("IPV6_V6ONLY");
    }
    if(bind(listener,(struct sockaddr*)&dest,length)<0) die("bind");
    if(listen(listener,1)<0) die("listen");
    if(getsockname(listener,(struct sockaddr*)&dest,&length)<0) die("getsockname");
    p.sender=socket(family,SOCK_STREAM,0);
    if(p.sender<0) die("socket sender");
    if(connect(p.sender,(struct sockaddr*)&dest,length)<0) die("connect");
    p.receiver=accept(listener,NULL,NULL);
    if(p.receiver<0) die("accept");
    if(close(listener)<0) die("close listener");
    if(larger_buffer) {
        int requested=512*1024;
        if(setsockopt(p.receiver,SOL_SOCKET,SO_RCVBUF,&requested,sizeof(requested))<0)
            die("SO_RCVBUF target");
    }
    make_nonblocking(p.sender);
    make_nonblocking(p.receiver);
    return p;
}
static int register_except(int receiver, bool edge) {
    int kq=kqueue();
    if(kq<0) die("kqueue");
    struct kevent change;
    EV_SET(&change,(uintptr_t)receiver,EVFILT_EXCEPT,
        (unsigned short)(EV_ADD|EV_ENABLE|(edge?EV_CLEAR:0)),
        NOTE_LOWAT,INTPTR_MAX,NULL);
    if(kevent(kq,&change,1,NULL,0,NULL)<0) die("register EXCEPT");
    return kq;
}
static void *produce(void *arg) {
    Producer *p=(Producer*)arg;
    unsigned char buf[8192];
    memset(buf,0x41,sizeof(buf));
    size_t bulk_left=p->bulk_target;
    while(!atomic_load_explicit(&p->stop,memory_order_relaxed)) {
        size_t request=bulk_left>0 ? (bulk_left<sizeof(buf) ? bulk_left:sizeof(buf)) : 1;
        ssize_t n=send(p->sender,buf,request,0);
        if(n>0) {
            atomic_fetch_add_explicit(&p->sent,(uint64_t)n,memory_order_relaxed);
            if(bulk_left>0) bulk_left-=(size_t)n;
        } else if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
            nap_ms(1);
        } else if(n<0 && errno==EINTR) {
            continue;
        } else {
            atomic_store_explicit(&p->error,n==0 ? EPIPE:errno,memory_order_relaxed);
            break;
        }
    }
    return NULL;
}
static void gather(int kq,int receiver,int duration_ms,Observation *o) {
    int64_t deadline=clock_ms()+duration_ms;
    while(clock_ms()<deadline) {
        int n=queued(receiver);
        if(n>o->max_queued) o->max_queued=n;
        if(o->non_eof+o->eof >= MAX_EVENTS) {
            o->cap=true;
            nap_ms(2);
            continue;
        }
        struct kevent ev[16];
        struct timespec timeout={.tv_sec=0,.tv_nsec=2000000L};
        int count=kevent(kq,NULL,0,ev,16,&timeout);
        if(count<0) {
            if(errno==EINTR) continue;
            die("kevent gather");
        }
        for(int i=0;i<count;i++) {
            if(ev[i].flags & EV_ERROR) o->errors++;
            if(ev[i].filter!=EVFILT_EXCEPT) continue;
            if(ev[i].flags & EV_EOF) {
                o->eof++;
            } else {
                if(o->non_eof==0) {
                    o->first_non_eof_data=ev[i].data;
                    o->first_non_eof_flags=ev[i].flags;
                }
                o->non_eof++;
                if(ev[i].data>o->max_non_eof_data) o->max_non_eof_data=ev[i].data;
            }
        }
    }
    int last=queued(receiver);
    if(last>o->max_queued)o->max_queued=last;
}
static void consume(int receiver,int kq,Observation *post,size_t *drained,bool *eof) {
    int64_t deadline=clock_ms()+2000;
    unsigned char buf[16384];
    while(clock_ms()<deadline && !*eof) {
        for(;;) {
            ssize_t n=recv(receiver,buf,sizeof(buf),0);
            if(n>0) {*drained+=(size_t)n;continue;}
            if(n==0) *eof=true;
            else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)
                die("drain recv");
            break;
        }
        if(!*eof)gather(kq,receiver,5,post);
    }
    gather(kq,receiver,40,post);
}
static void run(int family,bool larger,bool clear,int rep,int duration_ms) {
    Pair p=connect_pair(family,larger);
    int capacity=rcv_capacity(p.receiver);
    int kq=register_except(p.receiver,clear);
    Producer producer={.sender=p.sender,.bulk_target=(size_t)capacity};
    atomic_init(&producer.stop,false);
    atomic_init(&producer.sent,0);
    atomic_init(&producer.error,0);
    pthread_t thread;
    if(pthread_create(&thread,NULL,produce,&producer)!=0) die("pthread_create");

    Observation before={0},after={0},drain={0};
    // Kernel watches EXCEPT while the peer continuously sends, no receiver reads.
    gather(kq,p.receiver,duration_ms,&before);

    atomic_store_explicit(&producer.stop,true,memory_order_relaxed);
    int rc=pthread_join(thread,NULL);
    if(rc!=0) {errno=rc;die("pthread_join");}
    uint64_t total=atomic_load_explicit(&producer.sent,memory_order_relaxed);
    int producer_error=atomic_load_explicit(&producer.error,memory_order_relaxed);
    int end_queued=queued(p.receiver);

    // FIN is queued behind any unsent TCP data; EOF before draining is not required
    // when the receiver's window is exhausted.
    if(shutdown(p.sender,SHUT_WR)<0) die("shutdown SHUT_WR");
    gather(kq,p.receiver,220,&after);

    size_t drained=0; bool recv_eof=false;
    consume(p.receiver,kq,&drain,&drained,&recv_eof);

    const char *state="RECORDED";
    if(producer_error) state="SEND_ERROR";
    else if(!recv_eof) state="RECV_EOF_NOT_SEEN";
    else if(before.non_eof>0) state="SPURIOUS_EXCEPT";
    else if(before.max_queued<400000) state="BELOW_400K";
    printf("%s\t%s\t%s\t%d\t%d\t%d\t%d\t%" PRIu64 "\t%d\t%d\t%" PRIu64
        "\t%" PRIu64 "\t%" PRIu64 "\t%" PRId64 "\t%" PRId64 "\t%u\t%d"
        "\t%" PRIu64 "\t%" PRIu64 "\t%zu\t%d\t%d\t%s\n",
        clear?"EV_CLEAR":"LEVEL",
        family==AF_INET?"ipv4":"ipv6",
        larger?"large512K":"native",
        rep,duration_ms,capacity,before.max_queued,total,end_queued,
        before.cap?1:0,before.non_eof,before.eof,
        before.first_non_eof_data,before.max_non_eof_data,
        (unsigned)before.first_non_eof_flags,
        after.eof>0?1:0,drain.eof,drain.non_eof,
        drained,recv_eof?1:0,producer_error,state);
    fflush(stdout);
    if(close(kq)<0 || close(p.receiver)<0 || close(p.sender)<0) die("close sockets");
}
int main(int argc,char **argv) {
    int reps=3;int duration_ms=1600;
    if(argc>3) {fprintf(stderr,"usage: %s [repetitions 1..12] [duration_ms 100..5000]\n",argv[0]);return 2;}
    if(argc>1) reps=atoi(argv[1]);
    if(argc>2) duration_ms=atoi(argv[2]);
    if(reps<1 || reps>12 || duration_ms<100 || duration_ms>5000) {
        fprintf(stderr,"invalid run count or duration\n");return 2;
    }
    signal(SIGPIPE,SIG_IGN);
    puts("mode\tfamily\tbuffer\titeration\tpoll_ms\trecv_capacity\tmax_queued_during\tbytes_sent\tqueued_after_sender\tpre_event_cap\tpre_non_eof\tpre_eof\tfirst_non_eof_data\tmax_non_eof_data\tfirst_non_eof_flags\tpost_close_eof_seen\tdrain_eof_events\tdrain_non_eof_events\tdrained_bytes\trecv_eof\tproducer_errno\tstatus");
    for(int r=1;r<=reps;r++)
        for(int size=0;size<2;size++)
            for(int family=0;family<2;family++)
                for(int edge=0;edge<2;edge++)
                    run(family==0?AF_INET:AF_INET6,size!=0,edge!=0,r,duration_ms);
    return 0;
}
