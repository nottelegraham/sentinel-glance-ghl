#include "../src/ghl_codec.h"
#include <cstdio>
#include <vector>
static int doserTests();
int main(){
  int fail=0;
  uint8_t tok[8];
  if(!ghl::deriveToken("02:00:00:12:34:56",123456789u,tok)){puts("token parse FAIL");return 1;}
  // token = last four MAC bytes reversed + serial LE (example MAC and serial, not a real unit)
  const uint8_t want_tok[8]={0x56,0x34,0x12,0x00,0x15,0xcd,0x5b,0x07};
  if(memcmp(tok,want_tok,8)){puts("token FAIL");fail++;} else puts("token OK");
  uint8_t f[64]; uint16_t r[1]={ghl::REG_PH};
  size_t n=ghl::buildRead(tok,r,1,f);
  // same layout as a captured pH read: header, length, token, ENQ, count, register 10000, checksum, trailer
  const uint8_t want[]={0x18,0x00,0x00,0xfe,0xff,0x0c,0x00,0x56,0x34,0x12,0x00,0x15,0xcd,0x5b,0x07,0x01,0x01,0x10,0x27,0x3a,0x19};
  if(n!=sizeof want||memcmp(f,want,n)){puts("pH frame FAIL");fail++;} else puts("pH read frame OK");
  // response: pH 831 (2 bytes), tempF 779, rssi -65 (1 byte), alarm 0 (4 bytes)
  std::vector<uint8_t> resp(want_tok,want_tok+8);
  resp.insert(resp.end(),{0x03,4, 0x10,0x27,2,0x3f,0x03, 0x1d,0x27,2,0x0b,0x03, 0x84,0x29,1,0xbf, 0xcc,0x29,4,0,0,0,0});
  int got[4]={0}; int k=0;
  int c=ghl::parseRead(resp.data(),resp.size(),tok,[&](uint16_t id,int32_t v){printf("  reg 0x%04X = %d\n",id,v);got[k++]=v;});
  if(c!=4||got[0]!=831||got[1]!=779||got[2]!=-65||got[3]!=0){puts("parse FAIL");fail++;} else puts("response parse OK (signed values)");
  uint8_t bad[8]={0}; if(ghl::parseRead(resp.data(),resp.size(),bad,[](uint16_t,int32_t){})!=-1){puts("token check FAIL");fail++;}
  if(ghl::parseRead(resp.data(),resp.size()-2,tok,[](uint16_t,int32_t){})!=-1){puts("truncation FAIL");fail++;}
  if(ghl::deriveToken("02:00:00:12:34",1,tok)||ghl::deriveToken("ZZ:00:00:12:34:56",1,tok)){puts("bad MAC FAIL");fail++;}
  fail+=doserTests();
  puts(fail?"FAILED":"ALL PASS"); return fail;
}
// ---- doser decoder ----
static int doserTests(){
  int fail=0; int32_t ml=-1;
  auto D=[&](const char* s,size_t n,const char* f){ml=-1;return doser::decode((const uint8_t*)s,n,f,&ml);};
  // 1000 mL = 0x3E8 → nibbles 8,E,3,0 → "8>30"
  const char a[]="\x02GDHB8>30\x03\x9f\x04";
  if(D(a,sizeof a-1,"GDHB")!=1||ml!=1000){puts("doser 1000 FAIL");fail++;} else puts("doser \"8>30\" = 1000 mL OK");
  // 97,000 mL needs 5 nibbles: 0x17AE8 → 8,E,A,7,1
  const char b[]="\x02MDHB8>:71\x03";
  if(D(b,sizeof b-1,"MDHB")!=1||ml!=97000){puts("doser 97 L FAIL");fail++;} else puts("doser 5-nibble 97,000 mL OK");
  // split across reads: no terminator yet → need more
  if(D(b,7,"MDHB")!=0){puts("doser partial FAIL");fail++;} else puts("doser waits for terminator OK");
  if(D(a,sizeof a-1,"MDHB")!=0){puts("doser wrong field FAIL");fail++;}
  const char c[]="GDHB\x03";
  if(D(c,sizeof c-1,"GDHB")!=-1){puts("doser empty FAIL");fail++;}
  return fail;
}
