// SPDX-License-Identifier: GPL-2.0-only
#include "kernqueue.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>
static_assert(sizeof(kq_request) == 272);
static_assert(sizeof(kq_result) == 24);
using Clock = std::chrono::steady_clock;
struct FD {
    int n;
    explicit FD(int flags = O_RDWR): n(open("/dev/kernqueue", flags | O_CLOEXEC)) {
        if (n < 0) throw std::runtime_error(std::string("open: ") + strerror(errno));
    }
    ~FD() { close(n); }
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;
};
static void require(bool value, const char *msg) {
    if (!value) throw std::runtime_error(msg);
}
static kq_request request(uint64_t id) {
    kq_request r{};
    r.id = id; r.length = 1 + id % KQ_PAYLOAD;
    for (unsigned i=0; i<r.length; ++i) r.data[i] = (id*17+i*31) & 255;
    return r;
}
static uint64_t checksum(const kq_request& r) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned i=0; i<r.length; ++i) h = (h ^ r.data[i]) * 1099511628211ULL;
    return h;
}
static void send(int fd, const kq_request& r) {
    ssize_t n;
    do { n=write(fd,&r,sizeof r); } while(n<0 && errno==EINTR);
    require(n==sizeof r,"write failed");
}
static kq_result receive(int fd) {
    kq_result r{};
    ssize_t n;
    do { n=read(fd,&r,sizeof r); } while(n<0 && errno==EINTR);
    require(n==sizeof r,"read failed");
    return r;
}
static void verify(const kq_result& r) {
    auto expected=request(r.id);
    require(r.length==expected.length && r.checksum==checksum(expected) && !r.reserved,
            "result mismatch");
}
static void expect_error(ssize_t n, int e) {
    require(n==-1 && errno==e,"unexpected errno");
}
static void noop(int) {}
static int tests() {
    unsigned passed=0;
    auto test=[&](const char *name, auto fn) {
        fn(); ++passed; std::cout << "PASS " << name << std::endl;
    };
    test("known FNV-1a vector", [] {
        FD fd; kq_request r{}; r.id=99; r.length=5; memcpy(r.data,"hello",5);
        send(fd.n,r); auto out=receive(fd.n);
        require(out.id==99 && out.checksum==0xa430d84680aabd0bULL,"known vector");
    });
    test("payload lengths 1 through 256 and FIFO order", [] {
        FD fd;
        for(unsigned base=0;base<256;base+=KQ_CAPACITY) {
            for(unsigned i=base;i<base+KQ_CAPACITY;++i) send(fd.n,request(i));
            for(unsigned i=base;i<base+KQ_CAPACITY;++i) {
                auto r=receive(fd.n); require(r.id==i,"FIFO order"); verify(r);
            }
        }
    });
    test("invalid request size and payload bounds", [] {
        FD fd; auto r=request(0);
        expect_error(write(fd.n,&r,sizeof(r)-1),EINVAL);
        expect_error(write(fd.n,&r,0),EINVAL);
        r.length=0; expect_error(write(fd.n,&r,sizeof r),EINVAL);
        r.length=257; expect_error(write(fd.n,&r,sizeof r),EINVAL);
        r.length=1; r.reserved=1; expect_error(write(fd.n,&r,sizeof r),EINVAL);
    });
    test("invalid user pointers preserve completion", [] {
        FD fd; auto r=request(3); send(fd.n,r);
        expect_error(syscall(SYS_read,fd.n,1,sizeof(kq_result)),EFAULT);
        auto out=receive(fd.n); require(out.id==3,"completion lost"); verify(out);
        expect_error(syscall(SYS_write,fd.n,1,sizeof(kq_request)),EFAULT);
    });
    test("short read rejected without consuming result", [] {
        FD fd; auto r=request(4); send(fd.n,r); kq_result out{};
        expect_error(read(fd.n,&out,sizeof(out)-1),EINVAL);
        require(receive(fd.n).id==4,"short read lost result");
    });
    test("nonblocking empty read", [] {
        FD fd(O_RDWR|O_NONBLOCK); kq_result r{};
        expect_error(read(fd.n,&r,sizeof r),EAGAIN);
    });
    test("bounded queue backpressure and writable readiness", [] {
        FD fd(O_RDWR|O_NONBLOCK);
        for(unsigned i=0;i<KQ_CAPACITY;++i) send(fd.n,request(i));
        auto r=request(999); expect_error(write(fd.n,&r,sizeof r),EAGAIN);
        pollfd p{fd.n,POLLOUT,0}; require(poll(&p,1,0)==0,"full queue writable");
        p.events=POLLIN; require(poll(&p,1,2000)==1,"no completion readiness");
        verify(receive(fd.n));
        p.events=POLLOUT; require(poll(&p,1,0)==1,"no slot readiness");
        send(fd.n,r);
    });
    test("blocking read wakes on completion", [] {
        FD fd; auto f=std::async(std::launch::async,[&]{ return receive(fd.n); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        send(fd.n,request(8));
        require(f.wait_for(std::chrono::seconds(2))==std::future_status::ready,"read stuck");
        require(f.get().id==8,"read wake result");
    });
    test("blocking write wakes when a result is consumed", [] {
        FD fd;
        for(unsigned i=0;i<KQ_CAPACITY;++i) send(fd.n,request(i));
        auto f=std::async(std::launch::async,[&]{ send(fd.n,request(999)); });
        require(f.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout,
                "full write failed to block");
        verify(receive(fd.n));
        require(f.wait_for(std::chrono::seconds(2))==std::future_status::ready,"write stuck");
        f.get();
    });
    test("epoll readable completion", [] {
        FD fd; int ep=epoll_create1(EPOLL_CLOEXEC); require(ep>=0,"epoll_create");
        epoll_event event{}; event.events=EPOLLIN; event.data.fd=fd.n;
        require(epoll_ctl(ep,EPOLL_CTL_ADD,fd.n,&event)==0,"epoll_ctl");
        require(epoll_wait(ep,&event,1,0)==0,"empty epoll readable");
        send(fd.n,request(17));
        require(epoll_wait(ep,&event,1,2000)==1 && (event.events&EPOLLIN),"epoll wake");
        verify(receive(fd.n)); close(ep);
    });
    test("independent opens isolate results", [] {
        FD a, b(O_RDWR|O_NONBLOCK); send(a.n,request(19));
        pollfd p{a.n,POLLIN,0}; require(poll(&p,1,2000)==1,"a readiness");
        kq_result r{}; expect_error(read(b.n,&r,sizeof r),EAGAIN);
        require(receive(a.n).id==19,"isolation");
    });
    test("signal interrupts blocking read", [] {
        struct sigaction sa{}; sa.sa_handler=noop; sigemptyset(&sa.sa_mask);
        require(sigaction(SIGUSR1,&sa,nullptr)==0,"sigaction");
        FD fd; std::atomic<bool> begun{false}; int result=0;
        std::thread t([&]{ kq_result r{}; begun=true;
            ssize_t n=read(fd.n,&r,sizeof r); result=(n==-1?errno:0); });
        while(!begun) std::this_thread::yield();
        // Repeat the signal to cover arrival just before the blocking syscall.
        for(int i=0;i<5;++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            pthread_kill(t.native_handle(),SIGUSR1);
        }
        t.join(); require(result==EINTR,"read not interrupted");
    });
    test("four shared-fd threads: no lost or duplicate results", [] {
        FD fd; constexpr unsigned N=2000;
        std::vector<std::atomic<unsigned>> seen(N);
        for(auto& s:seen) s=0;
        std::vector<std::thread> threads; std::atomic<bool> okay{true};
        for(unsigned t=0;t<4;++t) threads.emplace_back([&,t]{
            try { for(unsigned i=0;i<N/4;++i) {
                send(fd.n,request(t*(N/4)+i)); auto r=receive(fd.n); verify(r);
                require(r.id<N,"bad shared id"); ++seen[r.id];
            }} catch(...) { okay=false; }
        });
        for(auto& t:threads) t.join();
        require(okay,"shared thread failure");
        for(auto& s:seen) require(s==1,"lost or duplicate result");
    });
    test("close with outstanding work, 200 cycles", [] {
        for(unsigned j=0;j<200;++j) {
            FD fd; for(unsigned i=0;i<32;++i) send(fd.n,request(i));
        }
    });
    std::cout << "TEST_SUMMARY scenarios=" << passed << " failed=0" << std::endl;
    return 0;
}
struct Run { double seconds; uint64_t jobs; };
static Run throughput(unsigned clients, unsigned each) {
    std::vector<std::thread> threads; std::atomic<bool> okay{true};
    auto start=Clock::now();
    for(unsigned c=0;c<clients;++c) threads.emplace_back([&,c]{
        try { FD fd;
            for(unsigned base=0;base<each;base+=32) {
                unsigned end=std::min(each,base+32);
                for(unsigned i=base;i<end;++i) send(fd.n,request(uint64_t(c)*each+i));
                for(unsigned i=base;i<end;++i) {
                    auto r=receive(fd.n);
                    require(r.id==uint64_t(c)*each+i,"benchmark order"); verify(r);
                }
            }
        } catch(...) { okay=false; }
    });
    for(auto& t:threads) t.join();
    require(okay,"benchmark verification failed");
    return {std::chrono::duration<double>(Clock::now()-start).count(),uint64_t(clients)*each};
}
static int bench() {
    utsname u{}; uname(&u);
    std::cout << "{\"kernel\":\"" << u.release << "\",\"machine\":\"" << u.machine
              << "\",\"payload_bytes\":\"1..256 deterministic\",\"runs\":[";
    bool first=true;
    throughput(1,1000); // warm-up excluded
    for(unsigned clients:{1u,4u}) for(unsigned round=0;round<5;++round) {
        auto r=throughput(clients,5000);
        if(!first) std::cout<<",";
        first=false;
        std::cout << "{\"clients\":"<<clients<<",\"round\":"<<round+1
                  <<",\"verified_jobs\":"<<r.jobs<<",\"seconds\":"<<std::setprecision(9)<<r.seconds
                  <<",\"jobs_per_second\":"<<r.jobs/r.seconds<<"}";
    }
    FD fd; std::vector<double> us;
    for(unsigned i=0;i<1100;++i) {
        auto r=request(i); auto begin=Clock::now(); send(fd.n,r); auto out=receive(fd.n);
        auto end=Clock::now(); require(out.id==i,"latency id"); verify(out);
        if(i>=100) us.push_back(std::chrono::duration<double,std::micro>(end-begin).count());
    }
    std::sort(us.begin(),us.end());
    auto percentile=[&](unsigned p){ return us[(p*us.size()+99)/100-1]; };
    std::cout<<"],\"latency\":{\"samples\":1000,\"p50_us\":"<<percentile(50)
             <<",\"p95_us\":"<<percentile(95)<<",\"p99_us\":"<<percentile(99)
             <<",\"max_us\":"<<us.back()<<"}}"<<std::endl;
    return 0;
}
int main(int argc,char **argv) {
    try {
        std::string command=argc>1?argv[1]:"test";
        if(command=="test") return tests();
        if(command=="bench") return bench();
        if(command=="checksum" && argc==3) {
            std::string s=argv[2]; require(!s.empty() && s.size()<=256,"text must be 1..256 bytes");
            kq_request r{}; r.id=1; r.length=s.size(); memcpy(r.data,s.data(),s.size());
            FD fd; send(fd.n,r); auto out=receive(fd.n);
            std::cout<<std::hex<<out.checksum<<std::endl; return 0;
        }
        throw std::runtime_error("usage: kqctl [test|bench|checksum TEXT]");
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<std::endl; return 1; }
}
