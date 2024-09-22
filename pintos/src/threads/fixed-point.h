#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

static int f = 16384; //参考pintos中BSD调度p.q定点格式来以定点数定义实数其中p=17，q=14故f=2**14
typedef int fixed_point;//为int取别名fixed_point用于将int类型的值转换为定点数

//将int类型的值转换为fixed_point
static inline fixed_point itof(int n) {
  return n * f;
}
//方法1：将fixed_point类型的值转换为int
static inline int ftoi(fixed_point x) {
  return x / f;
}
//方法2：将fixed_point类型的值转换为int
static inline int ftoi_round(fixed_point x) {
  return x >= 0 ? (x + f / 2) / f : (x - f / 2) / f;
}
//将fixed_point类型的数与int类型的数相加
static inline fixed_point add_fi(fixed_point x, int n) {
  return x + n * f;
}
//将fixed_point类型的数与int类型的数相减
static inline fixed_point sub_fi(fixed_point x, int n) {
  return x - n * f;
}
//将fixed_point类型的数与fixed_point类型的数相乘
static inline fixed_point multiply_ff(fixed_point x, fixed_point y) {
  return ((int64_t)x) * y / f;
}
//将fixed_point类型的数与fixed_point类型的数相除
static inline fixed_point divide_ff(fixed_point x, fixed_point y) {
  return ((int64_t)x) * f / y;
}

#endif
