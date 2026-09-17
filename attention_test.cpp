#include "Vattention.h"
#include "verilated.h"
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
static Vattention d;static int checks=0,jobs=0;
static void ck(bool b,const char*s){checks++;if(!b)throw std::runtime_error(s);}
static void tick(){d.clk=0;d.eval();d.clk=1;d.eval();}
static void wr(int a,uint32_t v,int mask=15){d.req=1;d.addr=a;d.wdata=v;d.wstrb=mask;tick();d.req=0;d.wstrb=0;tick();}
static uint32_t rd(int a){d.addr=a;d.eval();return d.rdata;}
static void reset(){d.resetn=0;d.req=0;d.wstrb=0;tick();d.resetn=1;tick();}
static void bytes(int a,const std::vector<int8_t>& v){
    for(unsigned i=0;i<v.size();i+=4){uint32_t w=0;int mask=0;for(unsigned j=0;j<4&&i+j<v.size();j++){w|=(uint32_t)(uint8_t)v[i+j]<<(8*j);mask|=1<<j;}wr(a+i,w,mask);}
}
static void wait(){int timeout=10000;while(d.busy&&timeout--)tick();ck(timeout>0,"timeout");ck(d.done,"done");tick();ck(d.done,"sticky done");jobs++;}
int main(int argc,char**argv){
    Verilated::commandArgs(argc,argv);std::mt19937 rng(19);
    try{
        for(int dim:{1,3,4,7,16,63,64}){
            reset();wr(8,dim);
            std::vector<std::vector<int8_t>> keys,values;
            for(int t=0;t<32;t++){
                std::vector<int8_t> q(dim),k(dim),v(dim);
                for(int i=0;i<dim;i++){q[i]=(int8_t)rng();k[i]=(int8_t)rng();v[i]=(int8_t)rng();}
                if(t==31)for(int i=0;i<dim;i++){q[i]=-128;k[i]=-128;v[i]=-128;}
                keys.push_back(k);values.push_back(v);
                bytes(0x1000,q);bytes(0x1100,k);bytes(0x1200,v);
                wr(0,1);wait();ck(rd(12)==unsigned(t+1),"KV count");ck(rd(16)==unsigned((dim+3)/4),"append cycles");
                wr(0,2);wait();ck(rd(16)==unsigned((t+1)*((dim+3)/4)),"QK cycles");
                for(int j=0;j<=t;j++){int32_t sum=0;for(int i=0;i<dim;i++)sum+=(int)q[i]*keys[j][i];ck((int32_t)rd(0x2000+4*j)==sum,"QK mismatch");}
                std::vector<uint16_t> a(t+1);for(auto&x:a)x=(uint16_t)rng();a[t]=65535;
                for(int j=0;j<=t;j+=2)wr(0x1300+2*j,(uint32_t)a[j]|(j<t?(uint32_t)a[j+1]<<16:0));
                wr(0,3);wait();ck(rd(16)==unsigned(dim*((t+4)/4)),"AV cycles");
                for(int i=0;i<dim;i++){int32_t sum=0;for(int j=0;j<=t;j++)sum+=(int)a[j]*values[j][i];ck((int32_t)rd(0x3000+4*i)==sum,"AV mismatch/sign");}
                ck(!d.error,"unexpected error");ck(rd(20)==unsigned((t+1)*3),"completed");
            }
            wr(0,1);ck(d.error&&!d.busy&&rd(12)==32,"KV overflow");
            wr(0,0);wr(8,dim);ck(d.error,"dimension changed with cache");
            wr(0,4);ck(rd(12)==0&&!d.error&&!d.done,"clear history");
            wr(0,2);ck(d.error&&!d.busy,"QK empty");wr(0,0);wr(0,3);ck(d.error&&!d.busy,"AV empty");
        }
        reset();wr(8,0);wr(0,1);ck(d.error&&!d.busy,"dimension zero");
        wr(0,0);wr(8,65);wr(0,1);ck(d.error&&!d.busy,"dimension upper bound");
        wr(0,0);wr(8,64);wr(8,7,1);ck(d.error&&rd(8)==64,"partial CSR");
        wr(0,0);wr(0,9);ck(d.error&&!d.busy,"bad command");
        wr(0,0);wr(0x6000,0);ck(d.error,"bad address");
        wr(0,0);std::vector<int8_t> ones(64,1);bytes(0x1100,ones);bytes(0x1200,ones);bytes(0x1000,ones);
        wr(0,1);ck(d.busy,"append active");wr(0,4);wr(0x1100,0);wait();ck(d.error&&rd(12)==1,"busy writes ignored");
        wr(0,2);wait();ck(rd(0x2000)==64&&!d.error,"busy write corrupted K");
        wr(0,1);reset();ck(!d.busy&&!d.done&&!d.error&&rd(12)==0,"reset cancels");
        std::cout<<"{\"passed\":true,\"jobs\":"<<jobs<<",\"checks\":"<<checks<<"}\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<" checks="<<checks<<"\n";return 1;}
}
