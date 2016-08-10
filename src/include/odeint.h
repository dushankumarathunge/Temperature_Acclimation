#ifndef ODEINT_H
#define ODEINT_H

#include "gday.h"

void odeint(float [], int, float, float, float, float, float, int *, int *,
            double, double, double,
        	/*void (*derivs)(float, float [], float []),*/
        	void (*derivs)(float, float *, float *, double, double, double),
        	void (*rkqs)(float [], float [], int, float *, float, float, float [],
        	float *, float *, void (*)(float, float [], float [])));

double zbrent(double (*func) (double, double, double, double, double, double),
              double, double, double, double, double, double, double, double);

#endif /* ODEINT_H */
