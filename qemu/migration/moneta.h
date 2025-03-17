#ifndef MONETA_H
#define MONETA_H

extern bool post_rehost_tick;

int moneta_savevm(void);
int moneta_savevm_post_rehost(void);
bool moneta_loadvm_tick(void);
bool moneta_loadvm_check(void);
bool moneta_post_rehost_check(void);
#endif
