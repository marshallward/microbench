#ifndef AXPY_H_
#define AXPY_H_

#include "bench.h"

void * axpy_main(void *);
double roof_copy(float, float, float *, float *, int, double *, double);
double roof_ax(float, float, float *, float *, int, double *, double);
double roof_xpy(float, float, float *, float *, int, double *, double);
double roof_axpy(float, float, float *, float *, int, double *, double);
double roof_axpby(float, float, float *, float *, int, double *, double);

#endif
