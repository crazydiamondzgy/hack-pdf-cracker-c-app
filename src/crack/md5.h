#ifndef _MD5_H_
#define _MD5_H_

#include "common.h"

void 
md5(const uint8_t *msg, const unsigned int msgLen, uint8_t *digest);

void
md5_50(uint8_t *msg);

#endif /** _MD5_H_ */
