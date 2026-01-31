#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

/* 고정 소수점 연산을 위한 헤더 파일 (17.14 format)
  p: 17 bits (정수부), q: 14 bits (소수부)
*/

typedef int fixed_point_t;

// 소수점 비트 수  : 14
#define F (1 << 14)
#define CONVERT_TO_FP(n) ((n) * (F))
#define CONVERT_TO_INT_ZERO(x) ((x) / (F))
#define CONVERT_TO_INT_NEAREST(x) ((x) >= 0 ? ((x) + (F) / 2) / (F) : ((x) - (F) / 2) / (F))
#define ADD_FP(x, y) ((x) + (y))
#define SUB_FP(x, y) ((x) - (y))
#define ADD_INT(x, n) ((x) + (n) * (F))
#define SUB_INT(x, n) ((x) - (n) * (F))
#define MUL_FP(x, y) ((((int64_t)(x)) * (y)) / (F))
#define MUL_INT(x, n) ((x) * (n))
#define DIV_FP(x, y) ((((int64_t)(x)) * (F)) / (y))
#define DIV_INT(x, n) ((x) / (n))

#endif /* threads/fixedpointarith.h */