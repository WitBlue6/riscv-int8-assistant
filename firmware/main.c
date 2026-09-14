#include <stdint.h>
#include "model_weights.h"
#define MMIO(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define A 0x20000000u
#ifndef ACCEL_WORD_COPY
#define ACCEL_WORD_COPY 1
#endif
static int8_t input[256] __attribute__((aligned(4)));
static int8_t hidden_sw[32], hidden_hw[32] __attribute__((aligned(4)));
static int32_t logits_sw[6], logits_hw[6], acc_output[32];
static uint32_t known_count, text_count;

static uint32_t cycle(void) { uint32_t c; __asm__ volatile("rdcycle %0":"=r"(c)::"memory");return c; }
static void putc_(char c) { MMIO(0x10000000)= (uint8_t)c; }
static void puts_(const char *s) { while(*s) putc_(*s++); }
static void unsigned_(uint32_t n) { char a[11];int i=0;do { a[i++]='0'+n%10;n/=10; }while(n);while(i)putc_(a[--i]); }
static void signed_(int32_t n) { if(n<0){putc_('-');unsigned_(0u-(uint32_t)n);}else unsigned_((uint32_t)n); }
static void array_(const int32_t *p,int n) { putc_('[');for(int i=0;i<n;i++){if(i)putc_(',');signed_(p[i]);}putc_(']'); }
static uint32_t utf8(const unsigned char **s) {
    uint32_t c=*(*s)++;
    if(c<128)return c;
    int n=c<224?1:(c<240?2:3);c &= n==1?31:(n==2?15:7);
    while(n--){if(!**s)return 0;c=(c<<6)|(*(*s)++ & 63);}return c;
}
static void featurize(const char *text) {
    const unsigned char *p=(const unsigned char *)text;uint32_t prev=0;
    known_count=0;text_count=0;
    for(int i=0;i<256;i++)input[i]=0;
    while(*p){
        uint32_t c=utf8(&p);
        if(c>='A'&&c<='Z')c+=32;
        if(c>='0'&&c<='9')c='#';
        else if(!((c>='a'&&c<='z')||(c>=0x4e00&&c<=0x9fff)))continue;
        text_count++;
        for(unsigned i=0;i<sizeof(known_codepoints)/sizeof(known_codepoints[0]);i++)if(c==known_codepoints[i]){known_count++;break;}
        input[(c*2654435761u)>>24]=1;
        if(prev)input[(((prev*16777619u)^c)*2654435761u)>>24]=1;
        prev=c;
    }
}
static void software(void) {
    for(int r=0;r<32;r++){
        int32_t a=b1[r];
        for(int c=0;c<256;c++)a+=(int32_t)w1[r*256+c]*input[c];
        hidden_sw[r]=(int8_t)(a<0?0:(a>127?127:a));
    }
    for(int r=0;r<6;r++){
        int32_t a=b2[r];for(int c=0;c<32;c++)a+=(int32_t)w2[r*32+c]*hidden_sw[c];logits_sw[r]=a;
    }
}
static void copy_bytes(uint32_t address,const int8_t *src,int count) {
#if ACCEL_WORD_COPY
    // All inference tensors are aligned and have a multiple-of-four byte length.
    // may_alias makes the word access explicit without violating C aliasing rules.
    typedef uint32_t alias_word __attribute__((__may_alias__));
    const alias_word *words=(const alias_word *)(const void *)src;
    for(int i=0;i<count/4;i++)MMIO(address+4u*(uint32_t)i)=words[i];
#else
    for(int i=0;i<count;i+=4){uint32_t v=0;for(int j=0;j<4&&i+j<count;j++)v|=(uint32_t)(uint8_t)src[i+j]<<(8*j);MMIO(address+(uint32_t)i)=v;}
#endif
}
static uint32_t layer(const int8_t *x,const int8_t *w,const int32_t *b,int n,int m,int relu) {
    copy_bytes(A+0x1000,x,n);copy_bytes(A+0x2000,w,n*m);
    for(int i=0;i<m;i++)MMIO(A+0x4000+4*i)=(uint32_t)b[i];
    MMIO(A+8)=n;MMIO(A+12)=m;MMIO(A+16)=relu;MMIO(A)=3;
    while(!(MMIO(A+4)&2)){}
    for(int i=0;i<m;i++)acc_output[i]=(int32_t)MMIO(A+0x5000+4*i);
    return MMIO(A+0x14);
}
static const char *find_(const char *s,const char *needle) {
    for(;*s;s++){const char *a=s,*b=needle;while(*b&&*a==*b){a++;b++;}if(!*b)return s;}return 0;
}
// Limited tool grammar: two non-negative decimal operands <= 100000; integer division.
static int calculate(const char *s,int32_t *answer) {
    uint32_t v[2]={0,0};int count=0;
    if(find_(s,"负"))return 0;
    for(const char *p=s;*p;){if(*p>='0'&&*p<='9'){
        if(count==2)return 0;
        if(count==0)for(const char *q=s;q<p;q++)if(*q=='-')return 0;
        do {v[count]=v[count]*10u+(*p++-'0');if(v[count]>100000)return 0;}while(*p>='0'&&*p<='9');count++;
    }else p++;}
    if(count!=2)return 0;
    int mul=!!(find_(s,"乘")||find_(s,"积")||find_(s,"*"));
    int div=!!(find_(s,"除")||find_(s,"商")||find_(s,"/"));
    int sub=!!(find_(s,"减")||find_(s,"差")||find_(s,"-"));
    int add=!!(find_(s,"加")||find_(s,"和")||find_(s,"+"));
    if(mul+div+sub+add!=1)return 0;
    if(mul){if(v[1]&&v[0]>2147483647u/v[1])return 0;*answer=v[0]*v[1];}
    else if(div){if(!v[1])return 0;*answer=v[0]/v[1];}
    else if(sub)*answer=(int32_t)v[0]-(int32_t)v[1];
    else if(add)*answer=v[0]+v[1];
    else return 0;return 1;
}
int main(void) {
    const char *text=(const char *)(uintptr_t)0x30004;
    uint32_t boot_start=cycle();
    featurize(text);uint32_t feature_cycles=cycle()-boot_start;
    uint32_t start=cycle();software();uint32_t sw_cycles=cycle()-start;
    start=cycle();
    uint32_t core1=layer(input,w1,b1,256,32,1);
    for(int i=0;i<32;i++)hidden_hw[i]=(int8_t)acc_output[i];
    uint32_t core2=layer(hidden_hw,w2,b2,32,6,0);
    for(int i=0;i<6;i++)logits_hw[i]=acc_output[i];
    uint32_t hw_cycles=cycle()-start;
    int match=1;for(int i=0;i<32;i++)if(hidden_hw[i]!=hidden_sw[i])match=0;
    for(int i=0;i<6;i++)if(logits_hw[i]!=logits_sw[i])match=0;
    int best=0;for(int i=1;i<6;i++)if(logits_hw[i]>logits_hw[best])best=i;
    int32_t second=(-2147483647-1);for(int i=0;i<6;i++)if(i!=best&&logits_hw[i]>second)second=logits_hw[i];
    int32_t margin=logits_hw[best]-second;
    int accepted=text_count && known_count*100>=text_count*KNOWN_PERCENT && margin>=MARGIN_THRESHOLD;
    uint32_t before=MMIO(A+0x18);int32_t calc=0;int calc_ok=0;
    if(accepted&&best==2)calc_ok=calculate(text,&calc);
    if(accepted&&best==5)MMIO(A)=6;
    const char *reply="暂未识别，请输入问候、帮助、计算、运行状态、模型信息或清零统计指令。";
    if(accepted)switch(best){
        case 0:reply="你好！我是运行在 RISC-V RTL 仿真 SoC 中的轻量指令助手。";break;
        case 1:reply="支持问候、两整数计算、查询运行状态、模型信息和清零统计。";break;
        case 2:reply=calc_ok?"计算结果见下方，由 RISC-V C 固件执行。":"请输入两个非负整数及一种运算，例如：计算12加30。";break;
        case 3:reply="已读取硬件状态和周期统计，结果见下方。";break;
        case 4:reply="模型为256→32→6的INT8意图分类网络，含8384字节权重和152字节偏置。";break;
        case 5:reply="已通过内存映射寄存器将加速器完成计数清零。";break;
    }
    puts_("{\"intent\":");unsigned_(best);puts_(",\"accepted\":");puts_(accepted?"true":"false");
    puts_(",\"match\":");puts_(match?"true":"false");puts_(",\"margin\":");signed_(margin);
    puts_(",\"reply\":\"");puts_(reply);puts_("\",\"software_cycles\":");unsigned_(sw_cycles);
    puts_(",\"hardware_cycles\":");unsigned_(hw_cycles);puts_(",\"feature_cycles\":");unsigned_(feature_cycles);
    puts_(",\"accelerator_cycles\":");unsigned_(core1+core2);
    puts_(",\"completed_before_tool\":");unsigned_(before);puts_(",\"completed_after_tool\":");unsigned_(MMIO(A+0x18));
    puts_(",\"accel_status\":");unsigned_(MMIO(A+4));puts_(",\"calculation\":");if(calc_ok)signed_(calc);else puts_("null");
    puts_(",\"features\":[");for(int i=0;i<256;i++){if(i)putc_(',');unsigned_(input[i]);}putc_(']');
    puts_(",\"hidden\":[");for(int i=0;i<32;i++){if(i)putc_(',');unsigned_(hidden_hw[i]);}putc_(']');
    puts_(",\"scores\":");array_(logits_hw,6);puts_("}\n");
    return match?0:1;
}
