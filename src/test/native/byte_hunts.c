// arm64 strchr, strchrnul and strnchr after the prefix fix.
__asm__(
    ".globl _f_strchr\n"
    ".p2align 4\n"
    "_f_strchr:\n"
    "        and     w1, w1, #0xff\n" "        mov     x10, #0x0101010101010101\n" "        mul     x3, x1, x10             // the byte, in all eight positions\n"
    "        and     x4, x0, #7              // how far into the word it begins\n"
    "        bic     x5, x0, #7              // align down: same page, cannot fault\n"
    "        ldr     x6, [x5]\n"
    "        cbz     x4, 1f                  // aligned: nothing sits before the string\n"
    "        lsl     x4, x4, #3              // bytes -> bits\n"
    "        mov     x7, #-1\n" "        lsl     x7, x7, x4              // which bytes of this word count\n"
    "        eor     x8, x6, x3\n"
    "        orn     x8, x8, x7              // the prefix differs from the byte everywhere\n"
    "        orn     x6, x6, x7              // and cannot read as a terminator\n"
    "        b       2f\n"
    "1:      eor     x8, x6, x3              // the byte that matched is now zero\n"
    "2:      sub     x9, x8, x10\n" "        bic     x9, x9, x8\n" "        sub     x12, x6, x10            // and the terminator, the same way\n"
    "        bic     x12, x12, x6\n" "        orr     x9, x9, x12\n" "        and     x9, x9, #0x8080808080808080\n"
    "        cbnz    x9, 3f\n"
    "        add     x5, x5, #8\n" "        ldr     x6, [x5]\n" "        b       1b\n"
    "3:      rbit    x9, x9\n" "        clz     x9, x9                  // first set high bit\n"
    "        add     x9, x5, x9, lsr #3      // the byte or the terminator, whichever\n"
    "        ldrb    w4, [x9]\n" "        cmp     w4, w1\n" "        csel    x0, x9, xzr, eq         // the terminator instead means not found\n"
    "        ret\n"
);
__asm__(
    ".globl _f_strchrnul\n"
    ".p2align 4\n"
    "_f_strchrnul:\n"
    "        and     w1, w1, #0xff\n" "        mov     x10, #0x0101\n" "        movk    x10, #0x0101, lsl #16\n" "        movk    x10, #0x0101, lsl #32\n"
    "        movk    x10, #0x0101, lsl #48   // 0x0101010101010101\n"
    "        lsl     x11, x10, #7            // 0x8080808080808080\n"
    "        mul     x3, x1, x10             // the byte, in all eight positions\n"
    "        and     x4, x0, #7              // how far into the word it begins\n"
    "        bic     x5, x0, #7              // align down: same page, cannot fault\n"
    "        ldr     x6, [x5]\n" "        lsl     x4, x4, #3              // bytes -> bits\n"
    "        mov     x7, #-1\n" "        lsl     x7, x7, x4              // which bytes of the first word count\n"
    "        eor     x8, x6, x3\n"
    "        orn     x8, x8, x7              // the prefix differs from the byte everywhere\n"
    "        orn     x6, x6, x7              // and cannot read as a terminator\n"
    "        b       2f\n"
    "1:      eor     x8, x6, x3              // the byte that matched is now zero\n"
    "2:      sub     x9, x8, x10\n" "        bic     x9, x9, x8\n" "        and     x9, x9, x11\n" "        sub     x12, x6, x10            // and the terminator, the same way\n"
    "        bic     x12, x12, x6\n" "        and     x12, x12, x11\n" "        orr     x9, x9, x12\n"
    "        cbnz    x9, 3f\n"
    "        add     x5, x5, #8\n" "        ldr     x6, [x5]\n" "        b       1b\n"
    "3:      rbit    x9, x9\n" "        clz     x9, x9                  // first set high bit\n"
    "        lsr     x9, x9, #3              // its byte within the word\n"
    "        add     x0, x5, x9\n" "        ret\n"
);
__asm__(
    ".globl _f_strnchr\n"
    ".p2align 4\n"
    "_f_strnchr:\n"
    "        mov     x9, #0\n" "        cbz     x1, 9f\n" "        and     w2, w2, #0xff\n" "        mov     x10, #0x0101\n"
    "        movk    x10, #0x0101, lsl #16\n" "        movk    x10, #0x0101, lsl #32\n" "        movk    x10, #0x0101, lsl #48\n"
    "        lsl     x11, x10, #7\n" "        mul     x3, x2, x10\n" "        add     x13, x0, x1             // one past the last byte we may report\n"
    "        and     x4, x0, #7\n" "        bic     x5, x0, #7\n" "        ldr     x6, [x5]\n" "        lsl     x4, x4, #3\n"
    "        mov     x7, #-1\n" "        lsl     x7, x7, x4\n"
    "        eor     x8, x6, x3\n"
    "        orn     x8, x8, x7              // the prefix differs from the byte everywhere\n"
    "        orn     x6, x6, x7              // and cannot read as a terminator\n"
    "        b       2f\n"
    "1:      eor     x8, x6, x3\n"
    "2:      sub     x9, x8, x10\n" "        bic     x9, x9, x8\n" "        and     x9, x9, x11\n"
    "        sub     x12, x6, x10\n" "        bic     x12, x12, x6\n" "        and     x12, x12, x11\n" "        orr     x9, x9, x12\n"
    "        cbnz    x9, 3f\n" "        add     x5, x5, #8\n"
    "        cmp     x5, x13\n" "        b.hs    8f\n" "        ldr     x6, [x5]\n" "        b       1b\n"
    "3:      rbit    x9, x9\n" "        clz     x9, x9\n" "        lsr     x9, x9, #3\n" "        add     x9, x5, x9\n"
    "        cmp     x9, x13\n" "        b.hs    8f                      // beyond the count\n"
    "        ldrb    w4, [x9]\n" "        cmp     w4, w2\n" "        b.eq    9f                      // it was the byte, not the terminator\n"
    "8:      mov     x9, #0\n"
    "9:      mov     x0, x9\n" "        ret\n"
);

typedef unsigned char u8; typedef unsigned long u64;
char *f_strchr(const char*,int); char *f_strchrnul(const char*,int);
char *f_strnchr(const char*,u64,int);
static char*r_chr(const char*s,int c){for(;;s++){if(*s==(char)c)return(char*)s;if(!*s)return 0;}}
static char*r_nul(const char*s,int c){while(*s&&*s!=(char)c)s++;return(char*)s;}
static char*r_nchr(const char*s,u64 n,int c){for(u64 i=0;i<n;i++){if(s[i]==(char)c)return(char*)(s+i);if(!s[i])return 0;}return 0;}
int printf(const char*,...);
static u64 seed=0x2545F4914F6CDD1Dul;
static u64 next(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
int main(void){
  static u8 a[4096]; u64 ck=0,b1=0,b2=0,b3=0;
  // the adjacency that broke all three
  for(u64 off=1;off<8;off++) for(int c=1;c<256;c++){
    for(u64 i=0;i<off+40;i++) a[i]=0x55;
    a[off-1]=(u8)c; a[off]=(u8)(c+1); a[off+20]=(u8)c; a[off+30]=0;
    ck+=3;
    if(f_strchr((char*)a+off,c)!=r_chr((char*)a+off,c)) b1++;
    if(f_strchrnul((char*)a+off,c)!=r_nul((char*)a+off,c)) b2++;
    if(f_strnchr((char*)a+off,40,c)!=r_nchr((char*)a+off,40,c)) b3++;
    // terminator borrow
    for(u64 i=0;i<off+40;i++) a[i]=0x55;
    a[off-1]=0x00; a[off]=0x01; a[off+20]=(u8)c; a[off+30]=0;
    ck+=3;
    if(f_strchr((char*)a+off,c)!=r_chr((char*)a+off,c)) b1++;
    if(f_strchrnul((char*)a+off,c)!=r_nul((char*)a+off,c)) b2++;
    if(f_strnchr((char*)a+off,40,c)!=r_nchr((char*)a+off,40,c)) b3++;
  }
  // and a broad random sweep, every alignment and length
  for(u64 t=0;t<300000;t++){
    u64 off=next()%64, len=next()%120+1; int c=(int)(next()%255)+1;
    for(u64 i=0;i<off+len+16;i++) a[i]=(u8)(next()%6)+1;
    a[off+len]=0;
    ck+=3;
    if(f_strchr((char*)a+off,c)!=r_chr((char*)a+off,c)) b1++;
    if(f_strchrnul((char*)a+off,c)!=r_nul((char*)a+off,c)) b2++;
    if(f_strnchr((char*)a+off,len,c)!=r_nchr((char*)a+off,len,c)) b3++;
  }
  printf("arm64 after fix: %lu checks | strchr %lu | strchrnul %lu | strnchr %lu\n",ck,b1,b2,b3);
  return (b1||b2||b3)!=0; }
