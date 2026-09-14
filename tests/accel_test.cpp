#include "Vint8_accel.h"
#include "verilated.h"
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

static Vint8_accel dut;
static int checks=0, jobs=0;
static void tick(){dut.clk=0;dut.eval();dut.clk=1;dut.eval();}
static void check(bool yes,const char* what){checks++;if(!yes)throw std::runtime_error(what);}
static void write(uint16_t a,uint32_t d,int strobe=15){dut.req=1;dut.addr=a;dut.wdata=d;dut.wstrb=strobe;tick();dut.req=0;dut.wstrb=0;tick();}
static uint32_t read(uint16_t a){dut.addr=a;dut.eval();return dut.rdata;}
static void reset(){dut.resetn=0;dut.req=0;dut.wstrb=0;tick();tick();dut.resetn=1;tick();}
static void bytes(uint16_t a,const std::vector<int8_t>& data,bool individual){
    for(size_t i=0;i<data.size();i+=4){uint32_t v=0;for(int j=0;j<4&&i+j<data.size();j++)v|=(uint32_t)(uint8_t)data[i+j]<<(j*8);
        if(individual){for(int j=0;j<4&&i+j<data.size();j++)write(a+i,v,1<<j);}
        else write(a+i,v);
    }
}
static void job(int n,int m,bool relu,std::mt19937 &rng,bool busy_error=false,bool extreme=false,bool individual=false){
    std::vector<int8_t>x(n),w(n*m);std::vector<int32_t>b(m),expected(m);
    for(auto &v:x)v=(int8_t)(rng()%256);
    for(auto &v:w)v=(int8_t)(rng()%256);
    if(extreme){for(auto&v:x)v=-128;for(auto&v:w)v=-128;}
    for(int r=0;r<m;r++){
        b[r]=(int32_t)(rng()%200001)-100000;
        if(extreme && r%2)b[r]=2147480000;
        int64_t acc=b[r];for(int c=0;c<n;c++)acc+=(int)x[c]*(int)w[r*n+c];
        int32_t wrapped=(int32_t)(uint32_t)acc;
        expected[r]=relu?(wrapped<0?0:(wrapped>127?127:wrapped)):wrapped;
    }
    bytes(0x1000,x,individual);bytes(0x2000,w,individual);
    for(int r=0;r<m;r++){
        if(individual){for(int j=0;j<4;j++)write(0x4000+4*r,(uint32_t)b[r],1<<j);}
        else write(0x4000+4*r,(uint32_t)b[r]);
    }
    write(8,n);write(12,m);write(16,relu);uint32_t old=read(0x18);write(0,3);
    if(busy_error){check(dut.busy,"must be busy");write(8,1);write(0x2000,0);write(0,1);}
    int timeout=10000;while(dut.busy&&timeout--)tick();
    check(timeout>0,"accelerator timeout");check(dut.done,"done missing");check(bool(dut.error)==busy_error,"error flag mismatch");
    check(read(8)==(uint32_t)n,"busy write changed N");
    check(read(0x14)==(uint32_t)(((n+3)/4)*m),"MAC cycle count mismatch");
    check(read(0x18)==old+1,"completed count mismatch");
    for(int r=0;r<m;r++)check((int32_t)read(0x5000+4*r)==expected[r],"dot product mismatch");
    tick();tick();check(dut.done,"done must be sticky");write(0,2);check(!dut.done&&!dut.error,"clear flags failed");jobs++;
}
int main(int argc,char**argv){
    Verilated::commandArgs(argc,argv);
    try {
        reset();std::mt19937 rng(20260909);
        for(int n: {1,2,3,4,5,7,8,31,32,33,63,127,255,256})
            for(int m:{1,6,32})for(bool relu:{false,true})job(n,m,relu,rng,false,n==256,n==5);
        for(int i=0;i<300;i++)job(1+rng()%256,1+rng()%32,rng()%2,rng);
        job(256,32,false,rng,true);job(256,32,true,rng);
        for(auto nm: {std::pair<int,int>{0,1},{257,1},{1,0},{1,33}}){
            write(8,nm.first);write(12,nm.second);write(0,3);check(dut.error&&!dut.busy,"invalid shape accepted");write(0,2);
        }
        write(8,1);write(12,1);write(16,2);write(0,3);check(dut.error&&!dut.busy,"invalid mode accepted");write(0,2);
        write(8,99,1);check(dut.error&&read(8)==1,"partial CSR write accepted");write(0,2);
        write(0x6000,0);check(dut.error,"invalid address accepted");write(0,2);
        write(8,256);write(12,32);write(16,0);write(0,3);check(dut.busy,"start before reset failed");
        reset();check(!dut.busy&&!dut.done&&!dut.error&&read(0x18)==0,"reset must cancel job");
        job(7,6,true,rng);write(0,6);check(read(0x18)==0,"counter reset failed");
        std::cout<<"{\"jobs\":"<<jobs<<",\"checks\":"<<checks<<",\"passed\":true,\"seed\":20260909}\n";
        return 0;
    }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<" after "<<jobs<<" jobs\n";return 1;}
}
