#include "policy.hpp"
#include <cassert>
#include <initializer_list>
#include <cstring>
#include <cstdio>
using namespace MatebookUVC;
int main() {
    Request base {Vendor,Product,0xa1,0x81,0x0100,1,34,true,true,false};
    assert(eligible(base));
    auto r=base; r.length=26; assert(eligible(r));
    r=base; r.value=0x0200; assert(eligible(r));
    r=base; r.raw=false; r.descriptor=true; assert(eligible(r));
    for(unsigned len=0;len<512;len++){r=base;r.length=len;assert(eligible(r)==(len==26||len==34));}
    for(unsigned idx=0;idx<256;idx++){r=base;r.index=idx;assert(eligible(r)==(idx==1));}
    r=base;r.vendor=0x04f2;assert(!eligible(r));
    r=base;r.product=0x56dd;assert(!eligible(r));
    r=base;r.type=0x21;assert(!eligible(r));
    r=base;r.request=0x01;assert(!eligible(r));
    r=base;r.value=0x0300;assert(!eligible(r));
    r=base;r.synchronous=false;assert(!eligible(r));
    r=base;r.raw=false;assert(!eligible(r));
    r=base;r.descriptor=true;assert(!eligible(r));
    for(unsigned actual=0;actual<128;actual++)
        assert(safeReply(true,actual,48,34)==(actual==48));
    assert(!safeReply(false,48,48,34));
    assert(!safeReply(true,48,47,34));
    assert(!safeReply(true,48,48,48));
    unsigned char device[48];for(unsigned i=0;i<48;i++)device[i]=i;
    for(unsigned length: {26u,34u}) {
        unsigned char guarded[80];memset(guarded,0xcc,sizeof(guarded));
        assert(safeReply(true,48,sizeof(device),length));
        memcpy(guarded+16,device,length);
        for(unsigned i=0;i<16;i++)assert(guarded[i]==0xcc);
        for(unsigned i=0;i<length;i++)assert(guarded[16+i]==i);
        for(unsigned i=16+length;i<80;i++)assert(guarded[i]==0xcc);
    }
    puts("PASS: target isolation, request lengths, synchronous buffers, exact replies, bounded prefix copy");
}
