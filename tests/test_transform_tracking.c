#include "psx.h"
#include "pgxp.h"
#include <assert.h>
#include <math.h>
void bus_write16(uint32_t a,uint32_t v){(void)a;(void)v;}
int main(void) {
    CPU c={0};g_cpu=&c;g_pgxp=true;pgxp_reset();
    mem_w32(0x2000,122);transform_memory_set32(0x2000,122.375);
    c.r[4]=0x2000;
    transform_instruction(0,0x8c880000);c.r[8]=122;
    assert(fabs(transform_gpr32(8,c.r[8])-122.375)<1e-10);
    transform_instruction(0,0x48c80000);gte_write_ctrl(&c,0,c.r[8]);
    assert(fabs(transform_ctrl(0,0,c.gte.ctrl[0])-122.375)<1e-10);
    PgxpRegisters saved;pgxp_registers_save(&saved);
    transform_gpr_set(8,0,c.r[8]);pgxp_registers_restore(&saved);
    assert(fabs(transform_gpr32(8,c.r[8])-122.375)<1e-10);
    assert(transform_ctrl(0,0,123)==123); /* stale raw value */

    pgxp_reset();memset(&c,0,sizeof c);
    c.gte.ctrl[0]=c.gte.ctrl[2]=c.gte.ctrl[4]=4096;
    c.gte.data[0]=100;c.gte.data[1]=1024;c.gte.ctrl[26]=384;
    transform_result(0,100.375,c.gte.data[0]);
    CPU integer=c;g_pgxp=false;gte_command(&integer,0x280001);
    g_pgxp=true;gte_command(&c,0x280001);
    assert(!memcmp(&integer.gte,&c.gte,sizeof c.gte));
    PgxpVtx p;assert(pgxp_lookup(0,0,&p)==false);
    pgxp_reset();assert(transform_memory32(0x2000)==122);
    puts("Transform tracking: fractional memory/register/matrix propagation, interrupt snapshot, stale checks and integer GTE parity passed.");
    return 0;
}
