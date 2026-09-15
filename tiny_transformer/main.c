// Bare-metal RV32 integer Transformer. No host inference, float library or OS.
#include <stdint.h>
#include "weights.h"
#define MMIO(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define A 0x20000000u
#ifndef NEW_TOKENS
#define NEW_TOKENS 12
#endif
#ifndef USE_ACCEL
#define USE_ACCEL 1
#endif
#ifndef VERIFY_MAC
#define VERIFY_MAC 1
#endif
static int8_t packed[FF] __attribute__((aligned(4)));
static int32_t keys[CONTEXT][DIM],values[CONTEXT][DIM];
static uint32_t states[CONTEXT],projections[CONTEXT*7],state_count,projection_count;
static int32_t score_steps[NEW_TOKENS][VOCAB],output_ids[NEW_TOKENS];
static uint32_t mac_cycles,hardware_cycles,reference_cycles,mac_checks,failures;
static uint32_t cycle(void){uint32_t c;__asm__ volatile("rdcycle %0":"=r"(c)::"memory");return c;}
static void putc_(char c){MMIO(0x10000000)=(uint8_t)c;}
static void puts_(const char*s){while(*s)putc_(*s++);}
static void number(uint32_t n){char a[11];int i=0;do{a[i++]='0'+n%10;n/=10;}while(n);while(i)putc_(a[--i]);}
static void signed_(int32_t v){if(v<0){putc_('-');number(0u-(uint32_t)v);}else number(v);}
static int32_t clip(int32_t v,int lo,int hi){return v<lo?lo:(v>hi?hi:v);}
static uint32_t hash(uint32_t h,int32_t v){return (h^(uint32_t)v)*16777619u;}
static uint32_t isqrt(uint32_t x){
    uint32_t res=0,bit=1u<<30;
    while(bit>x)bit>>=2;
    while(bit){if(x>=res+bit){x-=res+bit;res=(res>>1)+bit;}else res>>=1;bit>>=2;}return res;
}
static void norm(const int32_t*x,int32_t*y){
    uint32_t sum=0;for(int i=0;i<DIM;i++)sum+=(uint32_t)(x[i]*x[i]);
    int32_t den=(int32_t)isqrt(sum/DIM);if(!den)den=1;
    for(int i=0;i<DIM;i++)y[i]=clip(x[i]*32/den,-127,127);
}
#if USE_ACCEL
static void upload(uint32_t dst,const int8_t*src,int n){
    typedef uint32_t word __attribute__((__may_alias__));
    const word*p=(const word*)(const void*)src;
    for(int i=0;i<n/4;i++)MMIO(dst+4*i)=p[i];
}
#endif
static void linear(const int32_t*x,const int8_t*w,int n,int m,int32_t*y){
    uint32_t h=2166136261u;
    for(int i=0;i<n;i++)packed[i]=(int8_t)x[i];
    for(int base=0;base<m;base+=32){
        int rows=m-base<32?m-base:32;int32_t raw[32];
#if USE_ACCEL
        uint32_t begin=cycle();
        upload(A+0x1000,packed,n);upload(A+0x2000,w+base*n,n*rows);
        for(int r=0;r<rows;r++)MMIO(A+0x4000+4*r)=0;
        MMIO(A+8)=n;MMIO(A+12)=rows;MMIO(A+16)=0;MMIO(A)=3;
        uint32_t wait_start=cycle();
        while(!(MMIO(A+4)&2)){
            if((MMIO(A+4)&4)||cycle()-wait_start>1000000u){puts_("{\"error\":\"accelerator timeout/error\"}\n");MMIO(0x10000004)=3;while(1){}}
        }
        for(int r=0;r<rows;r++)raw[r]=(int32_t)MMIO(A+0x5000+4*r);
        mac_cycles+=MMIO(A+0x14);hardware_cycles+=cycle()-begin;
#endif
#if VERIFY_MAC || !USE_ACCEL
        uint32_t ref_start=cycle();
        for(int r=0;r<rows;r++){
            int32_t expected=0;for(int c=0;c<n;c++)expected+=(int32_t)packed[c]*w[(base+r)*n+c];
#if USE_ACCEL
            mac_checks++;if(raw[r]!=expected)failures++;
#else
            raw[r]=expected;
#endif
        }
        reference_cycles+=cycle()-ref_start;
#endif
        for(int r=0;r<rows;r++){h=hash(h,raw[r]);y[base+r]=raw[r]/64;}
    }
    projections[projection_count++]=h;
}
static void forward(int token,int pos,int32_t*logits){
    int32_t x[DIM],z[DIM],q[DIM],k[DIM],v[DIM],ctx[DIM],tmp[DIM],h[FF];
    for(int i=0;i<DIM;i++)x[i]=wt_emb[token*DIM+i]+wt_pos[pos*DIM+i];
    norm(x,z);linear(z,wt_q,DIM,DIM,q);linear(z,wt_k,DIM,DIM,k);linear(z,wt_v,DIM,DIM,v);
    for(int i=0;i<DIM;i++){
        q[i]=clip(q[i],-127,127);k[i]=clip(k[i],-127,127);v[i]=clip(v[i],-127,127);
        keys[pos][i]=k[i];values[pos][i]=v[i];
    }
    int32_t dots[CONTEXT],amax=-2147483647;uint32_t attention[CONTEXT],den=0;
    for(int j=0;j<=pos;j++){
        int32_t a=0;for(int i=0;i<DIM;i++)a+=q[i]*keys[j][i];dots[j]=a;if(a>amax)amax=a;
    }
    for(int j=0;j<=pos;j++){int b=(amax-dots[j])/64;if(b>1024)b=1024;attention[j]=wt_exp_lut[b];den+=attention[j];}
    for(int i=0;i<DIM;i++){
        int32_t a=0;for(int j=0;j<=pos;j++)a+=(int32_t)attention[j]*values[j][i];ctx[i]=a/(int32_t)den;
    }
    linear(ctx,wt_o,DIM,DIM,tmp);for(int i=0;i<DIM;i++)x[i]+=tmp[i];
    norm(x,z);linear(z,wt_up,DIM,FF,h);for(int i=0;i<FF;i++)h[i]=clip(h[i],0,127);
    linear(h,wt_down,FF,DIM,tmp);for(int i=0;i<DIM;i++)x[i]+=tmp[i];
    norm(x,z);linear(z,wt_head,DIM,VOCAB,logits);
    uint32_t digest=2166136261u;
    for(int i=0;i<DIM;i++)digest=hash(digest,x[i]);
    for(int i=0;i<DIM;i++)digest=hash(digest,q[i]);
    for(int i=0;i<DIM;i++)digest=hash(digest,k[i]);
    for(int i=0;i<DIM;i++)digest=hash(digest,v[i]);
    for(int i=0;i<DIM;i++)digest=hash(digest,ctx[i]);
    states[state_count++]=digest;
}
static int token_next(const unsigned char**p){
    uint32_t c=*(*p)++,minimum=0;int count;
    if(c<128)count=0;
    else if(c>=0xc2&&c<=0xdf){c&=31;count=1;minimum=0x80;}
    else if(c>=0xe0&&c<=0xef){c&=15;count=2;minimum=0x800;}
    else if(c>=0xf0&&c<=0xf4){c&=7;count=3;minimum=0x10000;}
    else return -1;
    while(count--){if((**p&0xc0)!=0x80)return -1;c=(c<<6)|(*(*p)++&63);}
    if(c<minimum||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return -1;
    for(int i=3;i<VOCAB;i++)if(codepoints[i]==c)return i;return 2; // valid but unseen Unicode -> UNK
}
int main(void){
    const unsigned char*p=(const unsigned char*)(uintptr_t)0x30004;int ids[CONTEXT],len=1;ids[0]=1;
    if(!*p){puts_("{\"error\":\"empty prompt\"}\n");return 2;}
    while(*p){
        int token=token_next(&p);
        if(token<0){puts_("{\"error\":\"invalid UTF-8\"}\n");return 2;}
        if(len>=CONTEXT-1){puts_("{\"error\":\"prompt too long\"}\n");return 2;}ids[len++]=token;
    }
    uint32_t begin=cycle();int32_t logits[VOCAB];
    for(int i=0;i<len;i++)forward(ids[i],i,logits);
    int count=0;const char*reason="max_new";
    for(int j=0;j<NEW_TOKENS;j++){
        int best=0;for(int i=0;i<VOCAB;i++){score_steps[j][i]=logits[i];if(i!=1&&i!=2&&logits[i]>logits[best])best=i;}
        output_ids[count++]=best;
        if(best==0){reason="eos";break;}
        if(len>=CONTEXT){reason="context";break;}
        if(count==NEW_TOKENS)break;
        ids[len++]=best;forward(best,len-1,logits);
    }
    uint32_t total=cycle()-begin;
    puts_("{\"text\":\"");for(int i=0;i<count;i++)if(output_ids[i]>=3)puts_(vocabulary[output_ids[i]]);
    puts_("\",\"tokens\":[");for(int i=0;i<count;i++){if(i)putc_(',');number(output_ids[i]);}
    puts_("],\"scores\":[");for(int j=0;j<count;j++){if(j)putc_(',');putc_('[');for(int i=0;i<VOCAB;i++){if(i)putc_(',');signed_(score_steps[j][i]);}putc_(']');}
    puts_("],\"state_hashes\":[");for(unsigned i=0;i<state_count;i++){if(i)putc_(',');number(states[i]);}
    puts_("],\"projection_hashes\":[");for(unsigned i=0;i<projection_count;i++){if(i)putc_(',');number(projections[i]);}
    puts_("],\"stop_reason\":\"");puts_(reason);puts_("\",\"forward_tokens\":");number(state_count);
    puts_(",\"match\":");puts_((USE_ACCEL&&VERIFY_MAC)?(failures?"false":"true"):"null");puts_(",\"mac_checks\":");number(mac_checks);
    puts_(",\"accelerator_cycles\":");number(mac_cycles);puts_(",\"hardware_linear_cycles\":");number(hardware_cycles);
    puts_(",\"software_linear_cycles\":");number(reference_cycles);puts_(",\"total_cycles\":");number(total);
    puts_(",\"completed\":");number(MMIO(A+0x18));puts_(",\"use_accelerator\":");number(USE_ACCEL);puts_("}\n");
    return failures?1:0;
}
