// arm64 string_find (moonwater_find), copied out of src/library.next.c.
__asm__(
    ".globl _mw_find\n"
    ".p2align 4\n"
    "_mw_find:\n"
    "        ldrb    w3, [x1]\n" "        cbz     w3, 9f                  // an empty input matches nothing\n"
    "        mov     x10, #0x0101\n" "        movk    x10, #0x0101, lsl #16\n" "        movk    x10, #0x0101, lsl #32\n"
    "        movk    x10, #0x0101, lsl #48   // 0x0101010101010101\n"
    "        lsl     x11, x10, #7            // 0x8080808080808080\n"
    "        mul     x2, x3, x10             // it, in all eight positions\n"
    "1:      ldrb    w8, [x0]\n" "        cbz     w8, 9f\n" "        cmp     w8, w3\n" "        b.eq    3f\n"
    "        add     x0, x0, #1\n" "        ldrb    w8, [x0]\n" "        cbz     w8, 9f\n" "        cmp     w8, w3\n"
    "        b.eq    3f\n" "        add     x0, x0, #1\n" "        ldrb    w8, [x0]\n" "        cbz     w8, 9f\n"
    "        cmp     w8, w3\n" "        b.eq    3f\n" "        add     x0, x0, #1\n" "        ldrb    w8, [x0]\n"
    "        cbz     w8, 9f\n" "        cmp     w8, w3\n" "        b.eq    3f\n" "        add     x0, x0, #1\n"
    "        and     x13, x0, #7\n" "        bic     x5, x0, #7              // align down: same page, cannot fault\n"
    "        ldr     x6, [x5]\n" "        lsl     x13, x13, #3\n" "        mov     x7, #-1\n" "        lsl     x7, x7, x13             // which bytes of this word count\n"
    "5:      eor     x8, x6, x2              // the byte that matched is now zero\n"
    "        sub     x9, x8, x10\n" "        bic     x9, x9, x8\n" "        and     x9, x9, x11\n" "        sub     x12, x6, x10            // and the terminator, the same way\n"
    "        bic     x12, x12, x6\n" "        and     x12, x12, x11\n" "        orr     x9, x9, x12\n" "        and     x9, x9, x7\n"
    "        cbnz    x9, 6f\n" "        mov     x7, #-1\n" "        add     x5, x5, #8\n" "        ldr     x6, [x5]\n"
    "        b       5b\n"
    "6:      rbit    x9, x9\n" "        clz     x9, x9                  // first set high bit\n"
    "        lsr     x9, x9, #3              // its byte within the word\n"
    "        add     x0, x5, x9\n" "        ldrb    w8, [x0]\n" "        cbz     w8, 9f                  // the string ended first\n"
    "        cmp     w8, w3\n" "        b.ne    1b\n"
    "3:      mov     x14, x0                 // where this candidate starts\n"
    "        mov     x15, x1\n"
    "4:      ldrb    w8, [x15]\n" "        cbz     w8, 7f                  // input ran out: this is the answer\n"
    "        ldrb    w9, [x0]\n" "        cmp     w9, w8\n" "        b.ne    8f\n"
    "        add     x0, x0, #1\n" "        add     x15, x15, #1\n" "        b       4b\n"
    "8:      add     x0, x14, #1             // one past where this candidate began\n"
    "        b       1b\n"
    "7:      mov     x0, x14\n"
    "        ret\n"
    "9:      mov     x0, #0\n"
    "        ret\n"
);

typedef unsigned char u8; typedef unsigned long u64;
char *mw_find(const char*, const char*);
static char *ref(const char*h,const char*n){
  if(!*n) return (char*)h;
  for(;*h;h++){ const char*a=h,*b=n; while(*a&&*b&&*a==*b){a++;b++;} if(!*b) return (char*)h; }
  return 0; }
int printf(const char*,...);
static u64 seed=0x2545F4914F6CDD1Dul;
static u64 next(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
int main(void){
  static u8 hay[4096]; static u8 nee[64]; u64 checks=0,bad=0;
  // adversarial: a byte just before the haystack equal to the needle's first
  // byte, and the haystack starting with the next value up
  for(u64 off=1; off<8; off++) for(int c=1;c<255;c++){
      for(u64 i=0;i<off+40;i++) hay[i]=0x55;
      hay[off-1]=(u8)c; hay[off]=(u8)(c+1);
      hay[off+18]=(u8)c; hay[off+19]=0x77; hay[off+30]=0;
      nee[0]=(u8)c; nee[1]=0x77; nee[2]=0;
      char*a=mw_find((char*)hay+off,(char*)nee), *b=ref((char*)hay+off,(char*)nee);
      checks++; if(a!=b){ if(bad<3) printf("  FAIL off=%lu c=0x%02x asm=%ld ref=%ld\n",
        off,c,a?(long)((u8*)a-hay-off):-1,b?(long)((u8*)b-hay-off):-1); bad++; } }
  // and a broad random sweep
  for(u64 t=0;t<200000;t++){
    u64 off=next()%32, hl=next()%80+1, nl=next()%5+1;
    for(u64 i=0;i<off+hl;i++) hay[i]=(u8)(next()%4)+'a';
    hay[off+hl]=0;
    for(u64 i=0;i<nl;i++) nee[i]=(u8)(next()%4)+'a';
    nee[nl]=0;
    char*a=mw_find((char*)hay+off,(char*)nee), *b=ref((char*)hay+off,(char*)nee);
    checks++; if(a!=b){ if(bad<4){
        printf("  FAIL off=%lu  needle=\"%s\"  asm=%ld ref=%ld\n  hay=\"%s\"\n",
          off, (char*)nee, a?(long)((u8*)a-hay-off):-1, b?(long)((u8*)b-hay-off):-1, (char*)hay+off);
        printf("  align of hay+off = %lu\n", ((unsigned long)(hay+off))&7ul); }
      bad++; } }
  printf("string_find arm64 after fix: %lu checks, %lu failures\n",checks,bad);
  return bad!=0; }
