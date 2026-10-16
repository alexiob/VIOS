#ifndef VIOS_WORKBENCH_RUNTIME_H
#define VIOS_WORKBENCH_RUNTIME_H
int vios_workbench_runtime_init(void);
int vios_workbench_command(const char *word,const char **rest);
void vios_workbench_idle(int changed);
#endif
