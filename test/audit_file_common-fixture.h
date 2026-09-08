#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#define address_to *
#define address_of &
#define null 0
#define end 0
#define fn void
#define INLINE
#define PURE
#define CONST
#define RETURNS_NONNULL
#define FILE_FLAG(c) (1ull << ((c)&63))
#define MODE_FORMAT 0170000
#define MODE_DIRECTORY 0040000
#define MODE_FILE 0100000
#define MODE_LINK 0120000
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 256
#define AT_REMOVEDIR 512
#define AT_EMPTY_PATH 4096
#define FILE_READ 0
#define FILE_WRITE 577
#define O_TRUNC 512
#define O_DIRECTORY 65536
#define FILE_MAX_DEPTH 64
#define FILE_PATH_MAX 4096
#define FILE_NAME_MAX 256
#define FILE_BLOCK 4096
#define FILE_SEEK_SET 0
#define ERROR_NO_ENTRY 2
#define ERROR_NOT_EMPTY 39
#define UTIME_NOW 0x3fffffff
#define UTIME_OMIT 0x3ffffffe
#define STATX_BIRTH 0x800
#define CLOCK_SECONDS_PER_DAY 86400
#define positive_max ULONG_MAX
#define b64_max INT64_MAX
#define array_count(x) (sizeof(x)/sizeof((x)[0]))
typedef unsigned long positive; typedef long bipolar; typedef int b32;
typedef unsigned char p8; typedef uint16_t p16; typedef uint32_t p32;
typedef uint64_t p64; typedef int64_t b64; typedef char *string_address;
typedef void *address_any;
typedef void (*writer)(string_address,positive);
@facts@
typedef struct {char *program,*allowed,*valued;void *longs,*supersedes;positive flags,first;} file_taking;
static int diagnostics;
static void log_error(char *text,positive length){(void)text;(void)length;diagnostics++;}
static void mock_format(writer out,char *fmt,va_list ap){char b[8192];vsnprintf(b,sizeof b,fmt,ap);out(b,0);}
static void string_format(writer out,char *fmt,...){va_list ap;va_start(ap,fmt);mock_format(out,fmt,ap);va_end(ap);}
static b32 string_report(writer out,b32 result,char *fmt,...){va_list ap;va_start(ap,fmt);mock_format(out,fmt,ap);va_end(ap);return result;}
static char *file_reason(bipolar e){(void)e;return "mock error";}
static bool string_is(char *s,char c){return *s==c;}
static p8 string_get(char *s){return (p8)*s;}
#define string_compare strcmp
#define string_length strlen
#define memory_fill memset
static void log_flush(void){}
static int checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAILED %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
/* libc adapters implement basic byte helpers, not the algorithms under test. */
static positive string_digits(char *s,positive *taken){positive v=0,n=0;while(s[n]>='0'&&s[n]<='9'){v=v*10+(s[n]-'0');n++;}if(taken)*taken=n;return v;}
#define byte_is_digit(c) ((c)>='0'&&(c)<='9')
#define byte_is_alpha(c) (isalpha((unsigned char)(c))!=0)
#define byte_to_lower(c) ((p8)tolower((unsigned char)(c)))
#define memory_compare_ascii_case strncasecmp
#define string_span strspn
static char *string_set_blanks=" \t\n\r\f\v";
