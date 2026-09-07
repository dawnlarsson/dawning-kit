/* Actual Canvas assembly and composition, with ordinary memory instead of DRM. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define HOT
#define PURE
#define CONST
#define true 1
#define false 0
#define static_assert _Static_assert
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define clamp(a,b,c) min(max(a,b),c)
#define WINDOW_CELL_W 8
#define WINDOW_CELL_H 16
static struct {unsigned scale;} desktop={1};
#define canvas_cell_w (8*(int)desktop.scale)
#define canvas_cell_h (16*(int)desktop.scale)
static unsigned long canvas_painted,canvas_runs;
struct drm_rect {int x1,y1,x2,y2;};
struct window_cell {unsigned character; unsigned char ink,paper; unsigned short flags;};
struct font_desc {unsigned width,height; const unsigned char *data;};
static const unsigned char *font_data_buf(const unsigned char *p) {return p;}
static unsigned font_glyph_pitch(unsigned w) {return (w+7)/8;}
static unsigned font_glyph_size(unsigned w,unsigned h) {return font_glyph_pitch(w)*h;}
static unsigned long int_sqrt(unsigned long v) {unsigned long r=0;while((r+1)*(r+1)<=v)r++;return r;}
void memory_fill_u32(u32 *p,unsigned long n,u32 v) {while(n--)*p++=v;}
void *memory_copy_apart(void *to,const void *from,unsigned long n) {return memcpy(to,from,n);}
void canvas_rect_fill(u32 *,unsigned long,unsigned long,unsigned long,u32);
void canvas_glyph(u32 *,unsigned long,const u8 *,unsigned long,unsigned long,u32);
void canvas_cell(u32 *,unsigned long,const u8 *,unsigned long,u32,u32);
#include "canvas-cells.inc"

static unsigned checks;
static void check(int okay) {checks++;assert(okay);}
static u32 pixels[1024*64+2],expected[1024*64+2];
static unsigned char font_bits[256*16];
static void reference_row(const struct target *t,const struct shape *s,int x,int y,
                          const struct window_cell *cells,int used,int first,int last) {
    for(int c=first;c<last;c++) {
        unsigned ch=c<used?cells[c].character:0;
        u32 ink=c<used?canvas_terminal[cells[c].ink&15]|t->opaque:0;
        u32 paper=canvas_terminal[c<used?cells[c].paper&15:0]|t->opaque;
        for(int dy=0;dy<canvas_cell_h;dy++)for(int dx=0;dx<canvas_cell_w;dx++) {
            int px=x+c*canvas_cell_w+dx,py=y+dy;
            if(px<max(t->clip.x1,0)||px>=min(t->clip.x2,t->width)||
               py<max(t->clip.y1,0)||py>=min(t->clip.y2,t->height))continue;
            int inset=round_inset(py-s->y,s->h,s->radius);
            if(px>=s->x+inset && px<s->x+s->w-inset)t->pixels[py*t->pitch+px]=paper;
            if(ch>' ' && ch<=126 &&
               (font_bits[ch*16+dy/desktop.scale]&(0x80>>(dx/desktop.scale))))
                t->pixels[py*t->pitch+px]=ink;
        }
    }
}

struct pane {unsigned grid_columns,columns,rows,view,head,history,stride,skip;
    unsigned *lengths;struct window_cell *cells;};
static unsigned pane_rows(struct pane *p) {return p->rows;}
static unsigned pane_view_at(struct pane *p,unsigned view,unsigned *skip) {*skip=p->skip;return view;}
struct row_call {int y,used,first,last;const struct window_cell *cells;};
static struct row_call calls[32];
static unsigned call_count;
static void record_row(const struct target *t,const struct shape *s,int x,int y,
                       const struct window_cell *cells,int used,int first,int last) {
    (void)t;(void)s;(void)x;assert(call_count<32);
    calls[call_count++]=(struct row_call){y,used,first,last,cells};
}
#define compose_row record_row
#include "canvas-ring.inc"
#undef compose_row

int main(void) {
    const u32 colors[]={0,1,0x80000000,0xffffffff,0xaabbccdd,0x10203040};
    for(unsigned a=0;a<6;a++)for(unsigned b=0;b<6;b++)for(unsigned pattern=0;pattern<256;pattern++) {
        unsigned rows=1+pattern%17,pitch=8+(pattern%10),align=(pattern/10)%2;
        u8 bits[17];for(unsigned r=0;r<rows;r++)bits[r]=(pattern+r*17)&255;
        memset(pixels,0xa5,300*sizeof(*pixels));memset(expected,0xa5,300*sizeof(*expected));
        for(unsigned r=0;r<rows;r++)for(unsigned c=0;c<8;c++)
            expected[align+r*pitch+c]=colors[(bits[r]&(0x80>>c))?a:b];
        canvas_cell(pixels+align,pitch,bits,rows,colors[a],colors[b]);
        check(!memcmp(pixels,expected,300*sizeof(*pixels)));
    }
    struct font_desc face={8,16,font_bits};canvas_font=&face;
    for(unsigned i=0;i<sizeof(font_bits);i++)font_bits[i]=(i*29+i/16*73)&255;
    struct window_cell cells[96];unsigned seed=123;
    for(unsigned trial=0;trial<2400;trial++) {
        desktop.scale=1+trial%2;
        struct target t={.pixels=pixels,.pitch=1024,.width=1024,.height=64,
            .opaque=trial&2?0xff000000:0,.clip={0,0,1024,64}};
        int used=trial%65,last=used+trial%9,first=min((int)(trial%5),used);
        int x=(int)(trial%7)-2,y=(int)(trial%5)-2;
        struct shape s={x,y,last*canvas_cell_w,canvas_cell_h,trial%3?0:4};
        if(trial%4)t.clip=(struct drm_rect){max(x+(int)(trial%13),0),max(y+(int)(trial%3),0),
            min(x+s.w-(int)(trial%7),1024),min(y+s.h-(int)(trial%5),64)};
        for(unsigned i=0;i<96;i++) {seed=seed*1664525+1013904223;
            cells[i]=(struct window_cell){trial%7?32+(seed>>16)%97:' ',trial%3?(seed>>8)&255:7,trial%3?(seed>>24)&255:0,0};}
        memset(pixels,0xa5,sizeof(pixels));memset(expected,0xa5,sizeof(expected));
        compose_row(&t,&s,x,y,cells,used,first,last);
        t.pixels=expected;reference_row(&t,&s,x,y,cells,used,first,last);
        check(!memcmp(pixels,expected,sizeof(pixels)));
    }
    unsigned lengths[8];struct window_cell ring[8*96];
    for(unsigned trial=0;trial<4096;trial++) {
        desktop.scale=1;
        unsigned width=1+trial%32,history=1+trial%8;
        for(unsigned i=0;i<history;i++)lengths[i]=(trial*7+i*37)%97;
        struct pane p={.grid_columns=width,.columns=1+trial%width,.rows=1+trial%16,
            .view=trial%7,.head=trial%7+1+trial%history,.history=history,.stride=96,.lengths=lengths,.cells=ring};
        p.skip=trial%max((lengths[p.view%history]+width-1)/width,1u);
        // A userspace writer can shrink the first line after pane_view_at
        // computed its fold skip. Preserve the old loop's whole-line skip.
        if(trial%7==0){p.skip=95/width;lengths[p.view%history]=0;}
        struct target t={.clip={(int)(trial%3)*8,(int)(trial%17)*16,(int)p.columns*8,(int)(trial%19)*16}};
        struct shape s={0};call_count=0;compose_cells(&p,&t,&s,0,0);
        unsigned line=p.view,fold=p.skip,at=0;
        int first=max(t.clip.x1/8,0),last=min(t.clip.x2/8,(int)p.columns);
        for(unsigned row=0;row<p.rows;row++) {
            const struct window_cell *from=NULL;int used=0;
            while(line!=p.head && fold>=max((lengths[line%history]+width-1)/width,1u)){line++;fold=0;}
            if(line!=p.head) {
                unsigned length=lengths[line%history];
                from=ring+(line%history)*96+fold*width;
                used=min(length>fold*width?length-fold*width:0,width);
                if(++fold==max((length+width-1)/width,1u)){line++;fold=0;}
            }
            if((int)(row*16)<t.clip.y1 || (int)(row*16)>=t.clip.y2 || first>=last)continue;
            check(at<call_count);
            struct row_call got=calls[at++];
            check(got.y==(int)row*16 && got.used==min(used,last) && got.first==first && got.last==last && got.cells==from);
        }
        check(at==call_count);
    }
    printf("  canvas-cells %u of %u\n",checks,checks);
    const char *tally=getenv("TEST_TALLY");
    if(tally){FILE *f=fopen(tally,"a");assert(f);fprintf(f,"canvas-cells %u %u\n",checks,checks);assert(!fclose(f));}
}
