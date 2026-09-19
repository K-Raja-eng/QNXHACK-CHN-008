/*
 * Minimal RFC 6455 WebSocket server for the reservoir dashboard.
 * Uses TCP sockets + pthread accept loop + shared-memory protected read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <math.h>
#include "../include/reservoir_state.h"

#define PORT 9000
#define MAX_CLIENTS 8
#define BUF_SIZE 8192
static volatile int running = 1;
static int clients[MAX_CLIENTS];
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static ShmHandle shm = { .fd = -1 };
static ReservoirTwinState *twin = NULL;

static void signal_handler(int sig) { (void)sig; running = 0; }

/* Small SHA-1 implementation for the browser handshake. */
typedef struct { uint32_t h[5]; uint64_t bits; uint8_t block[64]; size_t used; } SHA1;
static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static void sha1_init(SHA1 *s) { s->h[0]=0x67452301; s->h[1]=0xEFCDAB89; s->h[2]=0x98BADCFE; s->h[3]=0x10325476; s->h[4]=0xC3D2E1F0; s->bits=0; s->used=0; }
static void sha1_block(SHA1 *s, const uint8_t *p) {
    uint32_t w[80];
    for (int i=0;i<16;i++) w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (int i=16;i<80;i++) w[i]=rol32(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4];
    for (int i=0;i<80;i++) { uint32_t f,k; if(i<20){f=(b&c)|((~b)&d);k=0x5A827999;} else if(i<40){f=b^c^d;k=0x6ED9EBA1;} else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;} else {f=b^c^d;k=0xCA62C1D6;} uint32_t t=rol32(a,5)+f+e+k+w[i]; e=d; d=c; c=rol32(b,30); b=a; a=t; }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}
static void sha1_update(SHA1 *s,const uint8_t*p,size_t n){s->bits+=(uint64_t)n*8ULL;while(n){size_t k=64-s->used;if(k>n)k=n;memcpy(s->block+s->used,p,k);s->used+=k;p+=k;n-=k;if(s->used==64){sha1_block(s,s->block);s->used=0;}}}
static void sha1_final(SHA1*s,uint8_t out[20]){uint64_t bits=s->bits;uint8_t one=0x80,z=0;sha1_update(s,&one,1);while(s->used!=56)sha1_update(s,&z,1);uint8_t len[8];for(int i=0;i<8;i++)len[7-i]=(uint8_t)(bits>>(8*i));sha1_update(s,len,8);for(int i=0;i<5;i++){out[i*4]=(uint8_t)(s->h[i]>>24);out[i*4+1]=(uint8_t)(s->h[i]>>16);out[i*4+2]=(uint8_t)(s->h[i]>>8);out[i*4+3]=(uint8_t)s->h[i];}}
static const char b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const uint8_t*in,size_t n,char*out,size_t cap){size_t j=0;for(size_t i=0;i<n;i+=3){uint32_t v=(uint32_t)in[i]<<16;if(i+1<n)v|=(uint32_t)in[i+1]<<8;if(i+2<n)v|=in[i+2];if(j+4>=cap)break;out[j++]=b64[(v>>18)&63];out[j++]=b64[(v>>12)&63];out[j++]=(i+1<n)?b64[(v>>6)&63]:'=';out[j++]=(i+2<n)?b64[v&63]:'=';}out[j]='\0';}
static int send_all(int fd,const void*data,size_t n){const char*p=(const char*)data;while(n){ssize_t w=send(fd,p,n,0);if(w<=0)return -1;p+=w;n-=(size_t)w;}return 0;}

static int handshake(int fd)
{
    char req[4096]; size_t used=0;
    while(used<sizeof(req)-1){ssize_t n=recv(fd,req+used,sizeof(req)-1-used,0);if(n<=0)return -1;used+=(size_t)n;req[used]='\0';if(strstr(req,"\r\n\r\n"))break;}
    const char *needle="Sec-WebSocket-Key:"; char *p=strstr(req,needle); if(!p)return -1; p+=strlen(needle); while(*p==' '||*p=='\t')p++; char key[256]; size_t k=0; while(*p&&*p!='\r'&&*p!='\n'&&k<sizeof(key)-1)key[k++]=*p++; key[k]='\0'; if(k==0)return -1;
    char material[320]; snprintf(material,sizeof(material),"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
    SHA1 s; uint8_t digest[20]; char accept[64]; sha1_init(&s); sha1_update(&s,(const uint8_t*)material,strlen(material)); sha1_final(&s,digest); b64_encode(digest,20,accept,sizeof(accept));
    char resp[512]; int len=snprintf(resp,sizeof(resp),"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\nAccess-Control-Allow-Origin: *\r\n\r\n",accept);
    return send_all(fd,resp,(size_t)len);
}

static int ws_send_text(int fd,const char *data,size_t n)
{
    uint8_t h[10]; size_t hs;
    if(n<126){h[0]=0x81;h[1]=(uint8_t)n;hs=2;} else if(n<=65535){h[0]=0x81;h[1]=126;h[2]=(uint8_t)(n>>8);h[3]=(uint8_t)n;hs=4;} else {h[0]=0x81;h[1]=127;for(int i=0;i<8;i++)h[2+i]=(uint8_t)(n>>(56-8*i));hs=10;}
    if(send_all(fd,h,hs)<0)return -1; return send_all(fd,data,n);
}

static void add_client(int fd)
{
    pthread_mutex_lock(&clients_lock);
    for(int i=0;i<MAX_CLIENTS;i++) if(clients[i]<0){clients[i]=fd;pthread_mutex_unlock(&clients_lock);printf("[WS] client fd=%d connected\n",fd);fflush(stdout);return;}
    pthread_mutex_unlock(&clients_lock);close(fd);
}

static void *accept_loop(void *arg)
{
    int server=*(int*)arg;
    while(running){struct sockaddr_in ca;socklen_t al=sizeof(ca);int fd=accept(server,(struct sockaddr*)&ca,&al);if(fd<0){if(running)perror("accept");continue;}struct timeval tv={3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));if(handshake(fd)==0)add_client(fd);else close(fd);}return NULL;
}

static double finite_or(double v, double fallback)
{
    return isfinite(v) ? v : fallback;
}

static int json_build(const ReservoirTwinPayload *s, char *b, size_t z)
{
    return snprintf(b,z,
        "{\"seq\":%llu,\"level_m\":%.2f,\"storage_pct\":%.1f,\"rain_mmph\":%.1f,\"gate_pct\":%.1f,\"downstream_m\":%.2f,\"temp_c\":%.1f,\"arduino_mic1\":%.0f,\"arduino_mic2\":%.0f,\"arduino_gas\":%.0f,\"arduino_ir\":%.0f,\"arduino_humidity\":%.1f,\"arduino_aux_valid\":%u,\"upstream_inflow_m3s\":%.1f,\"upstream_release_m3s\":%.1f,\"rain_1h\":%.1f,\"rain_6h\":%.1f,\"rain_12h\":%.1f,\"rain_24h\":%.1f,\"rain_72h\":%.1f,\"forecast_confidence\":%.1f,\"predicted_inflow_m3s\":%.1f,\"predicted_level_m\":%.2f,\"predicted_storage_pct\":%.1f,\"prototype_release_m3s\":%.1f,\"flood_risk\":%u,\"advisory\":%u,\"sources\":%u,\"stale\":%u,\"health\":%u,\"fast\":%llu,\"med\":%llu,\"slow\":%llu,\"jitter_us\":%.2f,\"max_jitter_us\":%.2f,\"deadline_misses\":%llu}\n",
        (unsigned long long)s->sequence,
        finite_or(s->reservoir_level_m, 6.0), finite_or(s->storage_percent, 40.0),
        finite_or(s->rainfall_mmph, 0.0), finite_or(s->gate_percent, 0.0),
        finite_or(s->downstream_level_m, 0.0), finite_or(s->temperature_c, 28.0),
        finite_or(s->arduino_mic1, 0.0), finite_or(s->arduino_mic2, 0.0),
        finite_or(s->arduino_gas, 0.0), finite_or(s->arduino_ir, 0.0),
        finite_or(s->arduino_humidity, 0.0), (unsigned)s->arduino_aux_valid,
        finite_or(s->upstream_inflow_m3s, 0.0), finite_or(s->upstream_release_m3s, 0.0),
        finite_or(s->rain_1h_mm, 0.0), finite_or(s->rain_6h_mm, 0.0),
        finite_or(s->rain_12h_mm, 0.0), finite_or(s->rain_24h_mm, 0.0),
        finite_or(s->rain_72h_mm, 0.0), finite_or(s->weather_confidence_percent, 0.0),
        finite_or(s->predicted_inflow_m3s, 0.0), finite_or(s->predicted_level_m, 0.0),
        finite_or(s->predicted_storage_percent, 0.0), finite_or(s->recommended_release_m3s, 0.0),
        (unsigned)s->flood_risk_level, (unsigned)s->release_advisory,
        (unsigned)s->source_flags, (unsigned)s->stale_flags, (unsigned)s->health,
        (unsigned long long)s->fast_count, (unsigned long long)s->med_count,
        (unsigned long long)s->slow_count, finite_or(s->last_jitter_ns/1000.0, 0.0),
        finite_or(s->max_jitter_ns/1000.0, 0.0),
        (unsigned long long)s->deadline_miss_count);
}

int main(void)
{
    signal(SIGINT,signal_handler); signal(SIGTERM,signal_handler); signal(SIGPIPE,SIG_IGN);
    while(twin_open(&shm,&twin)!=0){fprintf(stderr,"[WS] waiting for /reservoir_twin...\n");sleep(1);}
    for(int i=0;i<MAX_CLIENTS;i++)clients[i]=-1;
    int server=socket(AF_INET,SOCK_STREAM,0);if(server<0){perror("socket");return 1;}
    int one=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons(PORT);
    if(bind(server,(struct sockaddr*)&a,sizeof(a))<0){perror("bind");return 1;}if(listen(server,MAX_CLIENTS)<0){perror("listen");return 1;}
    pthread_t accept_thread; pthread_create(&accept_thread,NULL,accept_loop,&server);
    printf("[WS] WebSocket ws://<pi-ip>:%d\n",PORT);
    char json[BUF_SIZE];
    while(running){
        ReservoirTwinPayload snap;
        twin_snapshot(twin, &snap);
        int n=json_build(&snap,json,sizeof(json));
        if(n>0){pthread_mutex_lock(&clients_lock);for(int i=0;i<MAX_CLIENTS;i++){if(clients[i]<0)continue;if(ws_send_text(clients[i],json,(size_t)n)<0){close(clients[i]);clients[i]=-1;}}pthread_mutex_unlock(&clients_lock);}usleep(100000);
    }
    close(server); pthread_join(accept_thread,NULL); pthread_mutex_lock(&clients_lock);for(int i=0;i<MAX_CLIENTS;i++)if(clients[i]>=0){close(clients[i]);clients[i]=-1;}pthread_mutex_unlock(&clients_lock);shm_close(&shm);return 0;
}
