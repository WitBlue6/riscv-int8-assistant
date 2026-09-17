#include "Vtiled_fc.h"
#include "verilated.h"
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
static Vtiled_fc d;static int checks=0,jobs=0;
static void ck(bool b,const char*s){checks++;if(!b)throw std::runtime_error(s);}
static void tick(){d.clk=0;d.eval();d.clk=1;d.eval();}
static void wr(int a,uint32_t v,int mask=15){d.req=1;d.addr=a;d.wdata=v;d.wstrb=mask;tick();d.req=0;d.wstrb=0;tick();}
static uint32_t rd(int a){d.addr=a;d.eval();return d.rdata;}
static void reset(){d.resetn=0;d.req=0;d.wstrb=0;tick();d.resetn=1;tick();}
static void bytes(int a,const std::vector<int8_t>& v){for(unsigned i=0;i<v.size();i+=4){uint32_t w=0;int mask=0;for(unsigned j=0;j<4&&i+j<v.size();j++){w|=(uint32_t)(uint8_t)v[i+j]<<(8*j);mask|=1<<j;}wr(a+i,w,mask);}}
static void wait(){int n=10000;while(d.busy&&n--)tick();ck(n>0&&d.done,"finish");jobs++;}
int main(int argc,char**argv){
    Verilated::commandArgs(argc,argv);std::mt19937 rng(29);
    try{
        // Full input/output double tiling incl. tail rows/columns and signed wrap.
        for(int n:{1,255,256,257,511,1024,2048})for(int m:{1,31,32,37,65})for(int activation:{0,1}){
            reset();std::vector<int8_t>x(n),w(n*m);std::vector<int32_t>b(m);for(auto&v:x)v=(int8_t)rng();for(auto&v:w)v=(int8_t)rng();for(auto&v:b)v=(int32_t)rng();
            for(int base=0;base<m;base+=32){int rows=std::min(32,m-base);
                for(int first=0;first<n;first+=256){int width=std::min(256,n-first);
                    bytes(0x1000,std::vector<int8_t>(x.begin()+first,x.begin()+first+width));
                    std::vector<int8_t> tile;for(int r=0;r<rows;r++)tile.insert(tile.end(),w.begin()+(base+r)*n+first,w.begin()+(base+r)*n+first+width);bytes(0x2000,tile);
                    for(int r=0;r<rows;r++)wr(0x4000+4*r,(uint32_t)(first?123456:b[base+r])); // ignored on accumulation
                    wr(8,width);wr(12,rows);wr(16,(first?2:0)|((first+width==n)?activation:0));wr(0,3);wait();
                    ck(!d.error,"valid tile error");ck(rd(0x14)==unsigned(rows*((width+3)/4)),"cycles");
                    for(int r=0;r<rows;r++){
                        int64_t full=b[base+r];for(int c=0;c<first+width;c++)full+=(int)x[c]*w[(base+r)*n+c];
                        int32_t expected=(int32_t)(uint32_t)full;
                        if(first+width==n&&activation)expected=expected<0?0:expected>127?127:expected;
                        ck((int32_t)rd(0x5000+4*r)==expected,"partial/final mismatch");
                    }
                }
            }
        }
        reset();wr(8,1);wr(12,1);wr(16,2);wr(0,3);ck(d.error&&!d.busy,"uninitialized accumulate");
        bytes(0x1000,{1});bytes(0x2000,{2});wr(0x4000,3);wr(16,0);wr(0,3);wait();ck(rd(0x5000)==5,"seed bias");
        wr(12,2);wr(16,2);wr(0,3);ck(d.error&&!d.busy,"row shape mismatch");
        wr(12,1);wr(16,1);wr(0,3);wait();wr(16,2);wr(0,3);ck(d.error&&!d.busy,"cannot accumulate clipped result");
        wr(16,4);wr(0,3);ck(d.error&&!d.busy,"mode bounds");
        wr(16,0);wr(8,256);wr(0,3);ck(d.busy,"busy");wr(8,1);ck(d.error&&rd(8)==256,"busy configuration ignored");reset();wr(8,1);wr(12,1);wr(16,2);wr(0,3);ck(d.error&&!d.busy,"reset invalidates partial");
        std::cout<<"{\"passed\":true,\"jobs\":"<<jobs<<",\"checks\":"<<checks<<"}\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<" checks="<<checks<<"\n";return 1;}
}
