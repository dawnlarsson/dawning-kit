/* Entire rm entry and recursive bodies are extracted. Every syscall is mocked. */
static bool rm_force,rm_ask,rm_loud,rm_one_system,rm_empty_directories,rm_recursive,rm_careful,rm_preserve_root;
static file_facts rm_root;
static p32 rm_device_major,rm_device_minor;
static b32 rm_status;
static p8 rm_collision_option;
static int rm_longs[1],rm_supersedes[1];
static positive options;
static int collision;
static char *operand;
static bool root_operand,root_opened,prompt,child_present;
static bipolar root_error,operand_error,opened_error,open_error,remove_error,seek_error,read_error,child_error;
static int opens,unlinks,rmdirs,seeks,reads,closes,prompts;
struct linux_dirent64 {char d_name[256];};
typedef struct {bipolar handle,error;positive have,at;p8 block[FILE_BLOCK];} file_walk;
static bool file_take(file_taking *t){t->flags=options;t->first=1;rm_collision_option=collision;return true;}
static int program_argument_count(void){return 2;}
static char *program_argument(int n){(void)n;return operand;}
static int file_missing(char *s){(void)s;return 1;}
static bipolar file_look_code(bipolar d,char *n,positive f,file_facts *out){
 bool root=(!strcmp(n,"/")&&f==0)||(d==42&&!*n&&root_opened)||(d==AT_FDCWD&&f!=0&&root_operand);
 *out=(file_facts){.mode=MODE_DIRECTORY,.device_major=8,.device_minor=1,.inode=root?2:3};
 if(!strcmp(n,"/")&&f==0)return root_error;
 if(d==42&&!*n)return opened_error;
 if(d==42&&!strcmp(n,"leaf")){out->mode=MODE_FILE;return 0;}
 return operand_error;
}
static bool file_look(bipolar d,char*n,positive f,file_facts *out){return file_look_code(d,n,f,out)==0;}
static bool file_is_directory(bipolar d,char*n){file_facts out;return file_look(d,n,AT_SYMLINK_NOFOLLOW,&out)&&(out.mode&MODE_FORMAT)==MODE_DIRECTORY;}
static bool file_ask(char*a,char*b,char*c){(void)a;(void)b;(void)c;prompts++;return prompt;}
static char *rm_wording(file_facts*f){(void)f;return "remove";}
static void rm_said(char*s,bool d){(void)s;(void)d;}
static bipolar system_open_at(bipolar d,char*n,positive f){(void)d;(void)n;(void)f;opens++;return open_error?open_error:42;}
static bipolar system_remove_at(bipolar d,char*n,positive f){
 if(f==AT_REMOVEDIR){rmdirs++;return remove_error;}
 unlinks++;
 if(d==42&&!strcmp(n,"leaf")){if(child_error)return child_error;child_present=false;return 0;}
 return -21;
}
static bipolar system_close(bipolar d){(void)d;closes++;return 0;}
static bipolar system_seek(bipolar d,bipolar o,positive w){(void)d;(void)o;(void)w;seeks++;return seek_error;}
static struct linux_dirent64 *file_walk_next(file_walk *w){
 static struct linux_dirent64 child={"leaf"};reads++;
 if(read_error){w->error=read_error;return 0;}
 if(child_present&&w->at++==0)return &child;
 return 0;
}
static bool file_path_join(p8 *out,char *d,char *n){return snprintf((char*)out,FILE_PATH_MAX,"%s/%s",d,n)<FILE_PATH_MAX;}
static void file_too_long(char*a,char*b,char*c,char*d){(void)a;(void)b;(void)c;(void)d;diagnostics++;}
@file_is_dot@
@file_same_identity@
static bool rm_tree(bipolar,char*,char*,positive);
@rm_contents@
@rm_tree@
@file_rm@
static void reset(void){
 options=FILE_FLAG('r')|FILE_FLAG('f');collision='f';operand="owned";
 root_operand=root_opened=false;prompt=true;child_present=false;
 root_error=operand_error=opened_error=open_error=remove_error=seek_error=read_error=child_error=0;
 opens=unlinks=rmdirs=seeks=reads=closes=prompts=diagnostics=0;
}
int main(void){
 char *aliases[]={"/","//","/.","/mock/..",".","../root-alias"};
 for(unsigned i=0;i<array_count(aliases);i++){reset();operand=aliases[i];root_operand=true;CHECK(file_rm()==1);CHECK(opens==0&&unlinks==0&&rmdirs==0&&reads==0);}
 reset();CHECK(file_rm()==0);CHECK(opens==1&&closes==1&&rmdirs==1);
 reset();root_opened=true;CHECK(file_rm()==1);CHECK(opens==1&&closes==1&&reads==0&&rmdirs==0);
 reset();root_error=-13;CHECK(file_rm()==1);CHECK(opens==0&&unlinks==0);
 reset();opened_error=-13;CHECK(file_rm()==1);CHECK(opens==1&&closes==1&&reads==0&&rmdirs==0);
 reset();root_operand=root_opened=true;options|=FILE_FLAG('N');CHECK(file_rm()==0);CHECK(opens==1&&rmdirs==1); /* Mock only! */
 reset();open_error=-13;CHECK(file_rm()==1);CHECK(diagnostics&&rmdirs==0);
 for(int e=0;e<2;e++){reset();remove_error=e?-39:-13;CHECK(file_rm()==1);CHECK(diagnostics&&rmdirs==1);}
 reset();operand_error=-2;CHECK(file_rm()==0);CHECK(!diagnostics&&!opens&&!unlinks);
 reset();open_error=-2;CHECK(file_rm()==0);CHECK(!diagnostics&&!rmdirs);
 reset();remove_error=-2;CHECK(file_rm()==0);CHECK(!diagnostics&&rmdirs==1);
 reset();collision='i';options=FILE_FLAG('r')|FILE_FLAG('i');prompt=false;CHECK(file_rm()==0);CHECK(prompts==1&&!opens&&!unlinks&&!diagnostics);
 reset();seek_error=-5;CHECK(file_rm()==1);CHECK(diagnostics&&!reads&&closes==1);
 reset();read_error=-5;CHECK(file_rm()==1);CHECK(diagnostics&&reads==1&&closes==1);
 reset();child_present=true;child_error=-13;CHECK(file_rm()==1);CHECK(diagnostics&&child_present&&closes==1);
 reset();child_present=true;CHECK(file_rm()==0);CHECK(!child_present&&rmdirs==1&&closes==1);
 printf("rm: %d assertions passed; all filesystem operations mocked\n",checks);return 0;
}
