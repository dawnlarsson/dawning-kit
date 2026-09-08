/* Real stat callback and percent walker; libc integer emitters test signed routing. */
#define log capture_log
static char output[8192];static positive used;
static void capture_log(char *text,positive length){if(!length)length=strlen(text);CHECK(used+length<sizeof output);memcpy(output+used,text,length);used+=length;output[used]=0;}
static void bipolar_to_string(writer w,bipolar n){char b[64];snprintf(b,sizeof b,"%ld",n);w(b,0);}
static void positive_to_string(writer w,positive n){char b[64];snprintf(b,sizeof b,"%lu",n);w(b,0);}
static void positive_to_base_field(writer w,positive n,positive b,positive a,bipolar p,positive f){(void)b;(void)a;(void)p;(void)f;positive_to_string(w,n);}
static bipolar file_link_text(char*p,p8*out,positive n){(void)p;(void)out;(void)n;return -1;}
static void file_mode_letters(p8*out,positive mode){(void)mode;memcpy(out,"----------",11);}
static char *file_kind_told(file_facts*f){(void)f;return "mock-kind";}
static void file_account_label(positive n,bool g,bool named,p8*out){(void)n;(void)g;(void)named;strcpy((char*)out,"mock-account");}
static positive file_device(p32 major,p32 minor){return ((positive)major<<32)|minor;}
static void file_stamp(writer w,b64 seconds,positive ns){char b[96];snprintf(b,sizeof b,"stamp:%"PRId64".%09lu",seconds,ns);w(b,0);}
static char *string_first_of_or_end(char*s,char c){char*p=strchr(s,c);return p?p:s+strlen(s);}
@stat_one_specifier@
@stat_percent_walk@
static void clear(void){used=0;output[0]=0;}
int main(void){
 b64 values[]={INT64_MIN,INT64_MIN+1,-2147483649LL,-2147483648LL,-1,0,1,2147483647LL,2147483648LL,INT64_MAX};
 char *letters="XYZW";
 for(unsigned i=0;i<array_count(values);i++)for(unsigned nano=0;nano<2;nano++)for(unsigned letter=0;letter<4;letter++){
  file_facts f={.mask=STATX_BIRTH,.accessed={values[i],nano?999999999:0,0},.modified={values[i],nano?999999999:0,0},.changed={values[i],nano?999999999:0,0},.created={values[i],nano?999999999:0,0}};
  char expected[64];snprintf(expected,sizeof expected,"%"PRId64,values[i]);clear();stat_one_specifier(letters[letter],"owned",&f);CHECK(!strcmp(output,expected));
 }
 file_facts f={.mask=STATX_BIRTH,.accessed={-1,1,0},.modified={2,2,0},.changed={-3,3,0},.created={-4,4,0},.inode=UINT64_MAX};
 clear();stat_percent_walk("%X:%Y:%Z:%W","owned",&f,stat_one_specifier);CHECK(!strcmp(output,"-1:2:-3:-4\n"));
 f.mask=0;clear();stat_percent_walk("%W:%w","owned",&f,stat_one_specifier);CHECK(!strcmp(output,"0:-\n"));
 clear();stat_percent_walk("a\\tb%%:%X:%i:%n%","owned",&f,stat_one_specifier);CHECK(!strcmp(output,"a\\tb%:-1:18446744073709551615:owned%\n"));
 f.mask=STATX_BIRTH;clear();stat_percent_walk("%x|%y|%z|%w","owned",&f,stat_one_specifier);CHECK(!strcmp(output,"stamp:-1.000000001|stamp:2.000000002|stamp:-3.000000003|stamp:-4.000000004\n"));
 printf("stat: %d assertions passed; all signed epochs, birth availability, distinct mapping and formatting controls\n",checks);return 0;
}
