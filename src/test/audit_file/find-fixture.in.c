/* Full parser/entry/predicate evaluator extracted; traversal, allocation and unrelated predicates mocked. */
#define FIND_BATCH_WORDS 256
#define FIND_BATCH_BYTES 32768
@find_types@
@find_predicates@
static find_node nodes[128],*find_nodes=nodes;
static find_batch batches[4],*find_batches=batches;
static positive find_at,find_count,find_used,find_node_room,find_batch_room,find_batch_have,find_maximum,find_minimum,find_device;
static b32 find_root,find_status;
static bool find_bad,find_has_action,find_deepest,find_one_system,find_follow,find_follow_named,find_quit,find_pruned,shell_memory_failed;
static b64 find_moment;
static file_facts facts,*find_facts=&facts;
static char *find_name="owned",*find_path="owned",*find_entry="owned";
static bipolar find_parent=AT_FDCWD;
static char *args[16];static int argc_now,walks,printed,metadata_calls;
static int program_argument_count(void){return argc_now;}
static char *program_argument(int n){return args[n];}
static b64 file_now(void){return 1767225600;}
#define shell_array_room(a,b,c) ((c)<=array_count(nodes))
#define array_store_reserve(a,b,c,d,e) ((d)<=array_count(batches))
static positive digit_known(p8 c,positive base){positive d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='z'?c-'a'+10:c>='A'&&c<='Z'?c-'A'+10:base;return d<base?d:base;}
@string_digits_checked@
@file_unsigned_decimal@
static bool string_digits_exact(char *s,positive*n){return file_unsigned_decimal(s,n);}
static positive string_table_find(char*w,const void*table,positive stride,positive count){for(positive i=0;i<count;i++)if(!strcmp(w,*(char**)((char*)table+i*stride)))return i;return count;}
static void find_lowered(char*s,p8*out){strcpy((char*)out,s);}
static void find_pattern_prepare(find_node*n){(void)n;}
static bool find_pattern_holds(find_node*n,char*s,bool ic){(void)n;(void)s;(void)ic;return false;}
static bool shell_match(char*a,p8*b){(void)a;(void)b;return false;}
static bipolar file_user_id(char*s){(void)s;return -1;}
static bipolar file_group_id(char*s){(void)s;return -1;}
static bool file_user_name(positive n,p8*s,positive c){(void)n;(void)s;(void)c;return false;}
static bool file_group_name(positive n,p8*s,positive c){(void)n;(void)s;(void)c;return false;}
static bool file_moment_read(char*s,b64 n,b64*out){(void)s;(void)n;(void)out;return false;}
static bipolar file_look_code(bipolar d,char*s,positive f,file_facts*out){(void)d;(void)s;(void)f;metadata_calls++;*out=facts;return 0;}
static bool find_facts_ready(void){metadata_calls++;return true;}
static bipolar file_link_text(char*p,p8*out,positive n){(void)p;(void)out;(void)n;return -1;}
static bool find_type_holds(p8 c,positive m){(void)c;(void)m;return false;}
static bool find_empty(char*p,file_facts*f){(void)p;(void)f;return false;}
static void file_line(char*s){(void)s;printed++;}
#define log capture_log
static void capture_log(char*s,positive n){(void)s;(void)n;printed++;}
static bipolar system_remove_at(bipolar d,char*s,positive f){(void)d;(void)s;(void)f;CHECK(false);return -1;}
static void find_batch_add(find_node*n,char*s){(void)n;(void)s;CHECK(false);}
static void find_batch_run(positive n){(void)n;CHECK(false);}
static bool find_exec_once(find_node*n){(void)n;CHECK(false);return false;}
static void path_tail_copy(p8*out,positive n,char*s){snprintf((char*)out,n,"%s",s);}
@file_mode_adjust@
@file_mode_of@
@find_make@
@find_word@
@find_is@
@find_value@
@find_marked@
@find_holds_count@
@find_size_holds@
@find_age@
static b32 find_parse_or(void);
@find_parse_primary@
@find_parse_and@
@find_parse_or@
@find_true@
static void find_walk(char*p,char*n,positive dep,bool named,bipolar par,char*entry,p8 type){(void)p;(void)n;(void)dep;(void)named;(void)par;(void)entry;(void)type;walks++;find_true(find_root);}
@file_find@
static void setup(char*predicate,char*value){args[0]="find";args[1]="owned";args[2]=predicate;args[3]=value;argc_now=4;walks=printed=metadata_calls=diagnostics=0;memset(nodes,0,sizeof nodes);facts=(file_facts){.mode=MODE_FILE|0644,.size=1,.hard_links=1,.inode=42,.modified={1767225600,0,0}};}
int main(void){
 char *depths[]={"-maxdepth","-mindepth"};char *bad_depth[]={"","+","-","+1","-1","1x","garbage","1.5","18446744073709551616","999999999999999999999999999999999999999"};
 for(unsigned p=0;p<array_count(depths);p++)for(unsigned v=0;v<array_count(bad_depth);v++){setup(depths[p],bad_depth[v]);CHECK(file_find()==1);CHECK(!walks&&!metadata_calls&&diagnostics);}
 for(unsigned p=0;p<array_count(depths);p++){setup(depths[p],"0");CHECK(file_find()==0);CHECK(walks==1);CHECK(p?find_minimum==0:find_maximum==0);setup(depths[p],"18446744073709551615");CHECK(file_find()==0);CHECK(p?find_minimum==positive_max:find_maximum==positive_max);}
 char *counts[]={"-size","-links","-inum","-mtime","-atime","-ctime","-mmin","-amin","-cmin"};
 char *bad_count[]={"","+","-","++1","--1","+-1","-+1","garbage","1garbage","1.5","1 ","1cc","1x","9223372036854775808","18446744073709551615","18446744073709551616"};
 for(unsigned p=0;p<array_count(counts);p++)for(unsigned v=0;v<array_count(bad_count);v++){setup(counts[p],bad_count[v]);CHECK(file_find()==1);CHECK(!walks&&!metadata_calls&&diagnostics);}
 char *good_count[]={"0","1","+1","-1","0001","9223372036854775807","+9223372036854775807","-9223372036854775807"};
 for(unsigned p=0;p<array_count(counts);p++)for(unsigned v=0;v<array_count(good_count);v++){setup(counts[p],good_count[v]);CHECK(file_find()==0);CHECK(walks==1);}
 char *suffixes[]={"b","c","w","k","M","G"};
 for(unsigned s=0;s<array_count(suffixes);s++){char value[16];snprintf(value,sizeof value,"1%s",suffixes[s]);setup("-size",value);CHECK(file_find()==0);CHECK(printed==1);setup("-links",value);CHECK(file_find()==1);CHECK(!walks);}
 setup("-size","+0c");CHECK(file_find()==0);CHECK(printed==1);setup("-size","-2w");CHECK(file_find()==0);CHECK(printed==1);
 char *modes[]={"/0","/000","/a=","-0","-000","-a=","/400","/u=r"};
 for(unsigned m=0;m<array_count(modes);m++){setup("-perm",modes[m]);CHECK(file_find()==0);CHECK(printed==1&&metadata_calls==1);}
 char *no_modes[]={"0","000","a=","/111","/u=x"};
 for(unsigned m=0;m<array_count(no_modes);m++){setup("-perm",no_modes[m]);CHECK(file_find()==0);CHECK(!printed&&metadata_calls==1);}
 setup("-perm","0");facts.mode=MODE_FILE;CHECK(file_find()==0);CHECK(printed==1);
 find_node n={.unit='b',.comparison='+',.number=0};facts.size=UINT64_MAX;CHECK(find_size_holds(&n,&facts));
 printf("find: %d assertions passed; whole entry/parser, numeric validation and permission matching; traversal mocked\n",checks);return 0;
}
