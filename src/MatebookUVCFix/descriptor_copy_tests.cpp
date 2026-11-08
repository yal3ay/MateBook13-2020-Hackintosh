#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <initializer_list>
using IOReturn=int; using IOByteCount=uint64_t;
constexpr int kIODirectionIn=1,kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnUnderrun=-2;
#include "descriptor_copy.hpp"
struct Descriptor {
 unsigned char bytes[64]={}; unsigned length=64,direction=1;bool prepared=false;
 int prepareError=0,completeError=0;bool partial=false;unsigned prepares=0,writes=0,completes=0;
 IOByteCount getLength(){return length;} unsigned getDirection(){return direction;}
 IOReturn prepare(int d){assert(d==kIODirectionIn);prepares++;if(prepareError)return prepareError;assert(!prepared);prepared=true;return 0;}
 IOByteCount writeBytes(IOByteCount o,const void*p,IOByteCount n){assert(prepared);assert(o+n<=length);writes++;if(partial)n--;memcpy(bytes+o,p,n);return n;}
 IOReturn complete(int d){assert(prepared&&d==kIODirectionIn);completes++;prepared=false;return completeError;}
};
int main(){unsigned char src[48];memset(src,0x7a,sizeof(src));
 for(unsigned n:{26u,34u}){Descriptor d;assert(MatebookUVC::copyToPreparedDescriptor(&d,src,n)==0);assert(d.prepares==1&&d.writes==1&&d.completes==1&&!d.prepared);assert(!memcmp(d.bytes,src,n)&&d.bytes[n]==0);}
 Descriptor d;d.prepareError=-7;assert(MatebookUVC::copyToPreparedDescriptor(&d,src,34)==-7);assert(d.writes==0&&d.completes==0);
 d=Descriptor{};d.partial=true;assert(MatebookUVC::copyToPreparedDescriptor(&d,src,34)==kIOReturnUnderrun);assert(d.completes==1&&!d.prepared);
 d=Descriptor{};d.completeError=-9;assert(MatebookUVC::copyToPreparedDescriptor(&d,src,34)==-9);assert(d.completes==1);
 d=Descriptor{};d.length=25;assert(MatebookUVC::copyToPreparedDescriptor(&d,src,26)==kIOReturnBadArgument);assert(d.prepares==0);
 d=Descriptor{};d.direction=0;assert(MatebookUVC::copyToPreparedDescriptor(&d,src,26)==kIOReturnBadArgument);assert(d.prepares==0);
 assert(MatebookUVC::copyToPreparedDescriptor<Descriptor>(nullptr,src,26)==kIOReturnBadArgument);
 {Descriptor w;assert(MatebookUVC::descriptorWritable(&w,34));w.direction=0;assert(!MatebookUVC::descriptorWritable(&w,34));w=Descriptor{};w.length=33;assert(!MatebookUVC::descriptorWritable(&w,34));assert(!MatebookUVC::descriptorWritable(&w,0));assert(!MatebookUVC::descriptorWritable<Descriptor>(nullptr,26));
  Descriptor z;assert(MatebookUVC::copyToPreparedDescriptor(&z,nullptr,26)==kIOReturnBadArgument);assert(z.prepares==0);}
 puts("PASS: writability precheck; never write unprepared memory; matched prepare/complete; copy bounds; failure cleanup; direction guard");}
