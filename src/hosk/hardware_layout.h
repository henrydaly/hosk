#ifndef HARDWARE_LAYOUT_H
#define HARDWARE_LAYOUT_H

#define GNU_SOURCE
#include <iostream>
#include <stdio.h>
#include <string>
#include <sstream>
FILE *popen(const char *command, const char *mode);
int pclose(FILE *stream);

#define THREADS_PER_CORE 2

struct core_t {
   int hwthread_id[THREADS_PER_CORE];
};

struct socket_t {
   core_t*  cores;
};

struct hardware_layout_t {
   socket_t*   sockets;
   int         num_sockets;
   int         cores_per_socket;
   int         max_cpu_num;
};
typedef hardware_layout_t hl_t;


// Public hardware layout interface
hl_t* get_hardware_layout(void);
void free_hardware_layout(hl_t* m);
void print_hardware_layout(hl_t* m);

#endif //HARDWARE_LAYOUT_H
