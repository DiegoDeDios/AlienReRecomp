/* Fractional transform shadows before projection. Integer guest execution
 * remains authoritative; every shadow is validated against its raw value. */
#include "transform.h"
#include "psx.h"
#include <math.h>
#include <limits.h>

static TransformWord t_gpr[32], t_data[32], t_ctrl[32];
static TransformWord t_ram[0x200000/4], t_scratch[0x400/4];
void transform_reset(void) {
    memset(t_gpr,0,sizeof t_gpr);memset(t_data,0,sizeof t_data);
    memset(t_ctrl,0,sizeof t_ctrl);memset(t_ram,0,sizeof t_ram);
    memset(t_scratch,0,sizeof t_scratch);
}
void transform_registers_save(TransformRegisters *p) {
    memcpy(p->gpr,t_gpr,sizeof t_gpr);memcpy(p->data,t_data,sizeof t_data);
    memcpy(p->ctrl,t_ctrl,sizeof t_ctrl);
}
void transform_registers_restore(const TransformRegisters *p) {
    memcpy(t_gpr,p->gpr,sizeof t_gpr);memcpy(t_data,p->data,sizeof t_data);
    memcpy(t_ctrl,p->ctrl,sizeof t_ctrl);
}
static TransformWord *transform_slot(uint32_t a) {
    a&=0x1fffffff;
    if(a<0x800000) return &t_ram[(a&0x1ffffc)>>2];
    if((a&0xfffffc00)==0x1f800000) return &t_scratch[(a&0x3fc)>>2];
    return NULL;
}
static TransformWord transform_integer(uint32_t raw) {
    return (TransformWord){(int16_t)raw,(int16_t)(raw>>16),raw,true};
}
static double scalar(TransformWord t) {
    return t.lo+((int16_t)t.raw<0?65536.0:0)+65536.0*t.hi;
}
static TransformWord scalar_word(double value,uint32_t raw) {
    if(!isfinite(value) || value<INT_MIN || value>INT_MAX)return transform_integer(raw);
    return (TransformWord){(int16_t)raw+value-(int32_t)raw,(int16_t)(raw>>16),raw,true};
}
static bool render_add(uint32_t pc) {
    pc&=0x1fffffff;
    return (pc>=0x20544 && pc<0x22290) || (pc>=0x308b0 && pc<0x30ae8) ||
           (pc>=0x6215c && pc<0x6250c) || (pc>=0x82d24 && pc<0x83300);
}
/* Object renderers compose root/bone translations with the camera in
 * several builders (static, animated and repeated ladder sections). Their
 * integer ADDU used to invalidate the fractional MVMVA result just before
 * CTC2. Recognize only these arithmetic sites and their original opcodes;
 * unrelated guest arithmetic keeps its existing invalidation rules. */
static bool object_translation(uint32_t pc,uint32_t w) {
    switch(pc&0x1fffffff) {
    case 0x22388: return w==0x00431021u;
    case 0x223a0: return w==0x00641821u;
    case 0x223a4: return w==0x00451021u;
    case 0x2269c: return w==0x00431021u;
    case 0x226b0: return w==0x00621821u;
    case 0x226c4: return w==0x00431021u;
    case 0x227cc: return w==0x00431021u;
    case 0x227e4: return w==0x00621821u;
    case 0x227fc: return w==0x00431021u;
    case 0x22b58: return w==0x00431021u;
    case 0x22b74: return w==0x00621821u;
    case 0x22b90: return w==0x00431021u;
    case 0x23050: return w==0x00431021u;
    case 0x2306c: return w==0x00621821u;
    case 0x23088: return w==0x00431021u;
    case 0x23a58: return w==0x00431021u;
    case 0x23a7c: return w==0x00641821u;
    case 0x23a80: return w==0x00451021u;
    case 0x22544: return w==0x00641823u;
    default: return false;
    }
}
static bool render_arithmetic(uint32_t pc,uint32_t w) {
    return render_add(pc) || object_translation(pc,w);
}
static bool rounded_position(uint32_t pc) {
    pc&=0x1fffffff;
    return pc==0x20ebc || pc==0x20ed8 || pc==0x20ef4 ||
           pc==0x21ba8 || pc==0x21bb8 || pc==0x21bc8 ||
           pc==0x21c78 || pc==0x21c80 || pc==0x21c9c ||
           pc==0x2232c || pc==0x2233c || pc==0x2234c ||
           pc==0x23654 || pc==0x23664 || pc==0x23674 || pc==0x239cc;
}
static bool root_position_unround(uint32_t pc) {
    pc&=0x1fffffff;
    return pc==0x21e40 || pc==0x21e4c || pc==0x21e68 ||
           pc==0x22514 || pc==0x2253c || pc==0x2254c;
}
static bool camera_origin_fixed(uint32_t pc) {
    pc&=0x1fffffff;
    return pc==0x22224 || pc==0x2223c || pc==0x22240;
}
static TransformWord checked(TransformWord t,uint32_t raw) {
    return t.valid && t.raw==raw?t:transform_integer(raw);
}
double transform_gpr32(int reg,uint32_t raw) { return scalar(checked(t_gpr[reg],raw)); }
void transform_gpr_set(int reg,double value,uint32_t raw) { if(reg)t_gpr[reg]=scalar_word(value,raw); }
double transform_memory16(uint32_t address) {
    TransformWord *p=transform_slot(address);
    TransformWord t=p?checked(*p,mem_r32(address&~3u)):transform_integer(mem_r32(address&~3u));
    return address&2?t.hi:t.lo;
}
double transform_memory32(uint32_t address) {
    TransformWord *p=transform_slot(address);uint32_t raw=mem_r32(address);
    return scalar(p?checked(*p,raw):transform_integer(raw));
}
void transform_memory_set16(uint32_t address,double value) {
    TransformWord *p=transform_slot(address);if(!p || !isfinite(value))return;
    *p=checked(*p,mem_r32(address&~3u));
    if(address&2)p->hi=value;else p->lo=value;
}
void transform_memory_set32(uint32_t address,double value) {
    TransformWord *p=transform_slot(address);if(p)*p=scalar_word(value,mem_r32(address));
}
double transform_ctrl(int reg,int half,uint32_t raw) {
    TransformWord t=checked(t_ctrl[reg],raw);return half?t.hi:t.lo;
}
double transform_ctrl32(int reg,uint32_t raw) {
    TransformWord t=checked(t_ctrl[reg],raw);
    return t.lo+((int16_t)raw<0?65536.0:0)+65536.0*t.hi;
}
double transform_data(int reg,int half,uint32_t raw) {
    TransformWord t=checked(t_data[reg],raw);return half?t.hi:t.lo;
}
void transform_result(int reg,double value,uint32_t raw) {
    if(!isfinite(value) || value < -32768 || value > 32767) value=(int16_t)raw;
    t_data[reg]=(TransformWord){value,(int16_t)(raw>>16),raw,true};
}
void transform_result32(int reg,double value,uint32_t raw) {
    if(!isfinite(value) || value<INT_MIN || value>INT_MAX) value=(int32_t)raw;
    t_data[reg]=(TransformWord){(int16_t)raw+value-(int32_t)raw,(int16_t)(raw>>16),raw,true};
}


void transform_instruction(uint32_t pc,uint32_t w) {
    unsigned op=w>>26;
    /* Reject unrelated opcodes before reading/copying register shadows. This
     * hook runs for every guest instruction; branches, byte I/O and most ALU
     * instructions cannot propagate a fractional matrix value here. */
    const uint64_t tracked=(UINT64_C(1)<<0)|(UINT64_C(1)<<9)|(UINT64_C(1)<<12)|
        (UINT64_C(1)<<13)|(UINT64_C(1)<<18)|(UINT64_C(1)<<33)|
        (UINT64_C(1)<<35)|(UINT64_C(1)<<37)|(UINT64_C(1)<<41)|
        (UINT64_C(1)<<43)|(UINT64_C(1)<<50)|(UINT64_C(1)<<58);
    if (!(tracked & (UINT64_C(1)<<op))) return;
    int rs=(w>>21)&31,rt=(w>>16)&31,rd=(w>>11)&31;
    if (op==0) {
        unsigned fn=w&63;
        const uint64_t alu=(UINT64_C(1)<<0)|(UINT64_C(1)<<3)|
            (UINT64_C(1)<<32)|(UINT64_C(1)<<33)|(UINT64_C(1)<<34)|
            (UINT64_C(1)<<35)|(UINT64_C(1)<<36)|(UINT64_C(1)<<37);
        if (!rd || !(alu & (UINT64_C(1)<<fn))) return;
    } else if ((op==9 || op==13) && ((!render_arithmetic(pc,w) && (w&65535)) || !rt)) return;
    else if (op==12 && (((w&65535)!=65535 && !(render_arithmetic(pc,w) && (w&65535)==4095)) || !rt)) return;
    else if (op==18 && (rs>6 || (rs&1))) return;
    uint32_t a=g_cpu->r[rs]+(int16_t)w,raw=g_cpu->r[rt];
    TransformWord t=rt?checked(t_gpr[rt],raw):transform_integer(0),*slot;
    if(op==0x12) {
        if(rs==0 && rt) t_gpr[rt]=checked(t_data[rd],gte_read_data(g_cpu,rd));
        else if(rs==2 && rt) {
            TransformWord src=checked(t_ctrl[rd],g_cpu->gte.ctrl[rd]);
            uint32_t value=gte_read_ctrl(g_cpu,rd);
            if(rd==4 || rd==12 || rd==20 || rd==26 || rd==27 || rd==29 || rd==30) {
                src.raw=value;src.hi=(int16_t)(value>>16);
            }
            t_gpr[rt]=src;
        }
        else if(rs==4) t_data[rd]=t;
        else if(rs==6) { if(rd==4) t.raw=raw&0xffff; t_ctrl[rd]=t; }
    } else if(op==0x32) {
        uint32_t word=mem_r32(a);slot=transform_slot(a);
        t_data[rt]=slot?checked(*slot,word):transform_integer(word);
    } else if(op==0x3a) {
        slot=transform_slot(a);if(slot) *slot=checked(t_data[rt],gte_read_data(g_cpu,rt));
    } else if(op==0x23 || op==0x21 || op==0x25) {
        uint32_t word=mem_r32(a&~3u);slot=transform_slot(a);
        t=slot?checked(*slot,word):transform_integer(word);
        if(op!=0x23) {
            double v=(a&2)?t.hi:t.lo;
            uint32_t h=mem_r16(a);
            if(op==0x21) h=(uint32_t)(int32_t)(int16_t)h;
            t=(TransformWord){v,(int16_t)(h>>16),h,true};
        }
        if(rt) t_gpr[rt]=t;
    } else if(op==0x2b || op==0x29) {
        slot=transform_slot(a);if(!slot)return;
        if(op==0x2b) *slot=t;
        else {
            uint32_t old=mem_r32(a&~3u);TransformWord out=checked(*slot,old);
            if(a&2) { out.hi=t.lo;out.raw=(old&0xffff)|(raw<<16); }
            else { out.lo=t.lo;out.raw=(old&0xffff0000)|(raw&0xffff); }
            *slot=out;
        }
    } else if(op==0 && (w&63)==3 && ((w>>6)&31)==12 && rd && rounded_position(pc)) {
        /* Model roots, ladder sections and viewmodel attachments convert
         * 20.12 coordinates to whole units. Keep the value before rounding
         * in the render shadow, removing the half-unit rounding bias. */
        uint32_t result=(uint32_t)((int32_t)raw>>12);
        t_gpr[rd]=scalar_word((scalar(t)-2048.0)/4096.0,result);
    } else if(op==0 && (w&63)==3 && ((w>>6)&31)==12 && rd &&
              (pc&0x1fffffff)>=0x30a60 && (pc&0x1fffffff)<=0x30a68) {
        t_gpr[rd]=scalar_word(scalar(t)/4096.0,(uint32_t)((int32_t)raw>>12));
    } else if(op==0 && ((w&63)==0x21 || (w&63)==0x20 || (w&63)==0x23 || (w&63)==0x22) && rd && render_arithmetic(pc,w)) {
        TransformWord a=checked(t_gpr[rs],g_cpu->r[rs]);
        bool subtract=(w&63)==0x23 || (w&63)==0x22;
        uint32_t result=subtract?g_cpu->r[rs]-raw:g_cpu->r[rs]+raw;
        t_gpr[rd]=scalar_word(scalar(a)+(subtract?-scalar(t):scalar(t)),result);
    } else if(op==0 && (w&63)==0x24 && rd) {
        /* Matrix column packing masks whole halfwords before OR. */
        TransformWord a=checked(t_gpr[rs],g_cpu->r[rs]);
        uint32_t ar=g_cpu->r[rs],result=ar&raw;TransformWord out=transform_integer(result);
        if((ar&0xffff)==0xffff && (raw&0xffff)==0xffff)out.lo=a.lo+t.lo+1;
        else if((ar&0xffff)==0xffff)out.lo=t.lo;else if((raw&0xffff)==0xffff)out.lo=a.lo;
        if((ar>>16)==0xffff && (raw>>16)==0xffff)out.hi=a.hi+t.hi+1;
        else if((ar>>16)==0xffff)out.hi=t.hi;else if((raw>>16)==0xffff)out.hi=a.hi;
        t_gpr[rd]=out;
    } else if(op==0 && (w&63)==0x21 && rd &&
              ((pc&0x1fffffff)==0x20664 || (pc&0x1fffffff)==0x20680 || (pc&0x1fffffff)==0x2069c)) {
        /* Animated model hierarchy: add the composed fractional translation
         * to the parent translation without quantizing it at each bone. */
        TransformWord a=checked(t_gpr[rs],g_cpu->r[rs]);
        double av=a.lo+((int16_t)a.raw<0?65536.0:0)+65536.0*a.hi;
        double bv=t.lo+((int16_t)t.raw<0?65536.0:0)+65536.0*t.hi;
        uint32_t result=g_cpu->r[rs]+raw;double exact=av+bv;
        if(exact<INT_MIN || exact>INT_MAX) t_gpr[rd]=transform_integer(result);
        else t_gpr[rd]=(TransformWord){(int16_t)result+exact-(int32_t)result,(int16_t)(result>>16),result,true};
    } else if(op==0 && (w&63)==0 && ((w>>6)&31)==4 && rd && root_position_unround(pc)) {
        /* Camera and animated-model hierarchies round a 20.12 root through SLL 4 / SRA 16.
         * Its render high lane retains the original root, without the +0.5
         * integer rounding bias. Gameplay still executes both original shifts. */
        uint32_t result=raw<<4;
        t_gpr[rd]=(TransformWord){(int16_t)result,(scalar(t)-2048.0)/4096.0,result,true};
    } else if(op==0 && (w&63)==0 && ((w>>6)&31)==12 && rd && camera_origin_fixed(pc)) {
        /* Camera attachment stores IR coordinates back as 20.12. Scaling the
         * integer IR alone would reintroduce a whole-world-unit camera step. */
        t_gpr[rd]=scalar_word(scalar(t)*4096.0,raw<<12);
    } else if(op==0 && (w&63)==0 && ((w>>6)&31)==16 && rd) {
        uint32_t result=raw<<16;
        t_gpr[rd]=(TransformWord){0,t.lo,result,true};
    } else if(op==0 && (w&63)==3 && ((w>>6)&31)==16 && rd) {
        uint32_t result=(uint32_t)((int32_t)raw>>16);
        t_gpr[rd]=(TransformWord){t.hi,(int16_t)(result>>16),result,true};
    } else if(op==9 && render_arithmetic(pc,w) && rt) {
        TransformWord src=checked(t_gpr[rs],g_cpu->r[rs]);
        t_gpr[rt]=scalar_word(scalar(src)+(int16_t)w,g_cpu->r[rs]+(int16_t)w);
    } else if(op==0xc && (w&0xffff)==4095 && render_arithmetic(pc,w) && rt) {
        TransformWord src=checked(t_gpr[rs],g_cpu->r[rs]);uint32_t result=g_cpu->r[rs]&4095;
        t_gpr[rt]=scalar_word(result+src.lo-(int16_t)src.raw,result);
    } else if(op==0xc && (w&0xffff)==0xffff && rt) {
        TransformWord src=checked(t_gpr[rs],g_cpu->r[rs]);
        t_gpr[rt]=(TransformWord){src.lo,0,g_cpu->r[rs]&0xffff,true};
    } else if(op==0 && (w&63)==0x22 && !rs && rd) {
        uint32_t result=0-raw;
        t_gpr[rd]=(TransformWord){-t.lo,(int16_t)(result>>16),result,true};
    } else if(op==0 && (w&63)==0x25 && rs && rt && rd) {
        TransformWord src=checked(t_gpr[rs],g_cpu->r[rs]);uint32_t a=g_cpu->r[rs];
        TransformWord out=transform_integer(a|raw);
        /* A packed lane can be integer zero yet carry a real fraction.
         * The other operand's shifted-out lane is exactly zero. */
        if(!(a&0xffff) && !(raw&0xffff))out.lo=src.lo+t.lo;
        else if(!(a&0xffff))out.lo=t.lo;else if(!(raw&0xffff))out.lo=src.lo;
        if(!(a&0xffff0000) && !(raw&0xffff0000))out.hi=src.hi+t.hi;
        else if(!(a&0xffff0000))out.hi=t.hi;else if(!(raw&0xffff0000))out.hi=src.hi;
        t_gpr[rd]=out;
    } else if(op==0 && ((w&63)==0x21 || (w&63)==0x25) && (rs==0)!=(rt==0)) {
        if(rd) t_gpr[rd]=checked(t_gpr[rs?rs:rt],g_cpu->r[rs?rs:rt]);
    } else if((op==9 || op==0xd) && !(w&0xffff) && rt) t_gpr[rt]=checked(t_gpr[rs],g_cpu->r[rs]);
}
void transform_commit(uint32_t pc,uint32_t w,int reg,uint32_t raw) {
    /* Invalid shadows stay invalid regardless of this instruction's result. */
    if (!t_gpr[reg].valid) return;
    unsigned op=w>>26,fn=w&63,rs=(w>>21)&31,rt=(w>>16)&31,sa=(w>>6)&31;
    bool known=(op==0 && ((fn==0 && sa==16) || (fn==3 && sa==16) ||
                 (fn==0x22 && !rs) || fn==0x24 || fn==0x25 || (fn==0x21 && (!rs || !rt)))) ||
               (op==0xc && (w&0xffff)==0xffff) ||
               ((op==9 || op==0xd) && !(w&0xffff));
    known|=op==0 && fn==0 && ((sa==4 && root_position_unround(pc)) || (sa==12 && camera_origin_fixed(pc)));
    known|=op==0 && fn==0x21 && ((pc&0x1fffffff)==0x20664 || (pc&0x1fffffff)==0x20680 || (pc&0x1fffffff)==0x2069c);
    known|=op==0 && fn==3 && sa==12 && rounded_position(pc);
    known|=op==0 && (fn==0x20 || fn==0x21 || fn==0x22 || fn==0x23) && render_arithmetic(pc,w);
    known|=op==9 && render_arithmetic(pc,w);
    known|=op==12 && (w&65535)==4095 && render_arithmetic(pc,w);
    known|=op==0 && fn==3 && sa==12 && (pc&0x1fffffff)>=0x30a60 && (pc&0x1fffffff)<=0x30a68;
    if(!known || t_gpr[reg].raw!=raw) t_gpr[reg].valid=false;
}

void transform_rotation(uint32_t out,double x,double y,double z,bool model) {
    const double k=6.283185307179586476925286766559/4096.0;
    double sx=sin(x*k),cx=cos(x*k),sy=sin(y*k),cy=cos(y*k),sz=sin(z*k),cz=cos(z*k);
    double m[9];
    if(model) {
        double a[9]={cy*cx,sy*sz-cy*sx*cz,sy*cz+cy*sx*sz,
                     sx,cx*cz,-cx*sz,-sy*cx,cy*sz+sy*sx*cz,cy*cz-sy*sx*sz};
        memcpy(m,a,sizeof m);
    } else {
        double a[9]={cy*cz+sy*sx*sz,sy*sx*cz-cy*sz,sy*cx,
                     cx*sz,cx*cz,-sx,cy*sx*sz-sy*cz,sy*sz+cy*sx*cz,cy*cx};
        memcpy(m,a,sizeof m);
    }
    for(int i=0;i<9;i++)transform_memory_set16(out+2*i,m[i]*4096.0);
}
