/* Real compact/general date parsers and touch entry; metadata and timestamps mocked. */
@dates@
static positive options;
static char *values[128];
static int touch_longs[1];
static file_facts reference;
static bool target_exists;
static bipolar reference_error,create_error,update_error;
static int lookups,creates,updates,closed;
static positive update_flags;
static p64 captured[4];
static bool file_take(file_taking *t){t->flags=options;t->first=1;return true;}
static char *file_option_value(file_taking *t,char c){(void)t;return values[(int)c];}
static int program_argument_count(void){return 2;}
static char *program_argument(int n){(void)n;return "owned-target";}
static b64 file_now(void){return 1767225600;}
static bool file_look_at(char*p,file_facts*out){if(!strcmp(p,"reference")){*out=reference;return !reference_error;}lookups++;return target_exists;}
static bool file_look_link(char*p,file_facts*out){return file_look_at(p,out);}
static bipolar system_open_at_mode(bipolar d,char*p,positive f,positive m){(void)d;(void)p;(void)f;(void)m;creates++;return create_error?create_error:42;}
static bipolar system_close(bipolar d){(void)d;closed++;return 0;}
static bipolar system_update_times_at(bipolar d,char*p,p64*t,positive f){(void)d;(void)p;updates++;memcpy(captured,t,sizeof captured);update_flags=f;return update_error;}
@file_times_of@
@touch_stamp@
@file_touch@
static void reset(void){
 memset(values,0,sizeof values);options=0;reference=(file_facts){.accessed={-1,1,0},.modified={2,999999999,0}};
 target_exists=true;reference_error=create_error=update_error=0;lookups=creates=updates=closed=diagnostics=0;memset(captured,0,sizeof captured);update_flags=0;
}
static void expect(p64 as,p64 an,p64 ms,p64 mn){CHECK(file_touch()==0);CHECK(updates==1);CHECK(captured[0]==as&&captured[1]==an&&captured[2]==ms&&captured[3]==mn);}
int main(void){
 reset();values['r']="reference";expect((p64)-1,1,2,999999999);
 reset();values['r']="reference";options=FILE_FLAG('a');expect((p64)-1,1,2,UTIME_OMIT);
 reset();values['r']="reference";options=FILE_FLAG('m');expect((p64)-1,UTIME_OMIT,2,999999999);
 reset();values['r']="reference";values['d']="1 second";expect(0,1,3,999999999);
 reset();values['r']="reference";values['d']="1 second";options=FILE_FLAG('a');expect(0,1,3,UTIME_OMIT);
 reset();values['r']="reference";values['d']="1 second";options=FILE_FLAG('m');expect(0,UTIME_OMIT,3,999999999);
 reset();values['r']="reference";values['d']="now";expect((p64)-1,1,2,999999999);
 reset();values['r']="reference";values['d']="@-1.5";expect((p64)-2,500000000,(p64)-2,500000000);
 reset();values['r']="reference";values['d']="1970-01-01";expect(0,0,0,0);
 reset();values['r']="reference";values['d']="1970-01-01 00:00:03.123456789";expect(3,123456789,3,123456789);
 reset();values['r']="reference";values['t']="197001010000";expect(0,0,0,0);
 reset();expect(0,UTIME_NOW,0,UTIME_NOW);
 reset();options=FILE_FLAG('a');expect(0,UTIME_NOW,0,UTIME_OMIT);
 reset();values['r']="reference";options=FILE_FLAG('h');expect((p64)-1,1,2,999999999);CHECK(update_flags==AT_SYMLINK_NOFOLLOW);
 reset();target_exists=false;expect(0,UTIME_NOW,0,UTIME_NOW);CHECK(creates==1&&closed==1);
 reset();target_exists=false;options=FILE_FLAG('c');CHECK(file_touch()==0);CHECK(!creates&&!updates);
 reset();reference_error=-13;values['r']="reference";CHECK(file_touch()==1);CHECK(!lookups&&!creates&&!updates);
 reset();target_exists=false;create_error=-13;CHECK(file_touch()==1);CHECK(creates==1&&!updates);
 reset();update_error=-13;CHECK(file_touch()==1);CHECK(updates==1&&diagnostics);
 char *bad[]={"202602310000","202601010000.00junk","202602290000","190002290000","210002290000","202604310000","202600010000","202613010000","202601000000","202601012400","202601010060","202601010000.0","202601010000.","202601010000.000","202601010000.61","202601010000.99","202601010000junk","","1","20260101000000"};
 for(unsigned i=0;i<array_count(bad);i++){reset();values['t']=bad[i];CHECK(file_touch()==1);CHECK(!lookups&&!creates&&!updates);}
 struct {char *stamp;b64 seconds;} good[]={
  {"197001010000",0},{"6901010000",-31536000},{"6801010000",3092601600},{"01010000",1767225600},
  {"200002290000",951782400},{"202402290000",1709164800},{"202602280000",1772236800},{"202604300000",1777507200},
  {"202601010000.60",1767225660},{"202601010000.59",1767225659},{"202601010000.00",1767225600}};
 for(unsigned i=0;i<array_count(good);i++){b64 got;CHECK(touch_stamp(good[i].stamp,file_now(),&got));CHECK(got==good[i].seconds);}
 for(unsigned seconds=0;seconds<100;seconds++){char stamp[32];snprintf(stamp,sizeof stamp,"202601010000.%02u",seconds);b64 got=0;CHECK(touch_stamp(stamp,file_now(),&got)==(seconds<=60));if(seconds<=60)CHECK(got==1767225600+(b64)seconds);}
 positive fraction=123;b64 seconds;CHECK(file_moment_read_exact("1 second",0,&seconds,&fraction));CHECK(seconds==1&&fraction==0);
 printf("touch: %d assertions passed; calendar/date parsing real, filesystem mocked\n",checks);return 0;
}
