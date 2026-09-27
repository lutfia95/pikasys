/*
 * BUILD: make; sudo make install; pikasys
 * Requires a C11 compiler, make, and the OS development headers. macOS:
 * xcode-select --install. Linux/WSL: your distribution's C build tools.
 * No curses, Python, GPU SDK, shell subprocess, or elevated privileges needed.
 * NVIDIA's installed driver supplies optional libnvidia-ml.so.1 at runtime.
 *
 * SUPPORT:
 * Linux/WSL: procfs CPU/memory/process/network, sysfs sensors, physical-disk I/O.
 * NVIDIA: dynamically loaded NVML; AMD: sysfs + DRM fdinfo; Intel: sysfs +
 * DRM fdinfo (including Xe cycle counters). Other DRM devices: discovery and
 * compatible fdinfo fields. Missing permissions/metrics remain unavailable.
 * macOS: Mach CPU/memory, libproc processes/I/O, getifaddrs network, IOKit GPU.
 * macOS GPU PerformanceStatistics keys are undocumented, best effort, and
 * may change. No reliable system-wide per-process GPU attribution on macOS
 * is claimed. Apple GPU memory is shared, never presented as dedicated VRAM.
 * WSL metrics describe what the guest exposes, not all Windows host activity.
 * CPU process % uses 100% PER LOGICAL CPU, so multithreaded work can exceed 100%.
 * DRM process GPU % is the busiest normalized engine; shared DRM clients are
 * assigned to the first accessible PID to avoid double counting. It is not
 * a device-wide utilization estimate. Resident buffers can be shared by clients.
 *
 * Interface references:
 * https://docs.nvidia.com/deploy/nvml-api/
 * https://docs.kernel.org/gpu/drm-usage-stats.html
 * https://docs.kernel.org/gpu/amdgpu/thermal.html
 * https://developer.apple.com/documentation/kernel/1502854-host_processor_info
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <dlfcn.h>
#include <glob.h>
#include <sys/sysmacros.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <ifaddrs.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#else
#error "pikasys supports Linux, WSL and macOS"
#endif

#define VERSION "0.1.0"
#define MAX_GPU 64
#define MAX_CPU 8192
#define HISTORY 240
#define MAX_ENGINE 32
/* All monitored values are non-negative, so -1 safely represents a reading
 * that is unsupported, inaccessible, or not available until another sample. */
#define NA (-1.0)
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef struct { double values[HISTORY]; size_t pos, count; } History;
typedef struct { uint64_t total, idle; double usage; bool valid; } Cpu;
typedef struct {
    int pid, ppid, threads; uint64_t birth, ticks, rss, read_bytes, write_bytes;
    uid_t uid; char state, name[96], user[32], command[512];
    double cpu, gpu, gpu_mem, read_rate, write_rate;
    uint64_t gpu_mask;
} Process;
typedef struct {
    char id[128], name[160], backend[24], path[512];
    double busy, mem_busy, mem_used, mem_total, temp, watts, limit, fan, rpm;
    double clock, mem_clock, encoder, decoder;
    bool shared, seen, proc_supported;
    History history; void *handle;
    uint64_t last_stamp;
#ifdef __APPLE__
    io_registry_entry_t service;
#endif
} Gpu;
typedef struct {
    Cpu *cpus; size_t ncpu;
    double cpu, mem_used, mem_total, swap_used, swap_total, uptime, load[3];
    double cpu_temp, cpu_mhz, cpu_power, rx, tx, disk_read, disk_write;
    double fs_total, fs_used, now, elapsed;
    uint64_t prev_rx, prev_tx, prev_read, prev_write;
    bool network_valid, disk_valid, wsl;
    char hostname[128], platform[128], cpu_name[192];
    Process *procs, *old; size_t nproc, cap, nold;
    Gpu gpus[MAX_GPU]; size_t ngpu;
    History cpu_hist, mem_hist, rx_hist, tx_hist;
    unsigned denied; char gpu_status[192];
} Monitor;
enum { BG, FG, MUTED, ACCENT, GOOD, WARN, BAD, SELECT_BG, NROLE };
typedef struct { const char *name; unsigned rgb[NROLE]; } Theme;
static const Theme themes[] = {
    {"pikachu", {0x12151c,0xe8ecf3,0x8993a5,0xffd449,0x80d4a0,0xffb35c,0xff6b81,0x333a4d}},
    {"dracula", {0x282a36,0xf8f8f2,0x9da5c4,0xbd93f9,0x50fa7b,0xffb86c,0xff5555,0x44475a}},
    {"nord", {0x242c3a,0xe5e9f0,0x92a2bb,0x88c0d0,0xa3be8c,0xebcb8b,0xbf616a,0x3b4252}},
    {"ocean", {0x071a25,0xdaf1f4,0x799da9,0x39cce6,0x59dfab,0xf2bd74,0xff7e88,0x16394a}},
    {"light", {0xf3f5f8,0x202939,0x5b6779,0x805600,0x137943,0x9c5900,0xc0263d,0xd7e0ee}}
};
enum { SORT_CPU, SORT_MEM, SORT_GPU, SORT_PID, SORT_NAME, NSORT };
static const char *sort_names[] = {"CPU", "RAM", "GPU", "PID", "NAME"};
typedef struct {
    double interval; int theme, sort, view, gpu_index, core_page;
    bool color, ascii, json, gpu_only, reverse, no_gpu, no_config;
    bool paused, help, details, filtering; long count;
    size_t selection, scroll; char filter[128], config[PATH_MAX], notice[192];
    unsigned custom[NROLE]; bool custom_set[NROLE];
} Options;
static Options opt = {.interval=1.0, .color=true, .count=-1};
static volatile sig_atomic_t stopping = 0;
static struct termios saved_term;
static bool terminal_active;

static double monotime(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void restore_terminal(void) {
    if (terminal_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_term);
        fputs("\033[0m\033[?25h\033[?1049l", stdout); fflush(stdout);
        terminal_active = false;
    }
}
static void fail(const char *s) {
    restore_terminal(); fprintf(stderr, "pikasys: %s\n", s); exit(EXIT_FAILURE);
}
static void *resize(void *p, size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) fail("allocation overflow");
    void *q = realloc(p, n * size); if (!q && n) fail("out of memory"); return q;
}
static void copystr(char *to, size_t n, const char *from) {
    if (!n) return;
    if (!from) from="";
    size_t length=strlen(from);if(length>=n)length=n-1;
    memmove(to,from,length);to[length]=0;
}
static void cleanstr(char *s) {
    /* Strip control bytes so process/device names cannot inject terminal escapes. */
    for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s == 127) *s = ' ';
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    return s;
}
#ifdef __linux__
static bool read_text(const char *path, char *buf, size_t n) {
    if (!n) return false;
    FILE *f = fopen(path, "r"); if (!f) { buf[0]=0; return false; }
    size_t got = fread(buf, 1, n-1, f); bool ok = !ferror(f);
    fclose(f); buf[got]=0; return ok && got > 0;
}
static double read_number(const char *path) {
    char b[128], *end; if (!read_text(path,b,sizeof b)) return NA;
    errno=0; double v=strtod(b,&end); return end!=b && !errno && isfinite(v) && v>=0 ? v : NA;
}
#endif
static double clamp(double v, double lo, double hi) { return v<lo ? lo : v>hi ? hi : v; }
static double rate(uint64_t a, uint64_t b, double dt) { return dt>0 && a>=b ? (double)(a-b)/dt : NA; }
static double percent(uint64_t total, uint64_t idle, uint64_t oldtotal, uint64_t oldidle) {
    if (total<=oldtotal || idle<oldidle) return NA;
    return clamp(100.0 * (1.0-(double)(idle-oldidle)/(double)(total-oldtotal)),0,100);
}
static void push(History *h, double v) { h->values[h->pos]=v; h->pos=(h->pos+1)%HISTORY; if (h->count<HISTORY) h->count++; }
static double hist_at(const History *h, size_t back) { return back<h->count ? h->values[(h->pos+HISTORY-1-back)%HISTORY] : NA; }
static void bytes_text(double n, char *s, size_t z) {
    static const char *units[]={"B","KiB","MiB","GiB","TiB","PiB"};
    if (n<0 || !isfinite(n)) { copystr(s,z,"N/A"); return; }
    size_t u=0; while(n>=1024 && u<ARRAY_LEN(units)-1) {n/=1024;u++;}
    snprintf(s,z,n>=100 ? "%.0f %s" : "%.1f %s",n,units[u]);
}
static void metric(double v, const char *unit, char *s, size_t n) {
    if(v<0 || !isfinite(v)) copystr(s,n,"N/A"); else snprintf(s,n,"%.1f%s",v,unit);
}
static bool contains(const char *hay, const char *needle) {
    if (!*needle) return true;
    for (; *hay; hay++) if (!strncasecmp(hay,needle,strlen(needle))) return true;
    return false;
}
static uint32_t decode_utf8(const char **input) {
    const unsigned char *s=(const unsigned char*)*input;
    unsigned n=s[0]>=0xf0 && s[0]<=0xf4?4:s[0]>=0xe0 && s[0]<=0xef?3:s[0]>=0xc2 && s[0]<=0xdf?2:0;
    if(!n){(*input)++;return 0xfffd;}
    uint32_t value=s[0]&((1u<<(7-n))-1u);
    for(unsigned i=1;i<n;i++){if((s[i]&0xc0)!=0x80){(*input)++;return 0xfffd;}value=(value<<6)|(s[i]&63);}
    if((n==2 && value<0x80) || (n==3 && value<0x800) || (n==4 && value<0x10000) || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) {(*input)++;return 0xfffd;}
    *input+=n;return value;
}
static Process *add_process(Monitor *m) {
    if(m->nproc==m->cap) {m->cap=m->cap ? m->cap*2 : 512; m->procs=resize(m->procs,m->cap,sizeof(Process));}
    Process *p=&m->procs[m->nproc++]; memset(p,0,sizeof *p);
    /* Rate and GPU fields need a previous sample or backend attribution.
     * Unknown is different from a measured zero, so initialize them to NA. */
    p->cpu=p->gpu=p->gpu_mem=p->read_rate=p->write_rate=NA; return p;
}
static int pid_cmp(const void *a,const void *b) {
    int x=((const Process*)a)->pid,y=((const Process*)b)->pid; return (x>y)-(x<y);
}
static Process *find_process(Process *p,size_t n,int pid) {
    Process key={.pid=pid}; return n ? bsearch(&key,p,n,sizeof *p,pid_cmp) : NULL;
}
static void finish_process(Monitor *m, Process *p, double ticks_per_second) {
    Process *old=find_process(m->old,m->nold,p->pid);
    if(old && old->birth==p->birth) {
        p->cpu=rate(p->ticks,old->ticks,m->elapsed);
        if(p->cpu>=0) p->cpu=100*p->cpu/ticks_per_second;
        if(p->read_bytes!=UINT64_MAX && old->read_bytes!=UINT64_MAX) p->read_rate=rate(p->read_bytes,old->read_bytes,m->elapsed);
        if(p->write_bytes!=UINT64_MAX && old->write_bytes!=UINT64_MAX) p->write_rate=rate(p->write_bytes,old->write_bytes,m->elapsed);
        if(old->uid==p->uid) copystr(p->user,sizeof p->user,old->user);
    }
    if(!p->user[0]) {
        /* Cache usernames by uid; passwd lookup once per distinct account. */
        static struct {uid_t uid; char name[32];} users[256]; static size_t nu;
        for(size_t i=0;i<nu;i++) if(users[i].uid==p->uid) {copystr(p->user,sizeof p->user,users[i].name);break;}
        if(!p->user[0]) {
            struct passwd pw,*result=NULL; char buf[16384];
            if(!getpwuid_r(p->uid,&pw,buf,sizeof buf,&result) && result) copystr(p->user,sizeof p->user,pw.pw_name);
            else snprintf(p->user,sizeof p->user,"%u",(unsigned)p->uid);
            if(nu<ARRAY_LEN(users)) {users[nu].uid=p->uid;copystr(users[nu++].name,32,p->user);}
        }
    }
    cleanstr(p->name); cleanstr(p->command); cleanstr(p->user);
}
static void gpu_reset(Gpu *g) {
    g->busy=g->mem_busy=g->mem_used=g->mem_total=g->temp=g->watts=g->limit=NA;
    g->fan=g->rpm=g->clock=g->mem_clock=g->encoder=g->decoder=NA;
    g->seen=false; g->proc_supported=false;
}
static Gpu *gpu_slot(Monitor *m, const char *id) {
    for(size_t i=0;i<m->ngpu;i++) if(!strcmp(m->gpus[i].id,id)) {m->gpus[i].seen=true; return &m->gpus[i];}
    if(m->ngpu==MAX_GPU) return NULL;
    Gpu *g=&m->gpus[m->ngpu++]; memset(g,0,sizeof *g); gpu_reset(g);
    copystr(g->id,sizeof g->id,id); g->seen=true; return g;
}
static void gpu_process(Process *p,size_t idx,double memory,double util) {
    if(!p || idx>=64) return;
    p->gpu_mask |= UINT64_C(1)<<idx;
    if(memory>=0) p->gpu_mem=(p->gpu_mem<0?0:p->gpu_mem)+memory;
    if(util>=0) p->gpu=(p->gpu<0?0:p->gpu)+util;
}

#ifdef __linux__
static bool parse_proc_stat(char *text, Process *p) {
    char *left=strchr(text,'('), *right=strrchr(text,')');
    if(!left || !right || right<=left || right[1]!=' ') return false;
    *left=0; char *end; long pid=strtol(text,&end,10);
    if(pid<=0 || pid>INT_MAX) return false;
    p->pid=(int)pid; *right=0; copystr(p->name,sizeof p->name,left+1);
    char *save=NULL,*tok=strtok_r(right+2," \t\n",&save); unsigned field=3;
    uint64_t user=0,sys=0; bool gotrss=false;
    while(tok) {
        if(field==3) p->state=*tok;
        else if(field==4) p->ppid=(int)strtol(tok,NULL,10);
        else if(field==14) user=strtoull(tok,NULL,10);
        else if(field==15) sys=strtoull(tok,NULL,10);
        else if(field==20) p->threads=(int)strtol(tok,NULL,10);
        else if(field==22) p->birth=strtoull(tok,NULL,10);
        else if(field==24) {
            long long pages=strtoll(tok,NULL,10); long page_size=sysconf(_SC_PAGESIZE);
            p->rss=pages>0 && page_size>0 ? (uint64_t)pages*(uint64_t)page_size : 0;
            gotrss=true; break;
        }
        field++; tok=strtok_r(NULL," \t\n",&save);
    }
    p->ticks=user+sys; return gotrss;
}
static void linux_processes(Monitor *m) {
    DIR *dir=opendir("/proc"); if(!dir) return;
    struct dirent *de; long hz=sysconf(_SC_CLK_TCK);
    while((de=readdir(dir))) {
        if(!isdigit((unsigned char)de->d_name[0])) continue;
        char path[PATH_MAX],buf[8192]; struct stat st;
        snprintf(path,sizeof path,"/proc/%s",de->d_name);
        if(stat(path,&st)) continue;
        snprintf(path,sizeof path,"/proc/%s/stat",de->d_name);
        if(!read_text(path,buf,sizeof buf)) {if(errno==EACCES || errno==EPERM)m->denied++;continue;}
        Process temp={0}; if(!parse_proc_stat(buf,&temp)) continue;
        Process *p=add_process(m); *p=temp; p->uid=st.st_uid;
        p->cpu=p->gpu=p->gpu_mem=p->read_rate=p->write_rate=NA;
        p->read_bytes=p->write_bytes=UINT64_MAX;
        snprintf(path,sizeof path,"/proc/%d/cmdline",p->pid);
        FILE *f=fopen(path,"r");
        if(f) {size_t n=fread(p->command,1,sizeof(p->command)-1,f);fclose(f);
            for(size_t i=0;i<n;i++) if(!p->command[i])p->command[i]=' ';
            p->command[n]=0;
        }
        if(!p->command[0]) copystr(p->command,sizeof p->command,p->name);
        snprintf(path,sizeof path,"/proc/%d/io",p->pid);
        f=fopen(path,"r");
        if(f) {char line[256];while(fgets(line,sizeof line,f)) {
            uint64_t value;
            if(sscanf(line,"read_bytes: %"SCNu64,&value)==1)p->read_bytes=value;
            if(sscanf(line,"write_bytes: %"SCNu64,&value)==1)p->write_bytes=value;
        } fclose(f);}
        finish_process(m,p,hz>0 ? (double)hz : 100.0);
    }
    closedir(dir);
}
static void update_cpu(Cpu *c,uint64_t total,uint64_t idle) {
    c->usage=c->valid ? percent(total,idle,c->total,c->idle) : NA;
    c->total=total;c->idle=idle;c->valid=true;
}
static void linux_cpu(Monitor *m) {
    FILE *f=fopen("/proc/stat","r"); char line[1024]; static Cpu aggregate;
    m->cpu=NA;
    if(f) {size_t highest=0;while(fgets(line,sizeof line,f)) {
        if(strncmp(line,"cpu",3)) break;
        char label[32]; uint64_t a=0,b=0,c=0,d=0,e=0,g=0,h=0,j=0;
        int n=sscanf(line,"%31s %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64,
                     label,&a,&b,&c,&d,&e,&g,&h,&j);
        if(n<5) continue;
        uint64_t total=a+b+c+d+e+g+h+j, idle=d+e;
        if(!strcmp(label,"cpu")) {update_cpu(&aggregate,total,idle);m->cpu=aggregate.usage;}
        else {char *end;long id=strtol(label+3,&end,10); if(*end || id<0 || id>=MAX_CPU)continue;
            size_t count=(size_t)id+1;
            if(count>m->ncpu) {size_t old=m->ncpu;m->cpus=resize(m->cpus,count,sizeof(Cpu));
                memset(m->cpus+old,0,(count-old)*sizeof(Cpu));m->ncpu=count;}
            update_cpu(&m->cpus[id],total,idle); if(count>highest)highest=count;
        }
    } fclose(f); (void)highest;}
    m->cpu_mhz=NA; f=fopen("/proc/cpuinfo","r");
    if(f) {double sum=0;size_t n=0;while(fgets(line,sizeof line,f)) {
        char *colon=strchr(line,':');if(!colon)continue;*colon=0;
        if(!strcmp(trim(line),"cpu MHz")) {double v=strtod(colon+1,NULL);if(v>0){sum+=v;n++;}}
        if(!m->cpu_name[0] && (!strcmp(trim(line),"model name") || !strcmp(trim(line),"Hardware"))) copystr(m->cpu_name,sizeof m->cpu_name,trim(colon+1));
    }fclose(f);if(n)m->cpu_mhz=sum/(double)n;}
    char text[128]; if(read_text("/proc/uptime",text,sizeof text))m->uptime=strtod(text,NULL);
    if(getloadavg(m->load,3)!=3)m->load[0]=m->load[1]=m->load[2]=NA;
}
static void linux_memory(Monitor *m) {
    FILE *f=fopen("/proc/meminfo","r"); if(!f)return;
    char line[256]; double total=NA,avail=NA,freev=0,buffers=0,cached=0,reclaim=0,shmem=0,swtotal=0,swfree=0;
    while(fgets(line,sizeof line,f)) {char key[64];double v;if(sscanf(line,"%63s %lf",key,&v)!=2)continue;
        v*=1024;
        if(!strcmp(key,"MemTotal:"))total=v;else if(!strcmp(key,"MemAvailable:"))avail=v;
        else if(!strcmp(key,"MemFree:"))freev=v;else if(!strcmp(key,"Buffers:"))buffers=v;
        else if(!strcmp(key,"Cached:"))cached=v;else if(!strcmp(key,"SReclaimable:"))reclaim=v;
        else if(!strcmp(key,"Shmem:"))shmem=v;else if(!strcmp(key,"SwapTotal:"))swtotal=v;
        else if(!strcmp(key,"SwapFree:"))swfree=v;
    }fclose(f);
    if(total>=0){if(avail<0)avail=freev+buffers+cached+reclaim-shmem;
        m->mem_total=total;m->mem_used=clamp(total-avail,0,total);}
    m->swap_total=swtotal;m->swap_used=clamp(swtotal-swfree,0,swtotal);
}
static void linux_network(Monitor *m) {
    FILE *f=fopen("/proc/net/dev","r");if(!f)return;
    char line[1024];uint64_t rx=0,tx=0;
    while(fgets(line,sizeof line,f)) {
        char *p=strchr(line,':');if(!p)continue;*p=0;if(!strcmp(trim(line),"lo"))continue;
        uint64_t r,t;
        if(sscanf(p+1,"%"SCNu64" %*u %*u %*u %*u %*u %*u %*u %"SCNu64,&r,&t)==2){rx+=r;tx+=t;}
    }fclose(f);
    m->rx=m->network_valid ? rate(rx,m->prev_rx,m->elapsed):NA;
    m->tx=m->network_valid ? rate(tx,m->prev_tx,m->elapsed):NA;
    m->prev_rx=rx;m->prev_tx=tx;m->network_valid=true;
}
static void linux_disks(Monitor *m) {
    DIR *dir=opendir("/sys/block");if(!dir)return;struct dirent *de;
    uint64_t reads=0,writes=0;bool found=false;
    while((de=readdir(dir))) {
        if(de->d_name[0]=='.' || !strncmp(de->d_name,"loop",4) || !strncmp(de->d_name,"ram",3) ||
            !strncmp(de->d_name,"zram",4) || !strncmp(de->d_name,"dm-",3) || !strncmp(de->d_name,"md",2))continue;
        char path[PATH_MAX],buf[1024];snprintf(path,sizeof path,"/sys/block/%s/stat",de->d_name);
        if(!read_text(path,buf,sizeof buf))continue;
        uint64_t r,w;
        if(sscanf(buf,"%*u %*u %"SCNu64" %*u %*u %*u %"SCNu64,&r,&w)==2){reads+=r*512;writes+=w*512;found=true;}
    }closedir(dir);
    m->disk_read=found && m->disk_valid ? rate(reads,m->prev_read,m->elapsed):NA;
    m->disk_write=found && m->disk_valid ? rate(writes,m->prev_write,m->elapsed):NA;
    m->prev_read=reads;m->prev_write=writes;m->disk_valid=found;
}
static void linux_sensors(Monitor *m) {
    /* hwmon and RAPL are optional kernel interfaces. In particular, WSL often
     * does not forward these host sensors, so leave the fields as NA when the
     * files are absent instead of reporting a misleading zero. */
    m->cpu_temp=m->cpu_power=NA;glob_t gl={0};
    if(!glob("/sys/class/hwmon/hwmon*/name",0,NULL,&gl)) {
        for(size_t i=0;i<gl.gl_pathc;i++) {
            char name[128],base[512];if(!read_text(gl.gl_pathv[i],name,sizeof name))continue;
            if(!contains(name,"coretemp") && !contains(name,"k10temp") && !contains(name,"zenpower") && !contains(name,"cpu_thermal"))continue;
            copystr(base,sizeof base,gl.gl_pathv[i]);char *slash=strrchr(base,'/');if(slash)*slash=0;
            for(int k=1;k<=128;k++) {char path[PATH_MAX];snprintf(path,sizeof path,"%s/temp%d_input",base,k);
                double v=read_number(path);if(v>=0 && v<200000 && v/1000>m->cpu_temp)m->cpu_temp=v/1000;}
        }
    }globfree(&gl);
    static struct {char path[PATH_MAX];double energy,time;} power[64];static size_t npower;
    memset(&gl,0,sizeof gl);
    if(!glob("/sys/class/powercap/intel-rapl:*/energy_uj",0,NULL,&gl)) {
        double watts=0;bool valid=false;
        for(size_t i=0;i<gl.gl_pathc;i++) {
            const char *base=strstr(gl.gl_pathv[i],"intel-rapl:");if(!base || strchr(base+11,':'))continue;
            double energy=read_number(gl.gl_pathv[i]);if(energy<0)continue;
            size_t j;for(j=0;j<npower;j++)if(!strcmp(power[j].path,gl.gl_pathv[i]))break;
            if(j==npower){if(npower==ARRAY_LEN(power))continue;copystr(power[npower++].path,PATH_MAX,gl.gl_pathv[i]);}
            if(power[j].time>0 && m->now>power[j].time) {
                double delta=energy-power[j].energy;
                if(delta<0) {char path[PATH_MAX];copystr(path,sizeof path,gl.gl_pathv[i]);char *s=strrchr(path,'/');if(s)*s=0;
                    size_t len=strlen(path);snprintf(path+len,sizeof(path)-len,"/max_energy_range_uj");double range=read_number(path);if(range>0)delta+=range;}
                if(delta>=0){watts+=delta/1e6/(m->now-power[j].time);valid=true;}
            }
            power[j].energy=energy;power[j].time=m->now;
        }if(valid)m->cpu_power=watts;
    }globfree(&gl);
}

/* NVML ABI declarations: use specifically versioned process-v2 entry points.
 * Their four-field layout is distinct from the newer process-v3 ABI. */
typedef void *NvDevice;
typedef struct { unsigned gpu,memory; } NvUtil;
typedef struct { unsigned long long total,free,used; } NvMemory;
typedef struct { unsigned pid; unsigned long long used; unsigned gi,ci; } NvProcess2;
typedef struct { unsigned pid; unsigned long long stamp; unsigned sm,mem,enc,dec; } NvSample;
static struct {
    void *lib;bool ready;
    int (*init)(void),(*shutdown)(void),(*count)(unsigned*),(*device)(unsigned,NvDevice*);
    int (*name)(NvDevice,char*,unsigned),(*uuid)(NvDevice,char*,unsigned);
    int (*util)(NvDevice,NvUtil*),(*memory)(NvDevice,NvMemory*);
    int (*temp)(NvDevice,unsigned,unsigned*),(*power)(NvDevice,unsigned*),(*limit)(NvDevice,unsigned*);
    int (*fan)(NvDevice,unsigned*),(*clock)(NvDevice,unsigned,unsigned*);
    int (*enc)(NvDevice,unsigned*,unsigned*),(*dec)(NvDevice,unsigned*,unsigned*);
    int (*compute)(NvDevice,unsigned*,NvProcess2*),(*graphics)(NvDevice,unsigned*,NvProcess2*);
    int (*samples)(NvDevice,NvSample*,unsigned*,unsigned long long);
} nv;
static void nv_load(void) {
    nv.lib=dlopen("libnvidia-ml.so.1",RTLD_LAZY|RTLD_LOCAL);
    if(!nv.lib)nv.lib=dlopen("/usr/lib/wsl/lib/libnvidia-ml.so.1",RTLD_LAZY|RTLD_LOCAL);
    if(!nv.lib)return;
#define NV_LOAD(field,symbol) do { void *sym=dlsym(nv.lib,symbol); memcpy(&nv.field,&sym,sizeof sym); } while(0)
    NV_LOAD(init,"nvmlInit_v2");NV_LOAD(shutdown,"nvmlShutdown");NV_LOAD(count,"nvmlDeviceGetCount_v2");
    NV_LOAD(device,"nvmlDeviceGetHandleByIndex_v2");NV_LOAD(name,"nvmlDeviceGetName");NV_LOAD(uuid,"nvmlDeviceGetUUID");
    NV_LOAD(util,"nvmlDeviceGetUtilizationRates");NV_LOAD(memory,"nvmlDeviceGetMemoryInfo");
    NV_LOAD(temp,"nvmlDeviceGetTemperature");NV_LOAD(power,"nvmlDeviceGetPowerUsage");NV_LOAD(limit,"nvmlDeviceGetPowerManagementLimit");
    NV_LOAD(fan,"nvmlDeviceGetFanSpeed");NV_LOAD(clock,"nvmlDeviceGetClockInfo");
    NV_LOAD(enc,"nvmlDeviceGetEncoderUtilization");NV_LOAD(dec,"nvmlDeviceGetDecoderUtilization");
    NV_LOAD(compute,"nvmlDeviceGetComputeRunningProcesses_v2");NV_LOAD(graphics,"nvmlDeviceGetGraphicsRunningProcesses_v2");
    NV_LOAD(samples,"nvmlDeviceGetProcessUtilization");
#undef NV_LOAD
    if(nv.init && nv.shutdown && nv.count && nv.device && nv.init()==0)nv.ready=true;
}
static void nv_processes(Monitor *m,Gpu *g) {
    size_t idx=(size_t)(g-m->gpus);
    /* A PID may appear in compute AND graphics: assign max memory once/device. */
    double *memory=resize(NULL,m->nproc,sizeof(double));bool *seen=calloc(m->nproc?m->nproc:1,sizeof(bool));
    if(!seen)fail("out of memory");
    for(size_t i=0;i<m->nproc;i++)memory[i]=NA;
    for(int pass=0;pass<2;pass++) {
        int (*fn)(NvDevice,unsigned*,NvProcess2*)=pass?nv.graphics:nv.compute;if(!fn)continue;
        unsigned count=0;int status=fn(g->handle,&count,NULL);
        if(status==0 || status==7)g->proc_supported=true;
        if((status!=0 && status!=7) || !count || count>1048576)continue;
        count+=16;NvProcess2 *buf=resize(NULL,count,sizeof *buf);
        status=fn(g->handle,&count,buf);
        if(status==0)for(unsigned j=0;j<count;j++) {
            Process *p=find_process(m->procs,m->nproc,(int)buf[j].pid);if(!p)continue;
            size_t k=(size_t)(p-m->procs);seen[k]=true;
            if(buf[j].used!=ULLONG_MAX && (double)buf[j].used>memory[k])memory[k]=(double)buf[j].used;
        }
        free(buf);
    }
    for(size_t i=0;i<m->nproc;i++)if(seen[i])gpu_process(&m->procs[i],idx,memory[i],NA);
    free(memory);free(seen);
    if(nv.samples) {
        unsigned n=0;int status=nv.samples(g->handle,NULL,&n,g->last_stamp);
        if((status==0 || status==7) && n && n<=1048576) {
            n+=16;NvSample *s=resize(NULL,n,sizeof *s);
            status=nv.samples(g->handle,s,&n,g->last_stamp);
            if(status==0) {
                double *util=resize(NULL,m->nproc,sizeof(double));uint64_t *stamp=calloc(m->nproc?m->nproc:1,sizeof(uint64_t));
                if(!stamp)fail("out of memory");
                for(size_t i=0;i<m->nproc;i++)util[i]=NA;
                for(unsigned j=0;j<n;j++) {
                    if(s[j].stamp>g->last_stamp)g->last_stamp=s[j].stamp;
                    Process *p=find_process(m->procs,m->nproc,(int)s[j].pid);if(!p)continue;
                    size_t k=(size_t)(p-m->procs);if(s[j].stamp>=stamp[k]){stamp[k]=s[j].stamp;util[k]=(double)s[j].sm;}
                }
                for(size_t i=0;i<m->nproc;i++)if(util[i]>=0)gpu_process(&m->procs[i],idx,NA,util[i]);
                free(util);free(stamp);
            }free(s);
        }
    }
}
static void nv_collect(Monitor *m) {
    if(!nv.ready)return;
    unsigned count=0;if(nv.count(&count)!=0)return;
    for(unsigned i=0;i<count && i<MAX_GPU;i++) {
        NvDevice device;if(nv.device(i,&device)!=0)continue;
        char id[128];if(!nv.uuid || nv.uuid(device,id,sizeof id)!=0)snprintf(id,sizeof id,"NVIDIA:%u",i);
        Gpu *g=gpu_slot(m,id);if(!g)break;g->handle=device;copystr(g->backend,sizeof g->backend,"NVML");
        if(!nv.name || nv.name(device,g->name,sizeof g->name)!=0)copystr(g->name,sizeof g->name,"NVIDIA GPU");
        NvUtil u;NvMemory mem;unsigned value,period;
        if(nv.util && nv.util(device,&u)==0){g->busy=u.gpu;g->mem_busy=u.memory;}
        if(nv.memory && nv.memory(device,&mem)==0){g->mem_used=(double)mem.used;g->mem_total=(double)mem.total;}
        if(nv.temp && nv.temp(device,0,&value)==0)g->temp=value;
        if(nv.power && nv.power(device,&value)==0)g->watts=value/1000.0;
        if(nv.limit && nv.limit(device,&value)==0)g->limit=value/1000.0;
        if(nv.fan && nv.fan(device,&value)==0)g->fan=value;
        if(nv.clock && nv.clock(device,0,&value)==0)g->clock=value;
        if(nv.clock && nv.clock(device,2,&value)==0)g->mem_clock=value;
        if(nv.enc && nv.enc(device,&value,&period)==0)g->encoder=value;
        if(nv.dec && nv.dec(device,&value,&period)==0)g->decoder=value;
        nv_processes(m,g);
    }
}
static double gpu_file(Gpu *g,const char *suffix) {
    char path[PATH_MAX];snprintf(path,sizeof path,"%s/%s",g->path,suffix);return read_number(path);
}
static void drm_discover(Monitor *m) {
    DIR *dir=opendir("/sys/class/drm");if(!dir)return;struct dirent *de;
    while((de=readdir(dir))) {
        unsigned card;char extra;if(sscanf(de->d_name,"card%u%c",&card,&extra)!=1)continue;
        char path[PATH_MAX],real[PATH_MAX],buf[256],id[128];
        snprintf(path,sizeof path,"/sys/class/drm/%s/device",de->d_name);
        if(!realpath(path,real))continue;
        char *base=strrchr(real,'/');base=base?base+1:real;
        copystr(id,sizeof id,base);
        snprintf(path,sizeof path,"/sys/class/drm/%s/device/vendor",de->d_name);
        unsigned vendor=read_text(path,buf,sizeof buf)?(unsigned)strtoul(buf,NULL,0):0;
        if(vendor==0x10de && nv.ready)continue;
        Gpu *g=gpu_slot(m,id);if(!g)break;
        snprintf(g->path,sizeof g->path,"/sys/class/drm/%s",de->d_name);
        copystr(g->backend,sizeof g->backend,"DRM/sysfs");
        const char *brand=vendor==0x1002?"AMD":vendor==0x8086?"Intel":vendor==0x10de?"NVIDIA":"DRM";
        snprintf(path,sizeof path,"%s/device/product_name",g->path);
        if(read_text(path,buf,sizeof buf))copystr(g->name,sizeof g->name,trim(buf));
        else {snprintf(path,sizeof path,"%s/device/device",g->path);unsigned device_id=read_text(path,buf,sizeof buf)?(unsigned)strtoul(buf,NULL,0):0;
            snprintf(g->name,sizeof g->name,"%s GPU %04x (%.64s)",brand,device_id,de->d_name);}
        g->busy=gpu_file(g,"device/gpu_busy_percent");g->mem_busy=gpu_file(g,"device/mem_busy_percent");
        g->mem_total=gpu_file(g,"device/mem_info_vram_total");g->mem_used=gpu_file(g,"device/mem_info_vram_used");
        g->clock=gpu_file(g,"gt_cur_freq_mhz");if(g->clock<0)g->clock=gpu_file(g,"gt/gt0/rps_cur_freq_mhz");
        if(g->clock<0)g->clock=gpu_file(g,"device/tile0/gt0/freq0/cur_freq");
        g->shared=vendor==0x8086 && g->mem_total<=0;
        char pattern[PATH_MAX];snprintf(pattern,sizeof pattern,"%s/device/hwmon/hwmon*",g->path);glob_t gl={0};
        if(!glob(pattern,0,NULL,&gl)) {
            for(size_t j=0;j<gl.gl_pathc;j++) {
                const char *suffix[]={"temp1_input","power1_average","power1_input","power1_cap","fan1_input","freq1_input","freq2_input"};
                double *targets[]={&g->temp,&g->watts,&g->watts,&g->limit,&g->rpm,&g->clock,&g->mem_clock};
                double factors[]={1000,1e6,1e6,1e6,1,1e6,1e6};
                for(size_t k=0;k<ARRAY_LEN(suffix);k++) {
                    snprintf(path,sizeof path,"%s/%s",gl.gl_pathv[j],suffix[k]);double v=read_number(path);
                    if(v>=0 && *targets[k]<0)*targets[k]=v/factors[k];
                }
            }
        }
        globfree(&gl);
    }closedir(dir);
}

typedef struct {
    char name[64];uint64_t ns,cycles,total;unsigned capacity;
    bool has_ns,has_cycles,has_total;
} Engine;
typedef struct {
    char device[128];uint64_t client;bool has_client;int pid;
    Engine engines[MAX_ENGINE];size_t nengine;double memory;
} DrmClient;
static DrmClient *clients,*oldclients;static size_t nclient,noldclient,clientcap;
static Engine *engine_slot(DrmClient *c,const char *name) {
    for(size_t i=0;i<c->nengine;i++)if(!strcmp(c->engines[i].name,name))return &c->engines[i];
    if(c->nengine==MAX_ENGINE)return NULL;
    Engine *e=&c->engines[c->nengine++];
    memset(e,0,sizeof *e);copystr(e->name,sizeof e->name,name);e->capacity=1;return e;
}
static void parse_fdinfo(char *text,DrmClient *c) {
    char *save=NULL;double resident=0,legacy=0;bool has_res=false,has_legacy=false;
    for(char *line=strtok_r(text,"\n",&save);line;line=strtok_r(NULL,"\n",&save)) {
        char *colon=strchr(line,':');if(!colon)continue;*colon=0;char *key=trim(line),*value=trim(colon+1);
        if(!strcmp(key,"drm-client-id")){c->client=strtoull(value,NULL,10);c->has_client=true;}
        else if(!strcmp(key,"drm-pdev"))copystr(c->device,sizeof c->device,value);
        else if(!strncmp(key,"drm-resident-",13) || !strncmp(key,"drm-memory-",11)) {
            double v=strtod(value,NULL);if(strstr(value,"KiB"))v*=1024;else if(strstr(value,"MiB"))v*=1048576;
            if(v>=0 && isfinite(v)){if(!strncmp(key,"drm-resident-",13)){resident+=v;has_res=true;}else{legacy+=v;has_legacy=true;}}
        }else {
            const char *name=NULL;int mode=0;
            if(!strncmp(key,"drm-engine-capacity-",20)){name=key+20;mode=1;}
            else if(!strncmp(key,"drm-engine-",11)){name=key+11;mode=2;}
            else if(!strncmp(key,"drm-total-cycles-",17)){name=key+17;mode=3;}
            else if(!strncmp(key,"drm-cycles-",11)){name=key+11;mode=4;}
            if(name){Engine *e=engine_slot(c,name);if(!e)continue;uint64_t v=strtoull(value,NULL,10);
                if(mode==1)e->capacity=v>0 && v<UINT_MAX?(unsigned)v:1;
                else if(mode==2 && strstr(value,"ns")){e->ns=v;e->has_ns=true;}
                else if(mode==3){e->total=v;e->has_total=true;}
                else if(mode==4){e->cycles=v;e->has_cycles=true;}
            }
        }
    }
    c->memory=has_res?resident:has_legacy?legacy:NA;
}
static double drm_usage(DrmClient *cur,const DrmClient *prev,double dt) {
    double peak=NA;if(!prev || dt<=0)return peak;
    for(size_t i=0;i<cur->nengine;i++) {Engine *a=&cur->engines[i];
        for(size_t j=0;j<prev->nengine;j++){const Engine *b=&prev->engines[j];if(strcmp(a->name,b->name))continue;
            double v=NA;
            if(a->has_ns && b->has_ns) {
                if(a->ns<b->ns)a->ns=b->ns;
                v=100.0*(double)(a->ns-b->ns)/(dt*1e9*a->capacity);
            }else if(a->has_cycles && b->has_cycles && a->has_total && b->has_total) {
                if(a->cycles<b->cycles)a->cycles=b->cycles;
                if(a->total<b->total)a->total=b->total;
                if(a->total>b->total)v=100.0*(double)(a->cycles-b->cycles)/(double)(a->total-b->total)/a->capacity;
            }
            if(v>=0 && v>peak)peak=clamp(v,0,100);
            break;
        }
    }return peak;
}
static void drm_processes(Monitor *m) {
    free(oldclients);oldclients=clients;noldclient=nclient;clients=NULL;nclient=clientcap=0;
    bool any=false;for(size_t i=0;i<m->ngpu;i++)if(m->gpus[i].seen && !strcmp(m->gpus[i].backend,"DRM/sysfs"))any=true;
    if(!any)return;
    for(size_t i=0;i<m->nproc;i++) {
        Process *p=&m->procs[i];char path[PATH_MAX];snprintf(path,sizeof path,"/proc/%d/fdinfo",p->pid);
        DIR *dir=opendir(path);if(!dir){if(errno==EACCES || errno==EPERM)m->denied++;continue;}
        struct dirent *de;
        while((de=readdir(dir))) {
            if(de->d_name[0]=='.')continue;
            /* Only inspect character-device fds; do not parse arbitrary fdinfo. */
            struct stat st;snprintf(path,sizeof path,"/proc/%d/fd/%s",p->pid,de->d_name);
            if(stat(path,&st) || !S_ISCHR(st.st_mode) || major(st.st_rdev)!=226)continue;
            char buf[16384];snprintf(path,sizeof path,"/proc/%d/fdinfo/%s",p->pid,de->d_name);
            if(!read_text(path,buf,sizeof buf))continue;
            DrmClient c={0};c.pid=p->pid;parse_fdinfo(buf,&c);if(!c.has_client)continue;
            if(!c.device[0]) {char sys[PATH_MAX],real[PATH_MAX];snprintf(sys,sizeof sys,"/sys/dev/char/%u:%u/device",major(st.st_rdev),minor(st.st_rdev));
                if(realpath(sys,real)){char *s=strrchr(real,'/');copystr(c.device,sizeof c.device,s?s+1:real);}}
            size_t gpu;for(gpu=0;gpu<m->ngpu;gpu++)if(m->gpus[gpu].seen && !strcmp(c.device,m->gpus[gpu].id))break;
            if(gpu==m->ngpu)continue;
            bool duplicate=false;for(size_t k=0;k<nclient;k++)if(clients[k].client==c.client && !strcmp(clients[k].device,c.device)){duplicate=true;break;}
            if(duplicate)continue;
            DrmClient *old=NULL;for(size_t k=0;k<noldclient;k++)if(oldclients[k].client==c.client && oldclients[k].pid==c.pid && !strcmp(oldclients[k].device,c.device)){old=&oldclients[k];break;}
            double util=drm_usage(&c,old,m->elapsed);gpu_process(p,gpu,c.memory,util);m->gpus[gpu].proc_supported=true;
            if(nclient==clientcap){clientcap=clientcap?clientcap*2:64;clients=resize(clients,clientcap,sizeof *clients);}clients[nclient++]=c;
        }closedir(dir);
    }
}
#endif /* __linux__ */

#ifdef __APPLE__
static double sysctl_number(const char *name) {
    uint64_t v=0;size_t len=sizeof v;
    return sysctlbyname(name,&v,&len,NULL,0)==0 ? (double)v:NA;
}
static double cf_number(CFDictionaryRef dict,CFStringRef key) {
    if(!dict || CFGetTypeID(dict)!=CFDictionaryGetTypeID())return NA;
    CFTypeRef value=CFDictionaryGetValue(dict,key);double n;
    if(value && CFGetTypeID(value)==CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)value,kCFNumberDoubleType,&n) && isfinite(n) && n>=0)return n;
    return NA;
}
static void cf_text(CFTypeRef value,char *buf,size_t size) {
    if(!value || !size)return;
    if(CFGetTypeID(value)==CFStringGetTypeID())CFStringGetCString((CFStringRef)value,buf,(CFIndex)size,kCFStringEncodingUTF8);
    else if(CFGetTypeID(value)==CFDataGetTypeID()) {
        CFIndex len=CFDataGetLength((CFDataRef)value);size_t n=(size_t)len<size-1?(size_t)len:size-1;
        memcpy(buf,CFDataGetBytePtr((CFDataRef)value),n);buf[n]=0;
    }cleanstr(buf);
}
static void mac_cpu(Monitor *m) {
    natural_t count=0;processor_info_array_t info=NULL;mach_msg_type_number_t len=0;
    host_t host=mach_host_self();
    if(host_processor_info(host,PROCESSOR_CPU_LOAD_INFO,&count,&info,&len)==KERN_SUCCESS) {
        if(count>MAX_CPU)count=MAX_CPU;
        if(count>m->ncpu){size_t old=m->ncpu;m->cpus=resize(m->cpus,count,sizeof(Cpu));memset(m->cpus+old,0,(count-old)*sizeof(Cpu));}
        m->ncpu=count;uint64_t alltotal=0,allidle=0;static uint64_t oldtotal,oldidle;static bool valid;
        for(size_t i=0;i<count;i++) {
            uint64_t total=0,idle=(unsigned)info[i*CPU_STATE_MAX+CPU_STATE_IDLE];
            for(size_t k=0;k<CPU_STATE_MAX;k++)total+=(unsigned)info[i*CPU_STATE_MAX+k];
            Cpu *c=&m->cpus[i];c->usage=c->valid?percent(total,idle,c->total,c->idle):NA;c->total=total;c->idle=idle;c->valid=true;
            alltotal+=total;allidle+=idle;
        }
        m->cpu=valid?percent(alltotal,allidle,oldtotal,oldidle):NA;oldtotal=alltotal;oldidle=allidle;valid=true;
        vm_deallocate(mach_task_self(),(vm_address_t)info,(vm_size_t)len*sizeof(integer_t));
    }
    mach_port_deallocate(mach_task_self(),host);
    m->cpu_mhz=sysctl_number("hw.cpufrequency");if(m->cpu_mhz>=0)m->cpu_mhz/=1e6;
    if(!m->cpu_name[0]){size_t n=sizeof m->cpu_name;if(sysctlbyname("machdep.cpu.brand_string",m->cpu_name,&n,NULL,0))copystr(m->cpu_name,sizeof m->cpu_name,"Apple processor");}
    struct timeval boot={0};size_t n=sizeof boot;if(!sysctlbyname("kern.boottime",&boot,&n,NULL,0))m->uptime=difftime(time(NULL),boot.tv_sec);
    if(getloadavg(m->load,3)!=3)m->load[0]=m->load[1]=m->load[2]=NA;
}
static void mac_memory(Monitor *m) {
    m->mem_total=sysctl_number("hw.memsize");host_t host=mach_host_self();vm_size_t page=0;
    vm_statistics64_data_t vm={0};mach_msg_type_number_t count=HOST_VM_INFO64_COUNT;
    if(host_page_size(host,&page)==KERN_SUCCESS && host_statistics64(host,HOST_VM_INFO64,(host_info64_t)&vm,&count)==KERN_SUCCESS) {
        /* Used working memory: active + wired + compressed physical pages. */
        m->mem_used=clamp(((double)vm.active_count+vm.wire_count+vm.compressor_page_count)*page,0,m->mem_total);
    }
    mach_port_deallocate(mach_task_self(),host);
    struct xsw_usage sw={0};size_t n=sizeof sw;
    if(!sysctlbyname("vm.swapusage",&sw,&n,NULL,0)){m->swap_total=(double)sw.xsu_total;m->swap_used=(double)sw.xsu_used;}
}
static void mac_processes(Monitor *m) {
    int bytes=proc_listpids(PROC_ALL_PIDS,0,NULL,0);if(bytes<=0)return;
    if(bytes>INT_MAX-4096)return;
    bytes+=4096;int *pids=resize(NULL,(size_t)bytes,1);
    int got=proc_listpids(PROC_ALL_PIDS,0,pids,bytes);
    for(int i=0;i<got/(int)sizeof(int);i++) {
        if(pids[i]<=0)continue;
        struct proc_taskallinfo task={0};
        if(proc_pidinfo(pids[i],PROC_PIDTASKALLINFO,0,&task,sizeof task)!=(int)sizeof task){m->denied++;continue;}
        Process *p=add_process(m);p->pid=pids[i];p->ppid=(int)task.pbsd.pbi_ppid;p->uid=task.pbsd.pbi_uid;
        p->birth=(uint64_t)task.pbsd.pbi_start_tvsec*1000000+task.pbsd.pbi_start_tvusec;
        p->ticks=task.ptinfo.pti_total_user+task.ptinfo.pti_total_system;p->rss=task.ptinfo.pti_resident_size;
        p->threads=task.ptinfo.pti_threadnum;p->state=task.pbsd.pbi_status==2?'R':task.pbsd.pbi_status==4?'T':task.pbsd.pbi_status==5?'Z':'S';
        if(task.pbsd.pbi_name[0])snprintf(p->name,sizeof p->name,"%.*s",(int)sizeof task.pbsd.pbi_name,task.pbsd.pbi_name);
        else snprintf(p->name,sizeof p->name,"%.*s",(int)sizeof task.pbsd.pbi_comm,task.pbsd.pbi_comm);
        char procpath[PROC_PIDPATHINFO_MAXSIZE]={0};
        if(proc_pidpath(p->pid,procpath,sizeof procpath)>0)copystr(p->command,sizeof p->command,procpath);
        else copystr(p->command,sizeof p->command,p->name);
        p->read_bytes=p->write_bytes=UINT64_MAX;
        struct rusage_info_v2 usage={0};
        if(proc_pid_rusage(p->pid,RUSAGE_INFO_V2,(rusage_info_t*)&usage)==0){p->read_bytes=usage.ri_diskio_bytesread;p->write_bytes=usage.ri_diskio_byteswritten;}
        finish_process(m,p,1e9);
    }free(pids);
}
static void mac_network(Monitor *m) {
    struct ifaddrs *list=NULL;if(getifaddrs(&list))return;uint64_t rx=0,tx=0;
    for(struct ifaddrs *it=list;it;it=it->ifa_next) {
        if(!it->ifa_addr || it->ifa_addr->sa_family!=AF_LINK || !it->ifa_data || (it->ifa_flags&IFF_LOOPBACK))continue;
        struct if_data *d=it->ifa_data;rx+=d->ifi_ibytes;tx+=d->ifi_obytes;
    }freeifaddrs(list);
    m->rx=m->network_valid?rate(rx,m->prev_rx,m->elapsed):NA;m->tx=m->network_valid?rate(tx,m->prev_tx,m->elapsed):NA;
    m->prev_rx=rx;m->prev_tx=tx;m->network_valid=true;
}
static void mac_disks(Monitor *m) {
    io_iterator_t iter=0;uint64_t readb=0,writeb=0;bool valid=false;
    if(IOServiceGetMatchingServices(MACH_PORT_NULL,IOServiceMatching("IOBlockStorageDriver"),&iter)!=KERN_SUCCESS)return;
    io_registry_entry_t service;
    while((service=IOIteratorNext(iter))) {
        CFTypeRef stats=IORegistryEntryCreateCFProperty(service,CFSTR("Statistics"),kCFAllocatorDefault,0);
        if(stats && CFGetTypeID(stats)==CFDictionaryGetTypeID()) {
            double r=cf_number((CFDictionaryRef)stats,CFSTR("Bytes (Read)")),w=cf_number((CFDictionaryRef)stats,CFSTR("Bytes (Write)"));
            if(r>=0 && w>=0){readb+=(uint64_t)r;writeb+=(uint64_t)w;valid=true;}
        }if(stats)CFRelease(stats);IOObjectRelease(service);
    }IOObjectRelease(iter);
    m->disk_read=valid && m->disk_valid?rate(readb,m->prev_read,m->elapsed):NA;
    m->disk_write=valid && m->disk_valid?rate(writeb,m->prev_write,m->elapsed):NA;
    m->prev_read=readb;m->prev_write=writeb;m->disk_valid=valid;
}
static void mac_gpus(Monitor *m) {
    io_iterator_t iter=0;
    if(IOServiceGetMatchingServices(MACH_PORT_NULL,IOServiceMatching("IOAccelerator"),&iter)!=KERN_SUCCESS)return;
    io_registry_entry_t service;
    while((service=IOIteratorNext(iter))) {
        uint64_t entry=0;IORegistryEntryGetRegistryEntryID(service,&entry);char id[128];snprintf(id,sizeof id,"IOKit:%"PRIu64,entry);
        Gpu *g=gpu_slot(m,id);if(!g){IOObjectRelease(service);continue;}
        if(g->service)IOObjectRelease(g->service);
        g->service=service;
        copystr(g->backend,sizeof g->backend,"IOKit (best effort)");
        io_name_t name={0};IORegistryEntryGetName(service,name);copystr(g->name,sizeof g->name,name);
        CFTypeRef model=IORegistryEntrySearchCFProperty(service,kIOServicePlane,CFSTR("model"),kCFAllocatorDefault,kIORegistryIterateRecursively|kIORegistryIterateParents);
        if(model){cf_text(model,g->name,sizeof g->name);CFRelease(model);}
        g->shared=contains(name,"AGX") || contains(g->name,"Apple");
        CFTypeRef stats=IORegistryEntryCreateCFProperty(service,CFSTR("PerformanceStatistics"),kCFAllocatorDefault,0);
        if(stats && CFGetTypeID(stats)==CFDictionaryGetTypeID()) {
            CFDictionaryRef d=(CFDictionaryRef)stats;
            g->busy=cf_number(d,CFSTR("Device Utilization %"));
            if(g->busy<0)g->busy=cf_number(d,CFSTR("GPU Activity(%)"));
            g->mem_used=cf_number(d,CFSTR("In use system memory"));
            if(g->mem_used<0)g->mem_used=cf_number(d,CFSTR("Alloc system memory"));
            if(g->mem_used>=0)g->shared=true;
            g->temp=cf_number(d,CFSTR("Temperature(C)"));
        }if(stats)CFRelease(stats);
        if(g->busy>100)g->busy=NA;
        if(!g->shared) {
            CFTypeRef v=IORegistryEntrySearchCFProperty(service,kIOServicePlane,CFSTR("VRAM,totalMB"),kCFAllocatorDefault,kIORegistryIterateRecursively|kIORegistryIterateParents);
            double n=0;if(v && CFGetTypeID(v)==CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v,kCFNumberDoubleType,&n))g->mem_total=n*1048576;
            if(v)CFRelease(v);
        }
    }IOObjectRelease(iter);
}
#endif /* __APPLE__ */

static void monitor_init(Monitor *m) {
    memset(m,0,sizeof *m);m->cpu=m->mem_used=m->mem_total=m->swap_used=m->swap_total=NA;
    m->cpu_temp=m->cpu_mhz=m->cpu_power=m->rx=m->tx=m->disk_read=m->disk_write=NA;
    m->fs_total=m->fs_used=NA;
    gethostname(m->hostname,sizeof(m->hostname)-1);cleanstr(m->hostname);
    struct utsname u;if(!uname(&u)) {
        snprintf(m->platform,sizeof m->platform,"%.48s %.64s",u.sysname,u.release);
        m->wsl=contains(u.release,"microsoft");
    }
#ifdef __linux__
    if(!opt.no_gpu)nv_load();
#endif
}
static void sample(Monitor *m) {
    double now=monotime();m->elapsed=m->now>0?now-m->now:0;m->now=now;m->denied=0;
    free(m->old);m->old=m->procs;m->nold=m->nproc;m->procs=NULL;m->nproc=m->cap=0;
    for(size_t i=0;i<m->ngpu;i++)gpu_reset(&m->gpus[i]);
#ifdef __linux__
    linux_cpu(m);linux_memory(m);linux_network(m);linux_disks(m);linux_sensors(m);linux_processes(m);
#else
    mac_cpu(m);mac_memory(m);mac_network(m);mac_disks(m);mac_processes(m);
#endif
    if(m->nproc)qsort(m->procs,m->nproc,sizeof(Process),pid_cmp);
    if(!opt.no_gpu) {
#ifdef __linux__
        nv_collect(m);drm_discover(m);drm_processes(m);
#else
        mac_gpus(m);
#endif
    }
    size_t active=0;for(size_t i=0;i<m->ngpu;i++){push(&m->gpus[i].history,m->gpus[i].busy);if(m->gpus[i].seen)active++;}
    if(opt.no_gpu)copystr(m->gpu_status,sizeof m->gpu_status,"GPU monitoring disabled (--no-gpu)");
    else if(!active)copystr(m->gpu_status,sizeof m->gpu_status,"No accessible GPU detected; check driver / device permissions");
    else snprintf(m->gpu_status,sizeof m->gpu_status,"%zu GPU(s) | N/A = unsupported / inaccessible | process visibility may be restricted",active);
    struct statvfs fs;if(!statvfs("/",&fs)) {m->fs_total=(double)fs.f_blocks*fs.f_frsize;m->fs_used=(double)(fs.f_blocks-fs.f_bfree)*fs.f_frsize;}
    push(&m->cpu_hist,m->cpu);push(&m->mem_hist,m->mem_total>0?100*m->mem_used/m->mem_total:NA);
    push(&m->rx_hist,m->rx);push(&m->tx_hist,m->tx);
}
static void monitor_free(Monitor *m) {
    free(m->cpus);free(m->procs);free(m->old);
#ifdef __linux__
    if(nv.ready)nv.shutdown();
    if(nv.lib)dlclose(nv.lib);
    free(clients);free(oldclients);
#else
    for(size_t i=0;i<m->ngpu;i++)if(m->gpus[i].service)IOObjectRelease(m->gpus[i].service);
#endif
}

/* Retained cell grid: only changed rows are sent to the terminal. */
typedef struct {uint32_t glyph;unsigned char fg,bg;} Cell;
static Cell *screen,*previous;static int width,height;static bool repaint=true;
static void cell(int x,int y,uint32_t c,int fg,int bg) {
    if(x<0 || y<0 || x>=width || y>=height)return;
    Cell *p=&screen[(size_t)y*(size_t)width+(size_t)x];p->glyph=c;p->fg=(unsigned char)fg;p->bg=(unsigned char)bg;
}
static void text_at(int x,int y,int fg,int bg,const char *s) {
    /* Names are rendered safely in single columns; non-ASCII bytes become '?'. */
    while(*s && x<width) {unsigned char c=(unsigned char)*s++;if(c>=128){while((*s&0xc0)==0x80)s++;c='?';}
        cell(x++,y,c<32 || c==127?' ':c,fg,bg);}
}
static void linef(int x,int y,int max,int fg,const char *fmt,...)
    __attribute__((format(printf,5,6)));
static void linef(int x,int y,int max,int fg,const char *fmt,...) {
    if(max<=0)return;
    char buf[2048];va_list ap;va_start(ap,fmt);vsnprintf(buf,sizeof buf,fmt,ap);va_end(ap);
    if((size_t)max<sizeof buf)buf[max]=0;
    text_at(x,y,fg,BG,buf);
}
static void hline(int x,int y,int w,int fg) {for(int i=0;i<w;i++)cell(x+i,y,opt.ascii?'-':0x2500,fg,BG);}
static void box(int x,int y,int w,int h,const char *title) {
    if(w<2 || h<2)return;
    hline(x+1,y,w-2,MUTED);hline(x+1,y+h-1,w-2,MUTED);
    for(int i=1;i<h-1;i++){cell(x,y+i,opt.ascii?'|':0x2502,MUTED,BG);cell(x+w-1,y+i,opt.ascii?'|':0x2502,MUTED,BG);}
    cell(x,y,opt.ascii?'+':0x256d,MUTED,BG);cell(x+w-1,y,opt.ascii?'+':0x256e,MUTED,BG);
    cell(x,y+h-1,opt.ascii?'+':0x2570,MUTED,BG);cell(x+w-1,y+h-1,opt.ascii?'+':0x256f,MUTED,BG);
    linef(x+2,y,w-4,ACCENT," %s ",title);
}
static int usage_color(double p) {return p>=90?BAD:p>=70?WARN:GOOD;}
static void bar(int x,int y,int w,double pct,int color) {
    if(w<=0)return;
    int fill=pct<0?0:(int)lround(clamp(pct,0,100)*w/100.0);
    for(int i=0;i<w;i++)cell(x+i,y,opt.ascii?(i<fill?'#':'.'):(i<fill?0x2588:0x2591),i<fill?color:MUTED,BG);
}
static void graph(int x,int y,int w,int h,const History *hist,double scale,int color) {
    if(w<1 || h<1 || scale<=0)return;
    for(int col=0;col<w;col++) {
        double v=hist_at(hist,(size_t)(w-1-col));if(v<0)continue;
        double level=clamp(v/scale,0,1)*h;
        for(int row=0;row<h;row++) {
            double part=clamp(level-(h-1-row),0,1);
            if(part>0)cell(x+col,y+row,opt.ascii?'#':0x2580+(uint32_t)ceil(part*8),color,BG);
            else if(row==h-1)cell(x+col,y+row,opt.ascii?'.':0x00b7,MUTED,BG);
        }
    }
}
static void utf8(uint32_t c) {
    if(c<128)putchar((int)c);
    else if(c<2048){putchar((int)(0xc0|(c>>6)));putchar((int)(0x80|(c&63)));}
    else {putchar((int)(0xe0|(c>>12)));putchar((int)(0x80|((c>>6)&63)));putchar((int)(0x80|(c&63)));}
}
static unsigned role_rgb(int role) {return opt.custom_set[role]?opt.custom[role]:themes[opt.theme].rgb[role];}
static void draw_flush(void) {
    if(repaint)fputs("\033[2J",stdout);
    for(int y=0;y<height;y++) {
        size_t offset=(size_t)y*(size_t)width;
        if(!repaint && !memcmp(screen+offset,previous+offset,(size_t)width*sizeof(Cell)))continue;
        printf("\033[%d;1H",y+1);int fg=-1,bg=-1;
        for(int x=0;x<width;x++) {Cell *p=&screen[offset+(size_t)x];
            if(opt.color && (fg!=p->fg || bg!=p->bg)) {
                unsigned f=role_rgb(p->fg),b=role_rgb(p->bg);
                printf("\033[38;2;%u;%u;%u;48;2;%u;%u;%um",f>>16,(f>>8)&255,f&255,b>>16,(b>>8)&255,b&255);
                fg=p->fg;bg=p->bg;
            }utf8(p->glyph);
        }
    }
    fputs("\033[0m",stdout);fflush(stdout);memcpy(previous,screen,(size_t)width*(size_t)height*sizeof(Cell));repaint=false;
}
static void screen_size(void) {
    struct winsize ws={0};int w=80,h=24;
    if(!ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws) && ws.ws_col && ws.ws_row){w=ws.ws_col;h=ws.ws_row;}
    w=(int)clamp(w,1,1000);h=(int)clamp(h,1,400);
    if(w!=width || h!=height){width=w;height=h;size_t n=(size_t)w*(size_t)h;
        screen=resize(screen,n,sizeof(Cell));previous=resize(previous,n,sizeof(Cell));memset(previous,0,n*sizeof(Cell));repaint=true;}
    memset(screen,0,(size_t)width*(size_t)height*sizeof(Cell));
    for(size_t i=0;i<(size_t)width*(size_t)height;i++){screen[i].glyph=' ';screen[i].fg=FG;screen[i].bg=BG;}
}
static void draw_cores(Monitor *m,int x,int y,int w,int rows) {
    int cols=w/23;if(cols<1)cols=1;int cw=w/cols;size_t capacity=(size_t)(cols*rows);if(!capacity)return;
    size_t pages=(m->ncpu+capacity-1)/capacity;if(!pages)pages=1;
    opt.core_page=(int)((size_t)opt.core_page%pages);size_t start=(size_t)opt.core_page*capacity;
    for(size_t i=0;i<capacity && start+i<m->ncpu;i++) {
        size_t idx=start+i;int xx=x+(int)(i%(size_t)cols)*cw,yy=y+(int)(i/(size_t)cols);double v=m->cpus[idx].usage;
        char value[24];metric(v,"%",value,sizeof value);linef(xx,yy,cw-1,FG,"%4zu %6s",idx,value);bar(xx+12,yy,cw-14,v,usage_color(v));
    }
}
static void draw_cpu(Monitor *m,int x,int y,int w,int h,bool cores) {
    box(x,y,w,h,"CPU");char pct[32],temp[32],mhz[32],power[32];
    metric(m->cpu,"%",pct,sizeof pct);metric(m->cpu_temp,"C",temp,sizeof temp);metric(m->cpu_mhz," MHz",mhz,sizeof mhz);metric(m->cpu_power," W",power,sizeof power);
    linef(x+2,y+1,w-4,FG,"%s   %zu logical CPUs   %s",pct,m->ncpu,mhz);
    linef(x+2,y+2,w-4,MUTED,"%s",m->cpu_name);
    int gh=h-(cores?7:5);if(gh<1)gh=1;
    graph(x+2,y+3,w-4,gh,&m->cpu_hist,100,ACCENT);
    if(cores && h>=9)draw_cores(m,x+2,y+h-4,w-4,2);
    linef(x+2,y+h-2,w-4,MUTED,"Peak temp %s | Package %s | load %.2f %.2f %.2f",temp,power,m->load[0],m->load[1],m->load[2]);
}
static void draw_memory(Monitor *m,int x,int y,int w,int h) {
    box(x,y,w,h,"MEMORY / I/O");char a[32],b[32];bytes_text(m->mem_used,a,sizeof a);bytes_text(m->mem_total,b,sizeof b);
    linef(x+2,y+1,w-4,FG,"RAM %s / %s",a,b);
    double pct=m->mem_total>0?100*m->mem_used/m->mem_total:NA;bar(x+2,y+2,w-4,pct,GOOD);
    bytes_text(m->swap_used,a,sizeof a);bytes_text(m->swap_total,b,sizeof b);linef(x+2,y+3,w-4,MUTED,"Swap %s / %s",a,b);
    bytes_text(m->rx,a,sizeof a);bytes_text(m->tx,b,sizeof b);linef(x+2,y+4,w-4,FG,"Net RX %s/s | TX %s/s",a,b);
    bytes_text(m->disk_read,a,sizeof a);bytes_text(m->disk_write,b,sizeof b);linef(x+2,y+5,w-4,FG,"Disk R %s/s | W %s/s",a,b);
    bytes_text(m->fs_used,a,sizeof a);bytes_text(m->fs_total,b,sizeof b);linef(x+2,y+6,w-4,MUTED,"Root %s / %s",a,b);
    if(h>=10)graph(x+2,y+7,w-4,h-8,&m->mem_hist,100,GOOD);
}
static Gpu *selected_gpu(Monitor *m) {
    if(!m->ngpu)return NULL;
    opt.gpu_index=(int)((size_t)opt.gpu_index%m->ngpu);return &m->gpus[opt.gpu_index];
}
static void draw_gpu(Monitor *m,int x,int y,int w,int h) {
    Gpu *g=selected_gpu(m);char title[200];
    if(!g){box(x,y,w,h,"GPU");linef(x+2,y+1,w-4,MUTED,"%s",m->gpu_status);if(h>3)linef(x+2,y+2,w-4,MUTED,"CPU and process monitoring remains available.");return;}
    snprintf(title,sizeof title,"GPU %d/%zu  %s",opt.gpu_index+1,m->ngpu,g->name);box(x,y,w,h,title);
    char busy[32],a[32],b[32],temp[32],watts[32],clock[32];
    metric(g->busy,"%",busy,sizeof busy);bytes_text(g->mem_used,a,sizeof a);bytes_text(g->mem_total,b,sizeof b);
    metric(g->temp,"C",temp,sizeof temp);metric(g->watts,"W",watts,sizeof watts);metric(g->clock,"MHz",clock,sizeof clock);
    linef(x+2,y+1,w-4,FG,"Usage %s | %s %s / %s | %s",busy,g->shared?"Shared":"VRAM",a,b,g->backend);
    linef(x+2,y+2,w-4,MUTED,"Temp %s | Power %s | Clock %s%s",temp,watts,clock,g->seen?"":" | DISCONNECTED");
    int graph_y=3;
    if(h>=10) {
        char mb[32],mc[32],limit[32];metric(g->mem_busy,"%",mb,sizeof mb);metric(g->mem_clock,"MHz",mc,sizeof mc);metric(g->limit,"W",limit,sizeof limit);
        linef(x+2,y+3,w-4,MUTED,"Mem activity %s | Mem clock %s | Power limit %s",mb,mc,limit);graph_y=4;
    }
    int gh=h-graph_y-2;if(gh>0)graph(x+2,y+graph_y,w-4,gh,&g->history,100,ACCENT);
    if(h>=6) {
        char fan[32],enc[32],dec[32];metric(g->fan,"%",fan,sizeof fan);if(g->fan<0)metric(g->rpm,"rpm",fan,sizeof fan);
        metric(g->encoder,"%",enc,sizeof enc);metric(g->decoder,"%",dec,sizeof dec);
        linef(x+2,y+h-2,w-4,MUTED,"Fan %s | Enc %s | Dec %s | [ ] switch GPU",fan,enc,dec);
    }
}
static int process_sort(const void *a,const void *b) {
    const Process *p=*(Process*const*)a,*q=*(Process*const*)b;int result=0;
    double x=0,y=0;
    if(opt.sort==SORT_CPU){x=p->cpu;y=q->cpu;}else if(opt.sort==SORT_MEM){x=(double)p->rss;y=(double)q->rss;}
    else if(opt.sort==SORT_GPU){x=p->gpu;y=q->gpu;}
    if(opt.sort<=SORT_GPU)result=(y>x)-(y<x);
    else if(opt.sort==SORT_PID)result=(p->pid>q->pid)-(p->pid<q->pid);
    else result=strcasecmp(p->name,q->name);
    if(!result)result=(p->pid>q->pid)-(p->pid<q->pid);
    return opt.reverse?-result:result;
}
static Process **visible;static size_t nvisible;static int process_rows=1,focused_pid;
static void list_processes(Monitor *m) {
    visible=resize(visible,m->nproc,sizeof(Process*));nvisible=0;
    for(size_t i=0;i<m->nproc;i++) {
        Process *p=&m->procs[i];char pid[32];snprintf(pid,sizeof pid,"%d",p->pid);
        if((opt.gpu_only || opt.view==2) && !p->gpu_mask)continue;
        if(!contains(p->command,opt.filter) && !contains(p->user,opt.filter) && !contains(pid,opt.filter))continue;
        visible[nvisible++]=p;
    }
    if(nvisible)qsort(visible,nvisible,sizeof(Process*),process_sort);
    if(focused_pid)for(size_t i=0;i<nvisible;i++)if(visible[i]->pid==focused_pid){opt.selection=i;break;}
    if(opt.selection>=nvisible)opt.selection=nvisible?nvisible-1:0;
    if(opt.details && (!nvisible || (focused_pid && visible[opt.selection]->pid!=focused_pid))){opt.details=false;copystr(opt.notice,sizeof opt.notice,"Selected process exited or no longer matches the filter.");}
    focused_pid=nvisible?visible[opt.selection]->pid:0;
}
static void draw_processes(Monitor *m,int y,int h) {
    if(h<4)return;
    char title[192];snprintf(title,sizeof title,"PROCESSES  %zu/%zu  sort:%s%s%s",nvisible,m->nproc,sort_names[opt.sort],opt.reverse?" ascending":"",opt.gpu_only || opt.view==2?"  GPU only":"");
    box(0,y,width,h,title);
    bool wide=width>=100;
    const char *header=wide?"    PID USER       S   CPU%       RAM   GPU%   GPU MEM   THR COMMAND":"    PID   CPU%       RAM   GPU% COMMAND";
    linef(2,y+1,width-4,ACCENT,"%s",header);
    process_rows=h-3;
    if(opt.selection<opt.scroll)opt.scroll=opt.selection;
    if(opt.selection>=opt.scroll+(size_t)process_rows)opt.scroll=opt.selection-(size_t)process_rows+1;
    if(opt.scroll>nvisible)opt.scroll=0;
    for(int row=0;row<process_rows;row++) {
        size_t idx=opt.scroll+(size_t)row;if(idx>=nvisible)break;Process *p=visible[idx];char ram[32],gm[32],cpu[32],gpu[32],buf[1024];
        bytes_text((double)p->rss,ram,sizeof ram);bytes_text(p->gpu_mem,gm,sizeof gm);metric(p->cpu,"",cpu,sizeof cpu);metric(p->gpu,"",gpu,sizeof gpu);
        if(wide)snprintf(buf,sizeof buf,"%7d %-10.10s %c %6s %9s %6s %9s %5d %s",p->pid,p->user,p->state,cpu,ram,gpu,gm,p->threads,p->command);
        else snprintf(buf,sizeof buf,"%7d %6s %9s %6s %s",p->pid,cpu,ram,gpu,p->command);
        int bg=idx==opt.selection?SELECT_BG:BG;
        for(int col=1;col<width-1;col++)cell(col,y+2+row,' ',FG,bg);
        if(idx==opt.selection)cell(1,y+2+row,'>',ACCENT,bg);
        if((size_t)(width-4)<sizeof buf)buf[width-4]=0;
        text_at(2,y+2+row,FG,bg,buf);
    }
    if(!nvisible)linef(2,y+2,width-4,MUTED,"No visible matching processes (GPU attribution may be unsupported).");
}
static void draw_help(void) {
    int w=width<100?width:100,h=height<29?height:29,x=(width-w)/2,y=(height-h)/2;
    for(int r=y;r<y+h;r++)for(int c=x;c<x+w;c++)cell(c,r,' ',FG,BG);
    box(x,y,w,h,"PIKASYS HELP");
    const char *lines[]={
        "0 Overview    1 CPU / memory    2 GPU    3 Processes",
        "Up/Down or j/k select | PgUp/PgDn scroll | Home/End",
        "s cycle sort | r reverse | / search PID, user or command",
        "G GPU processes only | Enter selected process details",
        "[ ] previous/next GPU | < > previous/next CPU page",
        "t cycle theme | a ASCII | C toggle color | w save settings",
        "+ faster refresh | - slower refresh | Space pause/resume",
        "? toggle help | Esc close/clear filter | q quit",
        "",
        "CPU% per process: 100% = one logical CPU; may exceed 100%.",
        "GPU%: NVML SM usage; DRM busiest engine per client (summed).",
        "N/A means unsupported, inaccessible, or not sampled yet.",
        "Shared GPU memory is system memory, not dedicated VRAM.",
        "DRM clients deduplicated across PIDs; visibility is limited",
        "by permissions. Shared buffers can occur in multiple clients.",
        "Linux RAM = total - available; macOS = active+wired+compressed.",
        "Network excludes loopback; includes virtual interfaces.",
        "Linux disk rates: whole backing devices, excludes dm/md/loop.",
        "WSL shows guest-exposed values; macOS GPU keys are best effort.",
        "Read-only monitor: no process killing or hardware control.",
        "",
        "Export: pikasys --json --count 10 --interval 1 > samples.jsonl",
        "Config: pikasys --help | Tests: pikasys --self-test"
    };
    for(size_t i=0;i<ARRAY_LEN(lines) && (int)i<h-2;i++)linef(x+2,y+1+(int)i,w-4,i<8?FG:MUTED,"%s",lines[i]);
}
static void draw_details(void) {
    if(!nvisible)return;
    Process *p=visible[opt.selection];int w=width<100?width:100,h=height<15?height:15,x=(width-w)/2,y=(height-h)/2;
    for(int r=y;r<y+h;r++)for(int c=x;c<x+w;c++)cell(c,r,' ',FG,BG);
    box(x,y,w,h,"PROCESS DETAILS  (Enter / Esc closes)");
    char a[32],b[32];linef(x+2,y+1,w-4,ACCENT,"%s",p->name);
    linef(x+2,y+2,w-4,FG,"PID %d | Parent %d | User %s | State %c | Threads %d",p->pid,p->ppid,p->user,p->state,p->threads);
    metric(p->cpu,"%",a,sizeof a);metric(p->gpu,"%",b,sizeof b);linef(x+2,y+4,w-4,FG,"CPU %s | GPU %s",a,b);
    bytes_text((double)p->rss,a,sizeof a);bytes_text(p->gpu_mem,b,sizeof b);linef(x+2,y+5,w-4,FG,"Resident RAM %s | GPU memory %s",a,b);
    bytes_text(p->read_rate,a,sizeof a);bytes_text(p->write_rate,b,sizeof b);linef(x+2,y+6,w-4,FG,"Disk read %s/s | write %s/s",a,b);
    linef(x+2,y+7,w-4,MUTED,"GPU device bit mask: 0x%"PRIx64,p->gpu_mask);
    int cw=w-4;size_t len=strlen(p->command);
    for(int row=9;row<h-1;row++){size_t offset=(size_t)(row-9)*(size_t)cw;if(offset>=len)break;linef(x+2,y+row,cw,FG,"%s",p->command+offset);}
}
static void draw(Monitor *m) {
    screen_size();list_processes(m);
    if(width<48 || height<18){linef(0,0,width,ACCENT,"pikasys: resize terminal to at least 48 x 18");draw_flush();return;}
    linef(1,0,width-2,ACCENT,"PIKASYS %s   %s   %s%s",VERSION,m->hostname,m->wsl?"WSL":"",opt.paused?" PAUSED":"");
    time_t raw=time(NULL);struct tm tm;localtime_r(&raw,&tm);char clock[16];strftime(clock,sizeof clock,"%H:%M:%S",&tm);
    if(width>85)linef(width-35,0,34,MUTED,"%s  %.1fs  %s",themes[opt.theme].name,opt.interval,clock);
    const char *tabs[]={"0 OVERVIEW","1 CPU","2 GPU","3 PROCESSES"};int tx=1;
    for(int i=0;i<4;i++){text_at(tx,1,i==opt.view?ACCENT:MUTED,BG,tabs[i]);tx+=(int)strlen(tabs[i])+3;}
    if(opt.view==0) {
        int top=height>=32?11:9;
        if(width>=110){int left=width*3/5;draw_cpu(m,0,3,left,top,true);draw_memory(m,left,3,width-left,top);}
        else {
            draw_cpu(m,0,3,width,top-1,false);char a[32],b[32];bytes_text(m->mem_used,a,sizeof a);bytes_text(m->mem_total,b,sizeof b);
            linef(2,3+top-1,width-4,GOOD,"RAM %s / %s | %zu processes | uptime %.1fh",a,b,m->nproc,m->uptime/3600);
        }
        int y=3+top;
        if(height-y>=8){int gh=height-y>=14?7:4;draw_gpu(m,0,y,width,gh);y+=gh;}
        draw_processes(m,y,height-y-2);
    }else if(opt.view==1) {
        int h=height/2;if(h<9)h=9;
        if(width>=110){int left=width*3/5;draw_cpu(m,0,3,left,h,false);draw_memory(m,left,3,width-left,h);}
        else {
            draw_cpu(m,0,3,width,h-2,false);char a[32],b[32],rx[32],txrate[32];
            bytes_text(m->mem_used,a,sizeof a);bytes_text(m->mem_total,b,sizeof b);bytes_text(m->rx,rx,sizeof rx);bytes_text(m->tx,txrate,sizeof txrate);
            linef(2,h+1,width-4,GOOD,"RAM %s / %s | RX %s/s | TX %s/s",a,b,rx,txrate);
        }
        int y=3+h;box(0,y,width,height-y-2,"LOGICAL CPUs  < > page");draw_cores(m,2,y+1,width-4,height-y-4);
    }else if(opt.view==2) {
        int h=height/2;if(h<7)h=7;draw_gpu(m,0,3,width,h);draw_processes(m,3+h,height-h-5);
    }else draw_processes(m,3,height-5);
    if(opt.filtering)linef(1,height-2,width-2,ACCENT,"Search: %s_  (Enter applies, Esc clears)",opt.filter);
    else if(opt.notice[0])linef(1,height-2,width-2,GOOD,"%s",opt.notice);
    else if(opt.filter[0])linef(1,height-2,width-2,ACCENT,"Filter: %s | Esc clears",opt.filter);
    else linef(1,height-2,width-2,MUTED,"%s",m->gpu_status);
    linef(1,height-1,width-2,MUTED,"? help  t theme  s sort  / search  G GPU-only  Space pause  q quit");
    if(opt.help)draw_help();else if(opt.details)draw_details();draw_flush();
}

static const char *role_names[NROLE]={"background","foreground","muted","accent","good","warning","bad","selection"};
static int theme_index(const char *s) {
    for(size_t i=0;i<ARRAY_LEN(themes);i++)if(!strcasecmp(s,themes[i].name))return (int)i;
    return -1;
}
static int sort_index(const char *s) {
    for(int i=0;i<NSORT;i++)if(!strcasecmp(s,sort_names[i]))return i;
    if(!strcasecmp(s,"memory"))return SORT_MEM;
    return -1;
}
static bool interval_value(const char *s,double *out) {
    char *end;errno=0;double n=strtod(s,&end);
    if(end==s || *end || errno || !isfinite(n) || n<0.1 || n>60)return false;
    *out=n;return true;
}
static void config_path(void) {
    const char *xdg=getenv("XDG_CONFIG_HOME"),*home=getenv("HOME");
    if(xdg && *xdg=='/')snprintf(opt.config,sizeof opt.config,"%s/pikasys/config",xdg);
    else if(home && *home=='/')snprintf(opt.config,sizeof opt.config,"%s/.config/pikasys/config",home);
}
static void load_config(void) {
    if(opt.no_config || !opt.config[0])return;
    FILE *f=fopen(opt.config,"r");if(!f)return;
    char line[1024];while(fgets(line,sizeof line,f)) {
        char *s=trim(line);if(!*s || *s=='#')continue;char *eq=strchr(s,'=');if(!eq)continue;*eq=0;
        char *key=trim(s),*value=trim(eq+1);int n;
        if(!strcmp(key,"theme") && (n=theme_index(value))>=0)opt.theme=n;
        else if(!strcmp(key,"interval")){double v;if(interval_value(value,&v))opt.interval=v;}
        else if(!strcmp(key,"sort") && (n=sort_index(value))>=0)opt.sort=n;
        else if(!strcmp(key,"ascii"))opt.ascii=!strcmp(value,"true");
        else if(!strcmp(key,"color"))opt.color=strcmp(value,"false")!=0;
        else if(!strncmp(key,"color.",6))for(int i=0;i<NROLE;i++)if(!strcmp(key+6,role_names[i])) {
            if(*value=='#')value++;
            char *end;unsigned long v=strtoul(value,&end,16);
            if(strlen(value)==6 && !*end && v<=0xffffff){opt.custom[i]=(unsigned)v;opt.custom_set[i]=true;}
        }
    }fclose(f);
}
static bool create_parents(const char *path) {
    char buf[PATH_MAX];copystr(buf,sizeof buf,path);
    for(char *p=buf+1;*p;p++) {
        if(*p=='/') {
            *p=0;if(mkdir(buf,0700)!=0 && errno!=EEXIST)return false;*p='/';
        }
    }
    return true;
}
static void save_config(void) {
    if(opt.no_config || !opt.config[0]){copystr(opt.notice,sizeof opt.notice,"Configuration saving disabled.");return;}
    if(!create_parents(opt.config)){copystr(opt.notice,sizeof opt.notice,"Cannot create configuration directory.");return;}
    char tmp[PATH_MAX];int n=snprintf(tmp,sizeof tmp,"%s.tmp.XXXXXX",opt.config);
    if(n<0 || (size_t)n>=sizeof tmp){copystr(opt.notice,sizeof opt.notice,"Configuration path is too long.");return;}
    int fd=mkstemp(tmp);if(fd<0){copystr(opt.notice,sizeof opt.notice,"Cannot create configuration file.");return;}
    FILE *f=fdopen(fd,"w");if(!f){close(fd);unlink(tmp);return;}
    fprintf(f,"# pikasys user preferences\ntheme=%s\ninterval=%.2f\nsort=%s\nascii=%s\ncolor=%s\n",themes[opt.theme].name,opt.interval,sort_names[opt.sort],opt.ascii?"true":"false",opt.color?"true":"false");
    for(int i=0;i<NROLE;i++)if(opt.custom_set[i])fprintf(f,"color.%s=#%06x\n",role_names[i],opt.custom[i]);
    bool ok=fflush(f)==0;if(ok && fsync(fd))ok=false;if(fclose(f))ok=false;
    if(ok && rename(tmp,opt.config)==0)copystr(opt.notice,sizeof opt.notice,"Preferences saved. Use --help for custom color settings.");
    else {unlink(tmp);copystr(opt.notice,sizeof opt.notice,"Could not save preferences.");}
}
static void print_help(void) {
    puts("pikasys " VERSION " -- live CPU + GPU terminal monitor\n"
        "\nBuild: make\nInstall: sudo make install\nLaunch from any directory: pikasys\n"
        "User install: make install PREFIX=\"$HOME/.local\"; add $HOME/.local/bin to PATH.\n"
        "\nUsage: pikasys [options]\n"
        "  --theme NAME       pikachu (default), dracula, nord, ocean, light\n"
        "  --interval SEC     Sampling period, 0.1 to 60 seconds (default 1)\n"
        "  --sort FIELD       cpu, ram, gpu, pid, name\n"
        "  --filter TEXT      Filter processes by command, user or PID\n"
        "  --gpu-only         Show only processes attributed to a GPU\n"
        "  --no-gpu           Skip GPU probing\n"
        "  --ascii            ASCII graphs and borders\n"
        "  --no-color         Disable color (also honors NO_COLOR)\n"
        "  --json             Newline-delimited JSON to stdout, no terminal needed\n"
        "  --count N          Stop after N samples (works in both modes)\n"
        "  --once             One JSON sample after a short counter warmup\n"
        "  --config PATH      Alternate preference file\n"
        "  --no-config        Do not load or save preferences\n"
        "  --self-test        Built-in parser/calculation regression tests\n"
        "  --version          Print version\n"
        "  --help             This help\n"
        "\nKeys: 0 overview, 1 CPU, 2 GPU, 3 processes; arrows/j/k and PgUp/PgDn;\n"
        "s sort, r reverse, / search, G GPU-only, Enter details, [ ] GPU, < > CPU page;\n"
        "t theme, a ASCII, C color, w save, +/- faster/slower, Space pause, ? help, q quit.\n"
        "\nConfig: $XDG_CONFIG_HOME/pikasys/config or ~/.config/pikasys/config\n"
        "Example:\n  theme=nord\n  interval=1\n  color.accent=#ffd449\n"
        "Color keys: background, foreground, muted, accent, good, warning, bad, selection.\n"
        "CLI settings override the file. 'w' saves explicitly; 't' clears custom colors.\n"
        "\nSupport: Linux/WSL procfs/sysfs; NVIDIA NVML loaded from the installed driver;\n"
        "AMD/Intel DRM fdinfo; macOS Mach/libproc/IOKit. No GPU SDK required to build.\n"
        "The monitor never launches shell commands or requests elevated privileges.\n"
        "Unsupported/inaccessible readings are N/A (null in JSON). Process CPU uses\n"
        "100% per logical CPU. DRM GPU% = sum of each client's busiest engine; this\n"
        "can exceed 100%. Shared DRM clients are attributed to one visible PID.\n"
        "macOS GPU registry counters are best effort and do not offer process GPU\n"
        "attribution. WSL reports only guest-exposed metrics. Apple memory is shared.\n"
        "Linux RAM=total-available; macOS RAM=active+wired+compressed physical pages.\n"
        "Network totals include virtual links, exclude loopback; they can include\n"
        "the same traffic at multiple virtual layers. Disk totals exclude Linux\n"
        "loop/ram/dm/md devices to avoid stacking duplicate physical I/O.\n"
        "\nExamples:\n  pikasys --theme dracula\n  pikasys --json --count 60 > metrics.jsonl\n"
        "  make check\n  make sanitize\n");
}
static bool self_test(void);
static bool parse_options(int argc,char **argv) {
    config_path();
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--no-config"))opt.no_config=true;
        else if(!strcmp(argv[i],"--config")){if(++i>=argc)fail("--config requires a path");copystr(opt.config,sizeof opt.config,argv[i]);}
    }
    load_config();if(getenv("NO_COLOR"))opt.color=false;
    for(int i=1;i<argc;i++) {
        const char *arg=argv[i],*value=NULL;
        if(!strcmp(arg,"--help") || !strcmp(arg,"-h")){print_help();return false;}
        if(!strcmp(arg,"--version")){puts("pikasys " VERSION);return false;}
        if(!strcmp(arg,"--self-test")){exit(self_test()?0:1);}
        if(!strcmp(arg,"--no-config"))continue;
        if(!strcmp(arg,"--config")){i++;continue;}
        if(!strcmp(arg,"--json")){opt.json=true;continue;}
        if(!strcmp(arg,"--once")){opt.json=true;opt.count=1;continue;}
        if(!strcmp(arg,"--ascii")){opt.ascii=true;continue;}
        if(!strcmp(arg,"--no-color")){opt.color=false;continue;}
        if(!strcmp(arg,"--gpu-only")){opt.gpu_only=true;continue;}
        if(!strcmp(arg,"--no-gpu")){opt.no_gpu=true;continue;}
        if(!strcmp(arg,"--theme") || !strcmp(arg,"--interval") || !strcmp(arg,"--sort") || !strcmp(arg,"--filter") || !strcmp(arg,"--count")) {
            if(++i>=argc)fail("option requires a value; see --help");
            value=argv[i];
        }else {fprintf(stderr,"pikasys: unknown option: %s\n",arg);exit(2);}
        if(!strcmp(arg,"--theme")){int n=theme_index(value);if(n<0)fail("unknown theme; see --help");opt.theme=n;memset(opt.custom_set,0,sizeof opt.custom_set);}
        else if(!strcmp(arg,"--interval")){if(!interval_value(value,&opt.interval))fail("interval must be 0.1 to 60 seconds");}
        else if(!strcmp(arg,"--sort")){int n=sort_index(value);if(n<0)fail("unknown sort field; see --help");opt.sort=n;}
        else if(!strcmp(arg,"--filter"))copystr(opt.filter,sizeof opt.filter,value);
        else {char *end;errno=0;opt.count=strtol(value,&end,10);if(end==value || *end || errno || opt.count<1)fail("count must be a positive integer");}
    }return true;
}
static void json_string(const char *s) {
    putchar('"');while(*s){unsigned char c=(unsigned char)*s;
        if(c>=128){uint32_t cp=decode_utf8(&s);if(cp<=0xffff)printf("\\u%04x",cp);else {cp-=0x10000;printf("\\u%04x\\u%04x",0xd800+(cp>>10),0xdc00+(cp&1023));}continue;}
        s++;
        if(c=='"' || c=='\\'){putchar('\\');putchar(c);}else if(c<32 || c==127)printf("\\u%04x",c);else putchar(c);
    }putchar('"');
}
static void json_number(double n) {if(n<0 || !isfinite(n))fputs("null",stdout);else printf("%.3f",n);}
static void json_field(const char *name,double n) {printf(",\"%s\":",name);json_number(n);}
static void print_json(Monitor *m) {
    list_processes(m);struct timespec ts;clock_gettime(CLOCK_REALTIME,&ts);
    printf("{\"schema\":1,\"version\":\"%s\",\"timestamp\":%.3f,\"hostname\":",VERSION,(double)ts.tv_sec+(double)ts.tv_nsec/1e9);
    json_string(m->hostname);fputs(",\"platform\":",stdout);json_string(m->platform);printf(",\"wsl\":%s",m->wsl?"true":"false");
    json_field("sample_seconds",m->elapsed);json_field("uptime_seconds",m->uptime);
    fputs(",\"cpu\":{\"usage_percent\":",stdout);json_number(m->cpu);json_field("temperature_c",m->cpu_temp);
    json_field("frequency_mhz",m->cpu_mhz);json_field("package_power_w",m->cpu_power);
    fputs(",\"cores\":[",stdout);for(size_t i=0;i<m->ncpu;i++){if(i)putchar(',');json_number(m->cpus[i].usage);}fputs("],\"load\":[",stdout);
    for(int i=0;i<3;i++){if(i)putchar(',');json_number(m->load[i]);}fputs("]}",stdout);
    fputs(",\"memory\":{\"used_bytes\":",stdout);json_number(m->mem_used);json_field("total_bytes",m->mem_total);
    json_field("swap_used_bytes",m->swap_used);json_field("swap_total_bytes",m->swap_total);putchar('}');
    fputs(",\"io\":{\"network_rx_bytes_s\":",stdout);json_number(m->rx);json_field("network_tx_bytes_s",m->tx);
    json_field("disk_read_bytes_s",m->disk_read);json_field("disk_write_bytes_s",m->disk_write);json_field("root_used_bytes",m->fs_used);json_field("root_total_bytes",m->fs_total);putchar('}');
    fputs(",\"gpus\":[",stdout);for(size_t i=0;i<m->ngpu;i++) {
        Gpu *g=&m->gpus[i];if(i)putchar(',');printf("{\"index\":%zu,\"id\":",i);json_string(g->id);fputs(",\"name\":",stdout);json_string(g->name);
        fputs(",\"backend\":",stdout);json_string(g->backend);printf(",\"present\":%s,\"shared_memory\":%s,\"process_memory_supported\":%s",g->seen?"true":"false",g->shared?"true":"false",g->proc_supported?"true":"false");
        json_field("usage_percent",g->busy);json_field("memory_activity_percent",g->mem_busy);json_field("memory_used_bytes",g->mem_used);json_field("memory_total_bytes",g->mem_total);
        json_field("temperature_c",g->temp);json_field("power_w",g->watts);json_field("power_limit_w",g->limit);json_field("fan_percent",g->fan);json_field("fan_rpm",g->rpm);
        json_field("clock_mhz",g->clock);json_field("memory_clock_mhz",g->mem_clock);json_field("encoder_percent",g->encoder);json_field("decoder_percent",g->decoder);putchar('}');
    }fputs("],\"processes\":[",stdout);
    for(size_t i=0;i<nvisible;i++) {
        Process *p=visible[i];if(i)putchar(',');printf("{\"pid\":%d,\"ppid\":%d,\"uid\":%u,\"name\":",p->pid,p->ppid,(unsigned)p->uid);json_string(p->name);
        fputs(",\"command\":",stdout);json_string(p->command);fputs(",\"user\":",stdout);json_string(p->user);
        char state[2]={p->state,0};fputs(",\"state\":",stdout);json_string(state);printf(",\"threads\":%d,\"rss_bytes\":%"PRIu64,p->threads,p->rss);
        json_field("cpu_percent",p->cpu);json_field("gpu_percent",p->gpu);json_field("gpu_memory_bytes",p->gpu_mem);
        json_field("read_bytes_s",p->read_rate);json_field("write_bytes_s",p->write_rate);
        printf(",\"gpu_mask\":\"0x%"PRIx64"\"}",p->gpu_mask);
    }
    printf("],\"visible_processes\":%zu,\"total_processes\":%zu,\"permission_denials\":%u,\"gpu_status\":",nvisible,m->nproc,m->denied);json_string(m->gpu_status);puts("}");
    fflush(stdout);
}

enum {KEY_UP=256,KEY_DOWN,KEY_LEFT,KEY_RIGHT,KEY_PGUP,KEY_PGDOWN,KEY_HOME,KEY_END};
static void handle_key(int key,Monitor *m) {
    opt.notice[0]=0;
    if(opt.filtering) {
        size_t n=strlen(opt.filter);
        if(key==27){opt.filter[0]=0;opt.filtering=false;}
        else if(key=='\r' || key=='\n')opt.filtering=false;
        else if(key==127 || key==8){if(n)opt.filter[n-1]=0;}
        else if(key>=32 && key<127 && n<sizeof(opt.filter)-1){opt.filter[n]=(char)key;opt.filter[n+1]=0;}
        opt.selection=opt.scroll=0;focused_pid=0;return;
    }
    if(key=='q' || key==3){stopping=1;return;}
    if(key==27){opt.help=opt.details=false;opt.filter[0]=0;return;}
    if(key=='?'){opt.help=!opt.help;return;}
    if(opt.help)return;
    if(key=='\r' || key=='\n'){opt.details=!opt.details;return;}
    if(key>='0' && key<='3'){opt.view=key-'0';opt.selection=opt.scroll=0;focused_pid=0;}
    else if(key==' '){opt.paused=!opt.paused;}
    else if(key=='t'){opt.theme=(opt.theme+1)%(int)ARRAY_LEN(themes);memset(opt.custom_set,0,sizeof opt.custom_set);repaint=true;}
    else if(key=='a'){opt.ascii=!opt.ascii;repaint=true;}
    else if(key=='C'){opt.color=!opt.color;repaint=true;}
    else if(key=='w')save_config();
    else if(key=='+' || key=='=')opt.interval=clamp(opt.interval/1.25,0.1,60);
    else if(key=='-')opt.interval=clamp(opt.interval*1.25,0.1,60);
    else if(key=='s'){opt.sort=(opt.sort+1)%NSORT;opt.selection=opt.scroll=0;focused_pid=0;}
    else if(key=='r'){opt.reverse=!opt.reverse;opt.selection=opt.scroll=0;focused_pid=0;}
    else if(key=='G'){opt.gpu_only=!opt.gpu_only;opt.selection=opt.scroll=0;focused_pid=0;}
    else if(key=='/'){opt.filtering=true;opt.details=false;}
    else if(key=='[' || key==KEY_LEFT){if(m->ngpu)opt.gpu_index=(opt.gpu_index+(int)m->ngpu-1)%(int)m->ngpu;}
    else if(key==']' || key==KEY_RIGHT){if(m->ngpu)opt.gpu_index=(opt.gpu_index+1)%(int)m->ngpu;}
    else if(key=='<'){if(opt.core_page>0)opt.core_page--;}
    else if(key=='>'){opt.core_page++;}
    else if(key==KEY_UP || key=='k'){if(opt.selection)opt.selection--;focused_pid=0;}
    else if(key==KEY_DOWN || key=='j'){if(opt.selection+1<nvisible)opt.selection++;focused_pid=0;}
    else if(key==KEY_PGUP){opt.selection=opt.selection>(size_t)process_rows?opt.selection-(size_t)process_rows:0;focused_pid=0;}
    else if(key==KEY_PGDOWN){opt.selection+=(size_t)process_rows;focused_pid=0;}
    else if(key==KEY_HOME){opt.selection=0;focused_pid=0;}
    else if(key==KEY_END){opt.selection=nvisible?nvisible-1:0;focused_pid=0;}
}
static bool keyboard(Monitor *m) {
    /* Stateful CSI parser tolerates arrow sequences split across reads. */
    static int state=0;static char seq[32];static size_t n;static double escape_time;
    unsigned char buf[128];ssize_t got=read(STDIN_FILENO,buf,sizeof buf);
    for(ssize_t i=0;i<got;i++) {
        unsigned char c=buf[i];
        if(state==0){if(c==27){state=1;n=0;escape_time=monotime();}else handle_key(c,m);}
        else if(state==1){if(c=='[' || c=='O')state=2;else {handle_key(27,m);state=0;handle_key(c,m);}}
        else {
            if(n<sizeof(seq)-1)seq[n++]=(char)c;
            if(c>=0x40 && c<=0x7e){seq[n]=0;int key=0;
                if(c=='A')key=KEY_UP;else if(c=='B')key=KEY_DOWN;else if(c=='C')key=KEY_RIGHT;else if(c=='D')key=KEY_LEFT;
                else if(c=='H')key=KEY_HOME;else if(c=='F')key=KEY_END;
                else if(!strcmp(seq,"5~"))key=KEY_PGUP;else if(!strcmp(seq,"6~"))key=KEY_PGDOWN;
                else if(!strcmp(seq,"1~") || !strcmp(seq,"7~"))key=KEY_HOME;else if(!strcmp(seq,"4~") || !strcmp(seq,"8~"))key=KEY_END;
                if(key)handle_key(key,m);
                state=0;
            }else if(n==sizeof(seq)-1)state=0;
        }
    }
    if(state && monotime()-escape_time>0.06){state=0;handle_key(27,m);return true;}
    return got>0;
}
static void signal_stop(int sig) {(void)sig;stopping=1;}
static void terminal_init(void) {
    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))fail("interactive mode requires a terminal; use --json or --once");
    const char *term=getenv("TERM");if(term && !strcmp(term,"dumb"))fail("TERM=dumb cannot display the interface; use --json");
    if(tcgetattr(STDIN_FILENO,&saved_term))fail("cannot read terminal settings");
    struct termios raw=saved_term;raw.c_lflag&=(tcflag_t)~(ICANON|ECHO|IEXTEN);raw.c_iflag&=(tcflag_t)~(IXON|ICRNL);
    raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;
    if(tcsetattr(STDIN_FILENO,TCSAFLUSH,&raw))fail("cannot set terminal mode");
    terminal_active=true;atexit(restore_terminal);fputs("\033[?1049h\033[?25l",stdout);fflush(stdout);
}
static bool self_test(void) {
    unsigned tests=0,errors=0;
#define CHECK(expr) do {tests++;if(!(expr)){errors++;fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expr);}}while(0)
    CHECK(fabs(percent(200,80,100,50)-70)<1e-9);CHECK(percent(100,50,100,50)<0);
    CHECK(percent(90,50,100,50)<0);CHECK(rate(50,100,1)<0);CHECK(rate(300,100,2)==100);
    History h={0};for(int i=0;i<300;i++)push(&h,(double)i);CHECK(h.count==HISTORY);CHECK(hist_at(&h,0)==299);CHECK(hist_at(&h,HISTORY-1)==60);CHECK(hist_at(&h,HISTORY)<0);
    CHECK(contains("Python Train","TRAIN"));CHECK(!contains("train","gpu"));
    double interval;CHECK(interval_value("0.1",&interval));CHECK(!interval_value("nan",&interval));CHECK(!interval_value("1junk",&interval));CHECK(!interval_value("0",&interval));
    char text[32];bytes_text(1024,text,sizeof text);CHECK(!strcmp(text,"1.0 KiB"));bytes_text(NA,text,sizeof text);CHECK(!strcmp(text,"N/A"));
    char unsafe[]="evil\033[31m\n";cleanstr(unsafe);CHECK(!strchr(unsafe,27) && !strchr(unsafe,'\n'));
    const char *unicode="\xc3\xbc\xf0\x9f\x98\x80";CHECK(decode_utf8(&unicode)==0xfc);CHECK(decode_utf8(&unicode)==0x1f600);CHECK(!*unicode);
    const char *bad_unicode="\xe0";CHECK(decode_utf8(&bad_unicode)==0xfffd);CHECK(!*bad_unicode);
    Process procs[3]={{.pid=10},{.pid=20},{.pid=30}};CHECK(find_process(procs,3,20)==&procs[1]);CHECK(find_process(procs,3,19)==NULL);
    gpu_process(&procs[0],63,1024,50);CHECK(procs[0].gpu_mask==(UINT64_C(1)<<63));CHECK(procs[0].gpu_mem==1024);CHECK(procs[0].gpu==50);
#ifdef __linux__
    char statline[]="123 (name with ) parens) R 1 2 3 4 5 6 7 8 9 10 120 30 16 17 18 19 4 21 900 4096 8 25";
    Process p={0};CHECK(parse_proc_stat(statline,&p));CHECK(p.pid==123 && p.ppid==1 && p.threads==4);CHECK(p.ticks==150 && p.birth==900);CHECK(!strcmp(p.name,"name with ) parens"));
    char invalid[]="4 broken";CHECK(!parse_proc_stat(invalid,&p));
    char fd[]="drm-client-id: 12\ndrm-pdev: 0000:03:00.0\ndrm-engine-render: 500000000 ns\ndrm-engine-capacity-render: 2\ndrm-memory-vram: 2048 KiB\ndrm-resident-vram: 2048 KiB\n";
    DrmClient a={0};parse_fdinfo(fd,&a);CHECK(a.has_client && a.client==12);CHECK(!strcmp(a.device,"0000:03:00.0"));CHECK(a.memory==2097152);CHECK(a.nengine==1 && a.engines[0].capacity==2);
    DrmClient b=a;b.engines[0].ns=0;CHECK(fabs(drm_usage(&a,&b,1)-25)<1e-9);
    a.engines[0].ns=0;b.engines[0].ns=500;CHECK(drm_usage(&a,&b,1)==0 && a.engines[0].ns==500);
    char xe[]="drm-client-id: 1\ndrm-cycles-rcs: 250\ndrm-total-cycles-rcs: 1000\ndrm-engine-capacity-rcs: 1\n";
    DrmClient c={0};parse_fdinfo(xe,&c);DrmClient d=c;d.engines[0].cycles=d.engines[0].total=0;CHECK(drm_usage(&c,&d,1)==25);
#endif
    printf("pikasys: %u checks, %u failures\n",tests,errors);
#undef CHECK
    return !errors;
}
int main(int argc,char **argv) {
    if(!parse_options(argc,argv))return 0;
    struct sigaction action={0};action.sa_handler=signal_stop;sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,NULL);sigaction(SIGTERM,&action,NULL);sigaction(SIGHUP,&action,NULL);sigaction(SIGPIPE,&action,NULL);
    /* Disable job-control stop in raw mode so Ctrl-Z cannot strand the terminal. */
    if(!opt.json){struct sigaction ignore={0};ignore.sa_handler=SIG_IGN;sigemptyset(&ignore.sa_mask);sigaction(SIGTSTP,&ignore,NULL);terminal_init();}
    Monitor m;monitor_init(&m);sample(&m);
    /* A short baseline interval makes the first visible CPU rate meaningful. */
    poll(NULL,0,150);if(!stopping)sample(&m);
    long emitted=0;double deadline=monotime()+opt.interval;
    if(!stopping){if(opt.json)print_json(&m);else draw(&m);emitted++;}
    while(!stopping && (opt.count<0 || emitted<opt.count)) {
        double now=monotime();
        if(!opt.paused && now>=deadline) {sample(&m);if(opt.json)print_json(&m);else draw(&m);emitted++;deadline=monotime()+opt.interval;continue;}
        int timeout=opt.json?(int)clamp((deadline-now)*1000,1,1000):50;
        struct pollfd input={.fd=STDIN_FILENO,.events=POLLIN};
        int result=poll(opt.json?NULL:&input,opt.json?0:1,timeout);
        if(!opt.json){if(result>0 && (input.revents&(POLLHUP|POLLERR|POLLNVAL)))break;
            bool paused=opt.paused;double interval=opt.interval;bool changed=keyboard(&m);
            if(paused!=opt.paused || interval!=opt.interval)deadline=monotime()+opt.interval;
            struct winsize ws={0};
            bool resized=!ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws) && ws.ws_col && ws.ws_row && ((int)ws.ws_col!=width || (int)ws.ws_row!=height);
            if(changed || resized)draw(&m);
        }
    }
    restore_terminal();monitor_free(&m);free(screen);free(previous);free(visible);return 0;
}
