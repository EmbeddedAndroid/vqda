// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * vqda-ping [n]: time n no-op round trips guest -> vqda-back -> guest
 * (VQDA_OP_PING: the back end answers without touching a DSP). Prints
 * min, p50, p90, p99 and max in microseconds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int vqda_ping(void);

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 2000, i, e;
	double *t = calloc(n, sizeof(*t));
	struct timespec a, b;

	if (n < 10 || !t)
		return 2;
	e = vqda_ping();
	if (e) {
		fprintf(stderr, "vqda-ping: %d\n", e);
		return 1;
	}
	for (i = 0; i < n; i++) {
		clock_gettime(CLOCK_MONOTONIC, &a);
		e = vqda_ping();
		clock_gettime(CLOCK_MONOTONIC, &b);
		if (e)
			return 1;
		t[i] = (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3;
	}
	qsort(t, n, sizeof(*t), cmp);
	printf("VQDAPING n=%d us min=%.1f p50=%.1f p90=%.1f p99=%.1f max=%.1f\n",
	       n, t[0], t[n / 2], t[n * 9 / 10], t[n * 99 / 100], t[n - 1]);
	return 0;
}
