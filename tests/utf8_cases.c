#include "aos_utf8.h"
#ifndef HOST_TEST
void *memset(void *dst,int value,size_t n)
{ volatile unsigned char *p=dst; while(n--) *p++=(unsigned char)value; return dst; }
#endif
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static int equal(const char *a,const char *b) { while (*a && *a==*b) { a++;b++; } return *a==*b; }
int test_main(void)
{
    const char *sample="简繁中文Aé😀";
    CHECK(aos_utf8_length(sample)==7);
    CHECK(aos_utf8_prefix_bytes(sample,0)==0);
    CHECK(aos_utf8_prefix_bytes(sample,2)==6);
    CHECK(aos_utf8_prefix_bytes(sample,4)==12);
    CHECK(aos_utf8_prefix_bytes(sample,99)==19);
    const char *p=sample;
    CHECK(aos_utf8_next(&p)==0x7b80);
    CHECK(aos_utf8_next(&p)==0x7e41);
    const char *bad="\xe0\x80\x80x";
    CHECK(aos_utf8_next(&bad)==0xfffd);
    CHECK(aos_utf8_next(&bad)=='x');
    const char *short_sequence="\xe4\xb8";
    CHECK(aos_utf8_next(&short_sequence)==0xfffd);
    CHECK(!*short_sequence);
    const char *surrogate="\xed\xa0\x80";
    CHECK(aos_utf8_next(&surrogate)==0xfffd);
    const char *empty="";
    CHECK(aos_utf8_next(&empty)==0 && !*empty);
    char lines[6][30]={{0}};
    CHECK(aos_utf8_wrap("简繁中文",24,6,(char*)lines,30,6)==2);
    CHECK(equal(lines[0],"简繁") && equal(lines[1],"中文"));
    CHECK(aos_utf8_wrap("hello world!",36,6,(char*)lines,30,6)==2);
    CHECK(equal(lines[0],"hello") && equal(lines[1],"world!"));
    CHECK(aos_utf8_wrap("中文\nEnglish",60,6,(char*)lines,30,6)==2);
    CHECK(equal(lines[0],"中文") && equal(lines[1],"English"));
    CHECK(aos_utf8_wrap("\n中文",60,6,(char*)lines,30,6)==2);
    CHECK(!lines[0][0] && equal(lines[1],"中文"));
    struct { char before;char rows[2][7];char after; } guarded={'a',{{0}},'z'};
    CHECK(aos_utf8_wrap("简繁中文",60,6,(char*)guarded.rows,7,2)==2);
    CHECK(equal(guarded.rows[0],"简繁") && equal(guarded.rows[1],"中文"));
    CHECK(guarded.before=='a' && guarded.after=='z');
    CHECK(aos_utf8_wrap("中文",1,6,(char*)lines,30,6)==2);
    CHECK(equal(lines[0],"中") && equal(lines[1],"文"));
    CHECK(aos_utf8_wrap("中文",60,6,(char*)lines,30,0)==0);
    CHECK(aos_utf8_wrap("中文",60,6,(char*)lines,30,1)==1);
    return 0;
}
#ifdef HOST_TEST
int main(void) { return test_main(); }
#endif
