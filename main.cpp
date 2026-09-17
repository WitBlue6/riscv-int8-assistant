#include "Vsoc.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    VerilatedContext context;context.commandArgs(argc,argv);
    std::string text="你好", trace;
    for(int i=1;i<argc;i++) {
        if(std::string(argv[i])=="--text" && i+1<argc) text=argv[++i];
        else if(std::string(argv[i])=="--trace" && i+1<argc) trace=argv[++i];
    }
    if(text.size()>255 || text.find('\0')!=std::string::npos) {std::cerr<<"Input must be <=255 UTF-8 bytes without NUL\n";return 2;}
    context.traceEverOn(!trace.empty());
    auto top=std::make_unique<Vsoc>(&context);
    auto wave=std::make_unique<VerilatedVcdC>();
    if(!trace.empty()){top->trace(wave.get(),2);wave->open(trace.c_str());}
    auto tick=[&](){
        top->clk=0;top->eval();if(!trace.empty())wave->dump(context.time());context.timeInc(5000);
        top->clk=1;top->eval();if(!trace.empty())wave->dump(context.time());context.timeInc(5000);
        if(top->console_valid) std::cout.put((char)top->console_data);
    };
    top->resetn=0;top->host_we=0;tick();tick();
    std::vector<unsigned char> bytes(260,0);
    bytes[0]=(unsigned char)text.size();
    for(size_t i=0;i<text.size();i++)bytes[i+4]=(unsigned char)text[i];
    for(size_t i=0;i<bytes.size();i+=4){
        top->host_we=1;top->host_word=(0x30000+i)/4;
        top->host_data=(uint32_t)bytes[i]|((uint32_t)bytes[i+1]<<8)|((uint32_t)bytes[i+2]<<16)|((uint32_t)bytes[i+3]<<24);tick();
    }
    top->host_we=0;tick();top->resetn=1;
    uint64_t clocks=0;
    while(!context.gotFinish()&&!top->finished&&!top->trap&&clocks<2000000000ull){tick();clocks++;}
    int rc=top->finished?(int)top->exit_code:3;
    if(top->trap){std::cerr<<"CPU trap after "<<clocks<<" cycles\n";rc=4;}
    if(!top->finished&&!top->trap)std::cerr<<"Simulation timeout\n";
    top->final();if(!trace.empty())wave->close();std::cout.flush();return rc;
}
