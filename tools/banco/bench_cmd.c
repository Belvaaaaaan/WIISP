/* Banco: el bucle de comandos de ge.c con devkitPPC */
#include "gpu/ge.c"
void bench_run_list(u32 pc, u32 end);
void bench_run_list(u32 pc, u32 end){
	DisplayList *l = &dls[0];
	memset(l, 0, sizeof(*l));
	l->pc = pc; l->stall = end; l->state = DL_RUNNING;
	current_list = l;
	gpu_state = GPU_RUNNING;
	if(opt_on[OPT_GE_CMD_PUNTERO]) run_commands_ptr(l);
	else while(gpu_state == GPU_RUNNING){
		if(l->pc == l->stall){ gpu_state = GPU_STALL; break; }
		if(!mem_valid(l->pc, 4)){ gpu_state = GPU_ERROR; break; }
		execute(l, mem_read32(l->pc));
		l->pc += 4;
	}
	current_list = NULL;
}
