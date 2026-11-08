#include "version_gate.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace MatebookGate;
int main(){
 for(int d=0;d<40;d++) assert(supportedDarwin(d)==(d>=22&&d<=25));
 // Prologue captured from 22A380 and 24H232 Boot KCs (identical first 32 bytes, 0xaa = placeholder).
 const unsigned char ventura[]={0x55,0x48,0x89,0xe5,0x53,0x48,0x83,0xec,0x38,0x44,0x8b,0x55,0x18,0x4c,0x8b,0x5d,0x10,0x48,0xbb};
 assert(prologueMatches(ventura,sizeof(ventura)));
 assert(!prologueMatches(ventura,16));
 assert(!prologueMatches(nullptr,32));
 for(size_t i=0;i<sizeof(ExpectedPrologue);i++){unsigned char m[19];memcpy(m,ventura,19);m[i]^=1;assert(!prologueMatches(m,19));}
 const unsigned char rerouted[]={0xe9,0,0,0,0,0x48,0x83,0xec,0x38,0x44,0x8b,0x55,0x18,0x4c,0x8b,0x5d,0x10};
 assert(!prologueMatches(rerouted,sizeof(rerouted)));
 puts("PASS: Darwin 22-25 only; exact audited prologue; single-byte change, short read, already-routed jump all fail closed");
}
