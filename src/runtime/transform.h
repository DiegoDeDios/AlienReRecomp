#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct TransformWord { double lo,hi;uint32_t raw;bool valid; } TransformWord;
typedef struct TransformRegisters { TransformWord gpr[32],data[32],ctrl[32]; } TransformRegisters;
void transform_reset(void);
void transform_registers_save(TransformRegisters *p);
void transform_registers_restore(const TransformRegisters *p);
double transform_ctrl(int reg,int half,uint32_t raw);
double transform_ctrl32(int reg,uint32_t raw);
double transform_data(int reg,int half,uint32_t raw);
void transform_result(int reg,double value,uint32_t raw);
void transform_result32(int reg,double value,uint32_t raw);
void transform_instruction(uint32_t pc,uint32_t instruction);
void transform_commit(uint32_t pc,uint32_t instruction,int reg,uint32_t raw);
double transform_gpr32(int reg,uint32_t raw);
void transform_gpr_set(int reg,double value,uint32_t raw);
double transform_memory16(uint32_t address);
double transform_memory32(uint32_t address);
void transform_memory_set16(uint32_t address,double value);
void transform_memory_set32(uint32_t address,double value);
